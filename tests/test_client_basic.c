#include "eventsource/sse_client.h"
#include "mock_transport.h"
#include "t.h"
#include <stdarg.h>

static char clog[4096];
static size_t clog_len;
static void cadd(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(clog + clog_len, sizeof clog - clog_len, fmt, ap);
  va_end(ap);
  if (n > 0) clog_len += (size_t)n;
}
static void c_open(void *ud, unsigned rc) { (void)ud; cadd("open(%u)\n", rc); }
static void c_msg(void *ud, const sse_message_t *m) {
  (void)ud;
  cadd("msg(%s,%s,%s)\n", m->event, m->last_event_id, m->data);
}
static void c_err(void *ud, const sse_error_t *e) {
  (void)ud;
  cadd("err(%d,%d,%d,%u)\n", (int)e->reason, e->http_status, (int)e->will_retry,
       (unsigned)e->retry_in_ms);
}
static void c_closed(void *ud) { (void)ud; cadd("closed\n"); }

static char db[1024], ib[128], eb[64];
static uint8_t rx[256];
static sse_client_t cl;

static sse_client_config_t base_cfg(void) {
  sse_client_config_t cfg = {0};
  cfg.url = "http://test.local/stream";
  cfg.buffers.data_buf = db; cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;   cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx; cfg.rx_buf_len = sizeof rx;
  cfg.transport = mock_transport();
  cfg.now_ms = fake_now;
  cfg.callbacks.on_open = c_open;
  cfg.callbacks.on_message = c_msg;
  cfg.callbacks.on_error = c_err;
  cfg.callbacks.on_closed = c_closed;
  return cfg;
}

static void fresh(void) {
  clog[0] = 0; clog_len = 0;
  mock_reset();
}

/* Poll until the client would sleep or is closed, at most `n` times. */
static void pump(int n) {
  while (n-- > 0) {
    uint32_t s = sse_client_poll(&cl);
    if (s != 0) break;
  }
}

int main(void) {
  /* invalid configs are rejected */
  {
    sse_client_config_t cfg = {0};
    OK_INT(sse_client_init(&cl, &cfg), -1);
    cfg = base_cfg();
    cfg.url = NULL;
    OK_INT(sse_client_init(&cl, &cfg), -1);
  }

  /* happy path: connect, open, one message, default event type */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "text/event-stream";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].chunks[0] = "data: hello\n\n";
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    OK_INT(sse_client_state(&cl), SSE_STATE_IDLE);
    pump(10);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
    OK_STR(clog, "open(0)\nmsg(message,,hello)\n");
    /* request headers were composed; no Last-Event-ID on first connect */
    OK(strstr(mock.captured_headers, "Accept: text/event-stream|") != NULL);
    OK(strstr(mock.captured_headers, "Cache-Control: no-store|") != NULL);
    OK(strstr(mock.captured_headers, "Last-Event-ID") == NULL);
  }

  /* named events, id tracking, extra headers, content-type with params */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "Text/Event-Stream; charset=utf-8";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].chunks[0] = "id: 41\nevent: tick\ndata: a\n\n";
  mock.conns[0].chunks[1] = "data: b\n\n";
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    static const char *extra[] = {"Authorization: Bearer xyz", NULL};
    sse_client_config_t cfg = base_cfg();
    cfg.extra_headers = extra;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    /* lastEventId persists into the second message even without an id field */
    OK_STR(clog, "open(0)\nmsg(tick,41,a)\nmsg(message,41,b)\n");
    OK_STR(sse_client_last_event_id(&cl), "41");
    OK(strstr(mock.captured_headers, "Authorization: Bearer xyz|") != NULL);
  }

  /* close(): transport closed, on_closed fired once, poll returns UINT32_MAX */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].status = 200;
  mock.conns[0].content_type = "text/event-stream";
  mock.conns[0].retry_after_s = -1;
  mock.conns[0].tail = SSE_READ_TIMEOUT;
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(3);
    sse_client_close(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
    OK_INT(mock.close_calls >= 1, 1);
    OK(sse_client_poll(&cl) == UINT32_MAX);
    sse_client_close(&cl); /* idempotent */
    OK(strstr(clog, "closed\n") != NULL);
    OK(strstr(clog, "closed\nclosed") == NULL);
  }

  T_END();
}
