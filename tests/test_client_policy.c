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

static bool policy_never(void *ud, const sse_error_t *e) {
  (void)ud; (void)e;
  return false;
}
static bool policy_always(void *ud, const sse_error_t *e) {
  (void)ud;
  cadd("hook(%d,%d,%d)\n", (int)e->reason, e->http_status, (int)e->will_retry);
  return true;
}
/* Closes the client from inside the hook, then asks for a retry anyway:
 * CLOSED must stay final. */
static bool policy_close_then_retry(void *ud, const sse_error_t *e) {
  (void)ud;
  (void)e;
  cadd("hook\n");
  sse_client_close(&cl);
  return true;
}

int main(void) {
  /* 204: SERVER_STOP, no retry, closed */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 204, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(3,204,0,0)\nclosed\n");
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
  }

  /* 404: stops permanently */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 404, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,404,0,0)\nclosed\n");
  }

  /* 401: stops permanently */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 401, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,401,0,0)\nclosed\n");
  }

  /* 404 + Retry-After: retries with the header's delay */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 404, "", NULL, SSE_READ_EOF);
  mock.conns[0].retry_after_s = 3;
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,404,1,3000)\n") != NULL);
  }

  /* 429 without Retry-After: still retries */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 429, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,429,1,3000)\n") != NULL);
  }

  /* 500: retries */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 500, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(1,500,1,3000)\n") != NULL);
  }

  /* wrong content type on 200: retries (captive-portal case) */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 200, "text/html", "<html>portal</html>", SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "err(2,200,1,3000)\n") != NULL);
  }

  /* skip_content_type_check: text/html is accepted */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 200, "text/html", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.skip_content_type_check = true;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(10);
    OK(strstr(clog, "msg(message,,x)\n") != NULL);
  }

  /* hook forces stop on a retryable error */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 500, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.reconnect_policy = policy_never;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "err(1,500,0,0)\nclosed\n");
  }

  /* hook forces retry on a non-retryable 401, sees the default decision */
  fresh();
  mock.n_conns = 2;
  sse_conn(0, 401, "", NULL, SSE_READ_EOF);
  sse_conn(1, 200, "text/event-stream", "data: x\n\n", SSE_READ_TIMEOUT);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.reconnect_policy = policy_always;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK(strstr(clog, "hook(1,401,0)\nerr(1,401,1,3000)\n") != NULL);
    g_now += 3001;
    pump(10);
    OK(strstr(clog, "open(1)\n") != NULL);
  }

  /* a hook that closes the client and returns true must not resurrect it:
   * on_closed stays the final signal, no error or reconnect afterwards */
  fresh();
  mock.n_conns = 1;
  sse_conn(0, 500, "", NULL, SSE_READ_EOF);
  {
    sse_client_config_t cfg = base_cfg();
    cfg.reconnect_policy = policy_close_then_retry;
    OK_INT(sse_client_init(&cl, &cfg), 0);
    pump(5);
    OK_STR(clog, "hook\nclosed\n");
    OK_INT(sse_client_state(&cl), SSE_STATE_CLOSED);
    OK(sse_client_poll(&cl) == UINT32_MAX);
    OK_INT(mock.open_calls, 1);
  }

  T_END();
}
