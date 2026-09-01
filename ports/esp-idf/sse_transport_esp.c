#include "sse_transport_esp.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

/* The manual open/fetch_headers flow does not auto-follow redirects (only
 * esp_http_client_perform does), so this transport follows them itself,
 * bounded to avoid redirect loops. */
#define ESP_MAX_REDIRECT_HOPS 5

typedef struct {
  esp_http_client_handle_t hc;
  char content_type[64];
  char location[512]; /* Location header of the current response, if any */
  char url[512];      /* URL of the current request, for origin comparison */
  int32_t retry_after_s;
  uint32_t configured_timeout_ms;
} esp_ctx_t;

/* Extract lowercase scheme, host (brackets kept for IPv6 literals), and
 * default-resolved port from an absolute http(s) URL. Returns false on
 * anything unparseable or non-http(s). */
static bool origin_of(const char *url, char *scheme, size_t scheme_cap,
                      char *host, size_t host_cap, int *port) {
  const char *sep = strstr(url, "://");
  if (!sep) return false;
  size_t slen = (size_t)(sep - url);
  if (slen == 0 || slen >= scheme_cap) return false;
  for (size_t i = 0; i < slen; i++) {
    char ch = url[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    scheme[i] = ch;
  }
  scheme[slen] = '\0';

  const char *h = sep + 3;
  const char *end = h;
  const char *at = NULL;
  while (*end && *end != '/' && *end != '?' && *end != '#') {
    if (*end == '@') at = end; /* userinfo */
    end++;
  }
  if (at) h = at + 1;
  if (h >= end) return false;

  const char *host_end = end;
  const char *port_str = NULL;
  if (*h == '[') { /* IPv6 literal */
    const char *close = memchr(h, ']', (size_t)(end - h));
    if (!close) return false;
    host_end = close + 1;
    if (host_end < end && *host_end == ':') port_str = host_end + 1;
  } else {
    const char *colon = memchr(h, ':', (size_t)(end - h));
    if (colon) {
      host_end = colon;
      port_str = colon + 1;
    }
  }
  size_t hlen = (size_t)(host_end - h);
  if (hlen == 0 || hlen >= host_cap) return false;
  for (size_t i = 0; i < hlen; i++) {
    char ch = h[i];
    if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
    host[i] = ch;
  }
  host[hlen] = '\0';

  if (port_str) {
    if (port_str >= end) return false;
    int p = 0;
    for (const char *q = port_str; q < end; q++) {
      if (*q < '0' || *q > '9') return false;
      p = p * 10 + (*q - '0');
      if (p > 65535) return false;
    }
    *port = p;
  } else if (strcmp(scheme, "http") == 0) {
    *port = 80;
  } else if (strcmp(scheme, "https") == 0) {
    *port = 443;
  } else {
    return false;
  }
  return true;
}

static bool same_origin_urls(const char *a, const char *b) {
  char sa[8], sb[8], ha[256], hb[256];
  int pa, pb;
  if (!origin_of(a, sa, sizeof sa, ha, sizeof ha, &pa)) return false;
  if (!origin_of(b, sb, sizeof sb, hb, sizeof hb, &pb)) return false;
  return strcmp(sa, sb) == 0 && strcmp(ha, hb) == 0 && pa == pb;
}

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  esp_ctx_t *t = evt->user_data;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (strcasecmp(evt->header_key, "content-type") == 0) {
      snprintf(t->content_type, sizeof t->content_type, "%s", evt->header_value);
    } else if (strcasecmp(evt->header_key, "location") == 0) {
      snprintf(t->location, sizeof t->location, "%s", evt->header_value);
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
  t->location[0] = '\0';
  t->retry_after_s = -1;
  snprintf(t->url, sizeof t->url, "%s", req->url);

  esp_http_client_config_t cfg = {
      .url = req->url,
      .method = HTTP_METHOD_GET,
      .event_handler = on_http_event,
      .user_data = t,
      .timeout_ms = 10000, /* connect + header timeout; reads adjust below */
      .crt_bundle_attach = esp_crt_bundle_attach,
      .buffer_size = 1024,
      .disable_auto_redirect = true, /* this transport follows redirects itself */
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

  int status = 0;
  for (int hop = 0;; hop++) {
    if (esp_http_client_open(t->hc, 0) != ESP_OK) {
      esp_teardown(t);
      return -1;
    }
    if (esp_http_client_fetch_headers(t->hc) < 0) {
      esp_teardown(t);
      return -1;
    }
    status = esp_http_client_get_status_code(t->hc);
    bool is_redirect = status == 301 || status == 302 || status == 303 ||
                       status == 307 || status == 308;
    if (!is_redirect || hop >= ESP_MAX_REDIRECT_HOPS) break;
    /* Same-origin only: request headers persist on the handle across the
     * reopen, so following a cross-origin Location would hand
     * caller-supplied credentials to another host. Absolute-path relative
     * Locations are same-origin by definition; absolute http(s) ones must
     * match scheme, host, and port. Everything else (cross-origin,
     * scheme-relative "//host/...", other schemes, no Location) surfaces
     * the 3xx to the client's reconnect policy. */
    if (t->location[0] == '\0') break;
    bool relative_path = t->location[0] == '/' && t->location[1] != '/';
    if (!relative_path) {
      bool absolute_http = strncasecmp(t->location, "http://", 7) == 0 ||
                           strncasecmp(t->location, "https://", 8) == 0;
      if (!absolute_http || !same_origin_urls(t->url, t->location)) break;
      snprintf(t->url, sizeof t->url, "%s", t->location);
    }
    if (esp_http_client_set_redirection(t->hc) != ESP_OK) break;
    t->content_type[0] = '\0'; /* final block's captured headers win */
    t->location[0] = '\0';
    t->retry_after_s = -1;
    esp_http_client_close(t->hc);
  }
  out->status_code = status;
  snprintf(out->content_type, sizeof out->content_type, "%s", t->content_type);
  out->retry_after_s = t->retry_after_s;
  t->configured_timeout_ms = 0; /* force set_timeout on first read */
  return 0;
}

static int esp_read_fn(void *vctx, void *buf, size_t len, uint32_t timeout_ms) {
  esp_ctx_t *t = vctx;
  if (!t->hc) return SSE_READ_ERROR;
  if (timeout_ms != t->configured_timeout_ms) {
    /* esp_http_client takes int; clamp rather than wrap negative. */
    uint32_t tmo = timeout_ms > (uint32_t)INT_MAX ? (uint32_t)INT_MAX : timeout_ms;
    esp_http_client_set_timeout_ms(t->hc, (int)tmo);
    t->configured_timeout_ms = timeout_ms;
  }
  errno = 0;
  size_t want = len > (size_t)INT_MAX ? (size_t)INT_MAX : len;
  int r = esp_http_client_read(t->hc, buf, (int)want);
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
