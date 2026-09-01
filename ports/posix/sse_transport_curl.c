#include "sse_transport_curl.h"
#include "sse_clock_posix.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strncasecmp */

#define RING_CAP (32 * 1024)
#define OPEN_TIMEOUT_MS 30000 /* per redirect hop */

/* Redirects are followed manually, same-origin only: libcurl's
 * FOLLOWLOCATION forwards caller-supplied custom headers (API keys and
 * friends) to whatever host the server names. Cross-origin redirects are
 * surfaced as their 3xx status instead. */
#define MAX_REDIRECT_HOPS 5

typedef struct {
  CURLM *multi;
  CURL *easy;
  struct curl_slist *hdrs;
  char ring[RING_CAP];
  size_t r_head, r_len;
  int body_started;         /* first body byte seen */
  int headers_done;         /* final header block completed */
  int header_block_status;  /* status code of the current header block */
  int transfer_done;
  CURLcode transfer_result;
  int paused;
  int32_t retry_after_s;
} curl_ctx_t;

static size_t on_body(char *ptr, size_t sz, size_t nm, void *ud) {
  curl_ctx_t *t = ud;
  size_t n = sz * nm;
  /* Bodies of redirect responses are transport noise: discard them so a
   * large 3xx body can neither fill the ring nor pause the transfer. A
   * paused transfer never completes, and CURLINFO_REDIRECT_URL is only
   * reliable once the transfer has completed. */
  if (t->header_block_status >= 300 && t->header_block_status < 400) {
    return n;
  }
  t->body_started = 1;
  if (n > RING_CAP - t->r_len) {
    t->paused = 1;
    return CURL_WRITEFUNC_PAUSE;
  }
  for (size_t i = 0; i < n; i++) {
    t->ring[(t->r_head + t->r_len + i) % RING_CAP] = ptr[i];
  }
  t->r_len += n;
  return n;
}

static size_t on_header(char *ptr, size_t sz, size_t nm, void *ud) {
  curl_ctx_t *t = ud;
  size_t n = sz * nm;
  if (n > 5 && strncmp(ptr, "HTTP/", 5) == 0) {
    t->retry_after_s = -1; /* new response block (redirect chain) */
    t->headers_done = 0;
    /* Status code follows the first space: "HTTP/1.1 302 ..." */
    t->header_block_status = 0;
    const char *sp = memchr(ptr, ' ', n);
    if (sp) {
      const char *q = sp + 1;
      const char *end = ptr + n;
      while (q < end && *q >= '0' && *q <= '9') {
        t->header_block_status = t->header_block_status * 10 + (*q - '0');
        q++;
      }
    }
  } else if (n <= 2) {
    /* Blank line: this header block is complete. 1xx blocks (e.g. 103
     * Early Hints) are informational and the real response follows, so
     * they never complete the open; redirect (3xx) decisions happen in
     * curl_open_fn. An SSE server may finish its headers and then stay
     * silent until the first event, so open() must not wait for body
     * bytes. */
    if (t->header_block_status >= 200) {
      t->headers_done = 1;
    }
  } else if (n > 12 && strncasecmp(ptr, "Retry-After:", 12) == 0) {
    const char *p = ptr + 12;
    const char *end = ptr + n;
    while (p < end && *p == ' ') p++;
    if (p < end && *p >= '0' && *p <= '9') {
      long v = strtol(p, NULL, 10); /* delta-seconds only; HTTP-date form left as absent */
      if (v >= 0) t->retry_after_s = (int32_t)v;
    }
  }
  return n;
}

/* Run curl for up to wait_ms; harvest completion. Returns 0 or -1. */
static int pump(curl_ctx_t *t, int wait_ms) {
  int running = 0;
  if (t->paused && t->r_len < RING_CAP / 2) {
    t->paused = 0;
    curl_easy_pause(t->easy, CURLPAUSE_CONT);
  }
  if (curl_multi_wait(t->multi, NULL, 0, wait_ms, NULL) != CURLM_OK) return -1;
  if (curl_multi_perform(t->multi, &running) != CURLM_OK) return -1;
  CURLMsg *msg;
  int left;
  while ((msg = curl_multi_info_read(t->multi, &left)) != NULL) {
    if (msg->msg == CURLMSG_DONE) {
      t->transfer_done = 1;
      t->transfer_result = msg->data.result;
    }
  }
  return 0;
}

/* True when two absolute URLs share scheme, host, and (default-resolved)
 * port. Anything unparseable is not same-origin. */
static int same_origin(const char *a, const char *b) {
  int same = 0;
  CURLU *ua = curl_url();
  CURLU *ub = curl_url();
  char *sa = NULL, *sb = NULL, *ha = NULL, *hb = NULL, *pa = NULL, *pb = NULL;
  if (ua && ub && curl_url_set(ua, CURLUPART_URL, a, 0) == CURLUE_OK &&
      curl_url_set(ub, CURLUPART_URL, b, 0) == CURLUE_OK &&
      curl_url_get(ua, CURLUPART_SCHEME, &sa, 0) == CURLUE_OK &&
      curl_url_get(ub, CURLUPART_SCHEME, &sb, 0) == CURLUE_OK &&
      curl_url_get(ua, CURLUPART_HOST, &ha, 0) == CURLUE_OK &&
      curl_url_get(ub, CURLUPART_HOST, &hb, 0) == CURLUE_OK &&
      curl_url_get(ua, CURLUPART_PORT, &pa, CURLU_DEFAULT_PORT) == CURLUE_OK &&
      curl_url_get(ub, CURLUPART_PORT, &pb, CURLU_DEFAULT_PORT) == CURLUE_OK) {
    same = strcasecmp(sa, sb) == 0 && strcasecmp(ha, hb) == 0 && strcmp(pa, pb) == 0;
  }
  curl_free(sa);
  curl_free(sb);
  curl_free(ha);
  curl_free(hb);
  curl_free(pa);
  curl_free(pb);
  if (ua) curl_url_cleanup(ua);
  if (ub) curl_url_cleanup(ub);
  return same;
}

static void teardown(curl_ctx_t *t) {
  if (t->easy && t->multi) curl_multi_remove_handle(t->multi, t->easy);
  if (t->easy) curl_easy_cleanup(t->easy);
  if (t->multi) curl_multi_cleanup(t->multi);
  if (t->hdrs) curl_slist_free_all(t->hdrs);
  memset(t, 0, sizeof *t);
  t->retry_after_s = -1;
}

static int curl_open_fn(void *vctx, const sse_request_t *req, sse_response_info_t *out) {
  curl_ctx_t *t = vctx;
  teardown(t);
  t->easy = curl_easy_init();
  t->multi = curl_multi_init();
  if (!t->easy || !t->multi) {
    teardown(t);
    return -1;
  }
  for (const char *const *h = req->headers; h && *h; h++) {
    /* On allocation failure curl_slist_append returns NULL without freeing
     * the old list; assigning directly would leak it. */
    struct curl_slist *appended = curl_slist_append(t->hdrs, *h);
    if (!appended) {
      teardown(t);
      return -1;
    }
    t->hdrs = appended;
  }
  curl_easy_setopt(t->easy, CURLOPT_URL, req->url);
  curl_easy_setopt(t->easy, CURLOPT_HTTPHEADER, t->hdrs);
  curl_easy_setopt(t->easy, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(t->easy, CURLOPT_WRITEDATA, t);
  curl_easy_setopt(t->easy, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(t->easy, CURLOPT_HEADERDATA, t);
  /* Proxy CONNECT responses ("200 Connection established") would otherwise
   * reach on_header and be mistaken for the final response. */
  curl_easy_setopt(t->easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
  /* No FOLLOWLOCATION: redirects are followed manually below, same-origin
   * only, so caller-supplied headers never travel to another origin. */
  curl_easy_setopt(t->easy, CURLOPT_ACCEPT_ENCODING, "identity");
  curl_easy_setopt(t->easy, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_multi_add_handle(t->multi, t->easy);

  int hops = 0;
  for (;;) {
    /* Wall-clock budget: pump() returns immediately whenever the socket is
     * active, so counting iterations would burn the budget in a fraction of
     * the nominal time against a busy connection. */
    uint32_t wait_start = sse_now_ms_posix();
    while (!t->headers_done && !t->body_started && !t->transfer_done &&
           (uint32_t)(sse_now_ms_posix() - wait_start) < OPEN_TIMEOUT_MS) {
      if (pump(t, 100) < 0) {
        teardown(t);
        return -1;
      }
    }
    if (!t->headers_done && !t->body_started && !t->transfer_done) { /* header stall */
      teardown(t);
      return -1;
    }
    /* A transfer that failed before its final headers has no usable
     * response: e.g. a redirect target that refused the connection. This
     * must be a transport failure (retryable), not a stale 3xx status. */
    if (t->transfer_done && t->transfer_result != CURLE_OK && !t->headers_done &&
        !t->body_started) {
      teardown(t);
      return -1;
    }
    if (t->header_block_status < 300 || t->header_block_status >= 400) {
      break; /* final response */
    }
    /* Drain the (discarded) 3xx body to completion first:
     * CURLINFO_REDIRECT_URL is only populated for a completed transfer.
     * Wall-clock budget, same reasoning as above: a slow-but-active body
     * wakes pump() far more often than once per 100 ms. */
    uint32_t drain_start = sse_now_ms_posix();
    while (!t->transfer_done &&
           (uint32_t)(sse_now_ms_posix() - drain_start) < OPEN_TIMEOUT_MS) {
      if (pump(t, 100) < 0) {
        teardown(t);
        return -1;
      }
    }
    if (!t->transfer_done) { /* redirect body stall */
      teardown(t);
      return -1;
    }
    /* Manual, same-origin-only redirect. Cross-origin (incl. any scheme or
     * port change), unparseable, or over-budget redirects surface the 3xx
     * to the client, whose reconnect policy decides. */
    char *redir = NULL;
    char *current = NULL;
    if (hops >= MAX_REDIRECT_HOPS ||
        curl_easy_getinfo(t->easy, CURLINFO_REDIRECT_URL, &redir) != CURLE_OK ||
        !redir ||
        curl_easy_getinfo(t->easy, CURLINFO_EFFECTIVE_URL, &current) != CURLE_OK ||
        !current || !same_origin(current, redir)) {
      break;
    }
    hops++;
    curl_multi_remove_handle(t->multi, t->easy);
    curl_easy_setopt(t->easy, CURLOPT_URL, redir); /* copied by libcurl */
    t->r_head = 0;
    t->r_len = 0;
    t->body_started = 0;
    t->headers_done = 0;
    t->transfer_done = 0;
    t->transfer_result = CURLE_OK;
    t->paused = 0;
    t->header_block_status = 0;
    t->retry_after_s = -1;
    curl_multi_add_handle(t->multi, t->easy);
  }
  long status = 0;
  curl_easy_getinfo(t->easy, CURLINFO_RESPONSE_CODE, &status);
  out->status_code = (int)status;
  const char *ct = NULL;
  curl_easy_getinfo(t->easy, CURLINFO_CONTENT_TYPE, &ct);
  snprintf(out->content_type, sizeof out->content_type, "%s", ct ? ct : "");
  out->retry_after_s = t->retry_after_s;
  return 0;
}

static int curl_read_fn(void *vctx, void *buf, size_t len, uint32_t timeout_ms) {
  curl_ctx_t *t = vctx;
  if (t->r_len == 0) {
    if (t->transfer_done) {
      return t->transfer_result == CURLE_OK ? SSE_READ_EOF : SSE_READ_ERROR;
    }
    if (pump(t, (int)timeout_ms) < 0) return SSE_READ_ERROR;
    if (t->r_len == 0) {
      if (t->transfer_done) {
        return t->transfer_result == CURLE_OK ? SSE_READ_EOF : SSE_READ_ERROR;
      }
      return SSE_READ_TIMEOUT;
    }
  }
  size_t n = t->r_len < len ? t->r_len : len;
  for (size_t i = 0; i < n; i++) {
    ((char *)buf)[i] = t->ring[(t->r_head + i) % RING_CAP];
  }
  t->r_head = (t->r_head + n) % RING_CAP;
  t->r_len -= n;
  if (t->paused && t->r_len < RING_CAP / 2) {
    t->paused = 0;
    curl_easy_pause(t->easy, CURLPAUSE_CONT);
  }
  return (int)n;
}

static void curl_close_fn(void *vctx) { teardown(vctx); }

sse_transport_t *sse_transport_curl_new(void) {
  static int global_done = 0;
  if (!global_done) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    global_done = 1;
  }
  sse_transport_t *tr = calloc(1, sizeof *tr);
  curl_ctx_t *ctx = calloc(1, sizeof *ctx);
  if (!tr || !ctx) {
    free(tr);
    free(ctx);
    return NULL;
  }
  ctx->retry_after_s = -1;
  tr->ctx = ctx;
  tr->open = curl_open_fn;
  tr->read = curl_read_fn;
  tr->close = curl_close_fn;
  return tr;
}

void sse_transport_curl_free(sse_transport_t *t) {
  if (!t) return;
  teardown(t->ctx);
  free(t->ctx);
  free(t);
}
