#include "sse_transport_esp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

typedef struct {
  esp_http_client_handle_t hc;
  char content_type[64];
  int32_t retry_after_s;
  uint32_t configured_timeout_ms;
} esp_ctx_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  esp_ctx_t *t = evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (strcasecmp(evt->header_key, "content-type") == 0) {
      snprintf(t->content_type, sizeof t->content_type, "%s", evt->header_value);
    } else if (strcasecmp(evt->header_key, "retry-after") == 0) {
      /* evt->header_value is NUL-terminated. Only parse delta-seconds
       * (leading ASCII digit); HTTP-date form is left as absent, matching
       * the curl port's on_header. */
      const char *p = evt->header_value;
      while (*p == ' ') p++;
      if (*p >= '0' && *p <= '9') {
        long v = strtol(p, NULL, 10);
        if (v >= 0) t->retry_after_s = (int32_t)v;
      }
    }
  }
  return ESP_OK;
}

static void esp_teardown(esp_ctx_t *t) {
  if (t->hc) {
    esp_http_client_close(t->hc);
    esp_http_client_cleanup(t->hc);
    t->hc = NULL;
  }
}

static int esp_open_fn(void *vctx, const sse_request_t *req, sse_response_info_t *out) {
  esp_ctx_t *t = vctx;
  esp_teardown(t);
  t->content_type[0] = '\0';
  t->retry_after_s = -1;

  esp_http_client_config_t cfg = {
      .url = req->url,
      .method = HTTP_METHOD_GET,
      .event_handler = on_http_event,
      .user_data = t,
      .timeout_ms = 10000, /* connect + header timeout; reads adjust below */
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 1024,
  };
  t->hc = esp_http_client_init(&cfg);
  if (!t->hc) return -1;

  for (const char *const *h = req->headers; h && *h; h++) {
    const char *colon = strchr(*h, ':');
    if (!colon) continue;
    char name[64];
    size_t nl = (size_t)(colon - *h);
    if (nl >= sizeof name) continue;
    memcpy(name, *h, nl);
    name[nl] = '\0';
    const char *val = colon + 1;
    while (*val == ' ') val++;
    esp_http_client_set_header(t->hc, name, val);
  }

  if (esp_http_client_open(t->hc, 0) != ESP_OK) {
    esp_teardown(t);
    return -1;
  }
  if (esp_http_client_fetch_headers(t->hc) < 0) {
    esp_teardown(t);
    return -1;
  }
  out->status_code = esp_http_client_get_status_code(t->hc);
  snprintf(out->content_type, sizeof out->content_type, "%s", t->content_type);
  out->retry_after_s = t->retry_after_s;
  t->configured_timeout_ms = 0; /* force set_timeout on first read */
  return 0;
}

static int esp_read_fn(void *vctx, void *buf, size_t len, uint32_t timeout_ms) {
  esp_ctx_t *t = vctx;
  if (!t->hc) return SSE_READ_ERROR;
  if (timeout_ms != t->configured_timeout_ms) {
    esp_http_client_set_timeout_ms(t->hc, (int)timeout_ms);
    t->configured_timeout_ms = timeout_ms;
  }
  errno = 0;
  int r = esp_http_client_read(t->hc, buf, (int)len);
  if (r > 0) return r;
  if (r == 0) {
    /* 0 can mean EOF or (for some transports) a poll timeout. Chunked SSE
     * streams report completion explicitly; trust that first. */
    return esp_http_client_is_complete_data_received(t->hc) ? SSE_READ_EOF
                                                            : SSE_READ_TIMEOUT;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
    return SSE_READ_TIMEOUT;
  }
  return SSE_READ_ERROR;
}

static void esp_close_fn(void *vctx) { esp_teardown(vctx); }

sse_transport_t *sse_transport_esp_http_client(void) {
  sse_transport_t *tr = calloc(1, sizeof *tr);
  esp_ctx_t *ctx = calloc(1, sizeof *ctx);
  if (!tr || !ctx) {
    free(tr);
    free(ctx);
    return NULL;
  }
  ctx->retry_after_s = -1;
  tr->ctx = ctx;
  tr->open = esp_open_fn;
  tr->read = esp_read_fn;
  tr->close = esp_close_fn;
  return tr;
}

void sse_transport_esp_http_client_free(sse_transport_t *t) {
  if (!t) return;
  esp_teardown(t->ctx);
  free(t->ctx);
  free(t);
}
