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

static void fresh(void) { clog[0] = 0; clog_len = 0; mock_reset(); }

static void pump(int n) {
  while (n-- > 0) {
    if (sse_client_poll(&cl) != 0) break;
  }
}

static void sse_conn(int i, int status, const char *ct, const char *body, int tail) {
  mock.conns[i].status = status;
  mock.conns[i].content_type = ct;
  mock.conns[i].retry_after_s = -1;
  mock.conns[i].chunks[0] = body;
  mock.conns[i].tail = tail;
}

int main(void) {
  /* EOF -> WAITING_RETRY with flat default delay (3000), then reconnect;
   * reconnect_count 1; Last-Event-ID header sent; parser reset between
   * connections (partial event must not leak). */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "id: 42\ndata: a\n\ndata: par", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: tial\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg(); /* max_retry_ms 0 -> flat 3000 */
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK_INT(sse_client_state(&cl), SSE_STATE_WAITING_RETRY);
    OK_STR(clog, "open(0)\nmsg(message,42,a)\nerr(0,0,1,3000)\n");
    /* not yet due */
    OK(sse_client_poll(&cl) > 0);
    OK_INT(mock.open_calls, 1);
    /* due: advance the fake clock past the deadline */
    g_now += 3001;
    pump(10);
    OK_INT(mock.open_calls, 2);
    OK(strstr(mock.captured_headers, "Last-Event-ID: 42|") != NULL);
    /* partial "par" from conn 0 must NOT prefix "tial" */
    OK(strstr(clog, "open(1)\nmsg(message,42,tial)\n") != NULL);
    OK_STR(sse_client_last_event_id(&cl), "42");
  }

  /* server-sent retry: overrides the base delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "retry: 10\ndata: x\n\n", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: y\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "err(0,0,1,10)\n") != NULL);
  }

  /* backoff: two failed attempts double the delay; message resets counter.
   * conn0 connect-error, conn1 connect-error, conn2 delivers. base=100. */
  fresh();
  mock.n_conns = 3;
  mock.conns[0].open_result = -1;
  mock.conns[1].open_result = -1;
  sse_conn(2, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 100;
    cfg.max_retry_ms = 30000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5); /* attempt 0 fails: delay 100 << 0 = 100 */
    OK(strstr(clog, "err(0,0,1,100)\n") != NULL);
    g_now += 101;
    pump(5); /* attempt 1 fails: delay 100 << 1 = 200 */
    OK(strstr(clog, "err(0,0,1,200)\n") != NULL);
    g_now += 201;
    pump(10);
    OK(strstr(clog, "open(2)\nmsg(message,,x)\n") != NULL);
  }

  /* backoff cap: max_retry_ms bounds the doubled delay */
  fresh();
  mock.n_conns = 2;
  mock.conns[0].open_result = -1;
  mock.conns[1].open_result = -1;
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 100;
    cfg.max_retry_ms = 150;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    g_now += 101;
    pump(5); /* second attempt: min(200, 150) = 150 */
    OK(strstr(clog, "err(0,0,1,150)\n") != NULL);
  }

  /* Retry-After overrides computed delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 503, "", NULL, SSE_READ_EOF);
  mock.conns[0].retry_after_s = 7;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,503,1,7000)\n") != NULL);
    g_now += 7001;
    pump(10);
    OK(strstr(clog, "open(1)\n") != NULL);
  }

  /* jitter: delay lands in [base, base + base*pct/100] */
  fresh();
  mock.n_conns = 1;
  mock.conns[0].open_result = -1;
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 1000;
    cfg.jitter_pct = 10;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    unsigned d = 0;
    OK_INT(sscanf(clog, "err(0,0,1,%u)", &d), 1);
    OK(d >= 1000 && d <= 1100);
  }

  T_END();
}
