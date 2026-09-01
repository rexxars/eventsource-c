#include "eventsource/sse_client.h"
#include "sse_clock_posix.h"
#include "sse_transport_curl.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static char db[8192], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  printf("* open (reconnect_count=%u)\n", rc);
}
static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  printf("[%s] (id=%s) %.*s\n", m->event, m->last_event_id, (int)m->data_len, m->data);
}
static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  printf("* error reason=%d status=%d will_retry=%d retry_in=%ums\n", (int)e->reason,
         e->http_status, (int)e->will_retry, (unsigned)e->retry_in_ms);
}
static void on_closed(void *ud) {
  (void)ud;
  printf("* closed\n");
}
static void on_sigint(int sig) {
  (void)sig;
  sse_client_request_stop(&client);
}

int main(int argc, char **argv) {
  const char *url = argc > 1 ? argv[1] : "http://127.0.0.1:8080/stream";
  sse_transport_t *tr = sse_transport_curl_new();
  if (!tr) return 1;

  sse_client_config_t cfg = {0};
  cfg.url = url;
  cfg.buffers.data_buf = db; cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;   cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx; cfg.rx_buf_len = sizeof rx;
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;
  cfg.idle_timeout_ms = 60000;
  cfg.transport = tr;
  cfg.now_ms = sse_now_ms_posix;
  cfg.callbacks.on_open = on_open;
  cfg.callbacks.on_message = on_message;
  cfg.callbacks.on_error = on_error;
  cfg.callbacks.on_closed = on_closed;

  if (sse_client_init(&client, &cfg) != 0) {
    fprintf(stderr, "invalid config\n");
    return 1;
  }
  signal(SIGINT, on_sigint);

  for (;;) {
    uint32_t s = sse_client_poll(&client);
    if (s == UINT32_MAX) break;
    if (s > 0) usleep((s > 250 ? 250 : s) * 1000u); /* stay responsive to ^C */
  }
  sse_transport_curl_free(tr);
  return 0;
}
