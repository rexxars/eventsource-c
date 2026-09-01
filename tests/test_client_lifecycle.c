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
  /* idle timeout: silent socket for > idle_timeout_ms forces reconnect */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  sse_conn(1, 200, "text/event-stream", "data: y\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.idle_timeout_ms = 60000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "msg(message,,x)\n") != NULL);
    /* below threshold: still open */
    g_now += 59999;
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
    /* cross threshold */
    g_now += 2;
    sse_client_poll(&cl);
    OK(strstr(clog, "err(4,0,1,") != NULL); /* 4 == SSE_ERR_IDLE_TIMEOUT */
    OK_INT(sse_client_state(&cl), SSE_STATE_WAITING_RETRY);
    g_now += 3001;
    pump(10);
    OK(strstr(clog, "msg(message,,y)\n") != NULL);
  }

  /* idle timeout disabled (0): a silent socket stays open forever */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    g_now += 100000000u;
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* received bytes reset the idle timer (heartbeat comments count) */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  mock.conns[0].chunks[1] = ": keepalive\n\n";
  {
    sse_client_config_t cfg = base_cfg();
    cfg.idle_timeout_ms = 60000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(2); /* consume chunk 0 only: poll(connect)=0, poll(read chunk0)=0 */
    g_now += 59000;
    pump(1); /* delivers the heartbeat chunk, resetting the timer */
    g_now += 59000; /* 118000 total > 60000, but only 59000 since heartbeat */
    sse_client_poll(&cl);
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* oversized message: informational error, stream continues */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", NULL, SSE_READ_TIMEOUT);
  {
    static char big[2048];
    memset(big, 'a', sizeof big);
    memcpy(big, "data: ", 6);
    memcpy(big + sizeof big - 12, "\n\ndata: k\n\n", 12); /* includes NUL */
    mock.conns[0].chunks[0] = big;
    sse_client_config_t cfg = base_cfg(); /* data_buf is 1024: overflow */
    cfg.rx_buf_len = 256; /* mock caps chunk to rx len; multiple reads */
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(20);
    OK(strstr(clog, "err(5,0,1,0)\n") != NULL); /* 5 == SSE_ERR_MESSAGE_TOO_LARGE */
    OK(strstr(clog, "msg(message,,k)\n") != NULL); /* resynced */
    OK_INT(sse_client_state(&cl), SSE_STATE_OPEN);
  }

  /* request_stop from "another task": next poll closes cleanly */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    sse_client_request_stop(&cl);
    OK(sse_client_poll(&cl) == UINT32_MAX);
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
    OK(strstr(clog, "closed\n") != NULL);
    OK_INT(mock.close_calls >= 1, 1);
  }

  /* WAITING_RETRY poll returns the remaining delay as a sleep hint */
  fresh();
  mock.n_conns = 2;
  mock.conns[0].open_result = -1;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.default_retry_ms = 2000;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    uint32_t hint = sse_client_poll(&cl);
    OK(hint > 0 && hint <= 2000);
    g_now += 500;
    uint32_t hint2 = sse_client_poll(&cl);
    OK(hint2 > 0 && hint2 <= 1500);
  }

  T_END();
}
