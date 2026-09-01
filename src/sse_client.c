#include "eventsource/sse_client.h"
#include <stdio.h>
#include <string.h>

/* Cross-task stop flag access. C99 has no <stdatomic.h>, so use the GNU
 * atomic builtins, which Apple clang and ESP-IDF's GCC toolchains support
 * even in -std=c99 mode. There is deliberately no silent fallback: a plain
 * (even volatile) access is a C data race, and sse_client_request_stop()
 * publicly promises cross-task safety. Compilers without the builtins must
 * opt in to the weakened volatile behavior explicitly. */
#if defined(__GNUC__) || defined(__clang__)
#define STOP_SET(c) __atomic_store_n(&(c)->stop_requested, 1, __ATOMIC_RELEASE)
#define STOP_GET(c) __atomic_load_n(&(c)->stop_requested, __ATOMIC_ACQUIRE)
#elif defined(SSE_CLIENT_ALLOW_NONATOMIC_STOP)
/* Explicit opt-in: volatile is NOT a synchronization primitive. The
 * cross-task guarantee of sse_client_request_stop() then rests entirely on
 * the platform making aligned single-byte stores atomic. */
#define STOP_SET(c) ((void)((c)->stop_requested = 1))
#define STOP_GET(c) ((c)->stop_requested)
#else
#error "No atomic builtins available for the cross-task stop flag. Use a \
GCC-compatible compiler, or define SSE_CLIENT_ALLOW_NONATOMIC_STOP to accept \
a volatile fallback with a weakened sse_client_request_stop() guarantee."
#endif

/* ---- parser callback bridge ---- */

static void bridge_on_id(void *ud, const char *id, size_t len) {
  sse_client_t *c = ud;
  if (c->state != SSE_STATE_OPEN) return; /* close() during dispatch freezes state */
  if (len <= SSE_CLIENT_ID_MAX) {
    memcpy(c->last_event_id, id, len + 1); /* parser NUL-terminates */
    c->last_event_id_len = len;
  }
}

static void bridge_on_retry(void *ud, uint32_t ms) {
  sse_client_t *c = ud;
  if (c->state != SSE_STATE_OPEN) return; /* close() during dispatch freezes state */
  c->base_retry_ms = ms;
}

static void bridge_on_event(void *ud, const sse_parser_event_t *ev) {
  sse_client_t *c = ud;
  if (c->state != SSE_STATE_OPEN) return; /* close() during dispatch stops it */
  c->attempts = 0; /* healthy connection: reset flap counter */
  if (!c->cfg.callbacks.on_message) return;
  sse_message_t m;
  m.event = ev->event ? ev->event : "message";
  m.data = ev->data;
  m.data_len = ev->data_len;
  m.last_event_id = c->last_event_id;
  c->cfg.callbacks.on_message(c->cfg.callbacks.userdata, &m);
}

static void bridge_on_perr(void *ud, sse_parse_error_t err) {
  sse_client_t *c = ud;
  if (c->state != SSE_STATE_OPEN) return; /* close() during dispatch stops it */
  if (err != SSE_PARSE_ERR_DATA_TOO_LARGE) return;
  if (!c->cfg.callbacks.on_error) return;
  sse_error_t e = {SSE_ERR_MESSAGE_TOO_LARGE, 0, true, 0}; /* stream continues */
  c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
}

/* ---- helpers ---- */

static uint32_t prng_next(sse_client_t *c) {
  uint32_t x = c->prng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  c->prng = x ? x : 0x9e3779b9u;
  return c->prng;
}

static void close_transport(sse_client_t *c) {
  if (c->transport_open) {
    c->cfg.transport->close(c->cfg.transport->ctx);
    c->transport_open = false;
  }
}

static void to_closed(sse_client_t *c) {
  close_transport(c);
  if (c->state == SSE_STATE_CLOSED) return;
  c->state = SSE_STATE_CLOSED;
  if (c->cfg.callbacks.on_closed) c->cfg.callbacks.on_closed(c->cfg.callbacks.userdata);
}

static bool ct_is_event_stream(const char *ct) {
  static const char want[] = "text/event-stream";
  size_t i = 0;
  for (; want[i]; i++) {
    char a = ct[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (a != want[i]) return false;
  }
  return ct[i] == '\0' || ct[i] == ';' || ct[i] == ' ';
}

static void build_headers(sse_client_t *c) {
  size_t n = 0;
  c->headers[n++] = "Accept: text/event-stream";
  c->headers[n++] = "Cache-Control: no-store";
  if (c->last_event_id_len > 0) {
    snprintf(c->id_header, sizeof c->id_header, "Last-Event-ID: %s", c->last_event_id);
    c->headers[n++] = c->id_header;
  }
  if (c->cfg.extra_headers) {
    /* Bound by the count validated at init: the borrowed array could have
     * been (wrongly) grown since, and c->headers must never overflow. */
    for (size_t i = 0; i < c->extra_header_count && c->cfg.extra_headers[i]; i++) {
      c->headers[n++] = c->cfg.extra_headers[i];
    }
  }
  c->headers[n] = NULL;
}

static bool default_should_retry(sse_error_reason_t reason, int status, int32_t retry_after_s) {
  switch (reason) {
    case SSE_ERR_TRANSPORT:
    case SSE_ERR_IDLE_TIMEOUT:
    case SSE_ERR_BAD_CONTENT_TYPE:
      return true;
    case SSE_ERR_SERVER_STOP:
      return false;
    case SSE_ERR_HTTP_STATUS:
      if (status >= 500) return true;
      if (status == 429) return true;
      return retry_after_s >= 0;
    default:
      return false;
  }
}

static void fail_conn(sse_client_t *c, sse_error_reason_t reason, int status,
                      int32_t retry_after_s) {
  close_transport(c);
  sse_error_t e;
  e.reason = reason;
  e.http_status = status;
  e.retry_in_ms = 0;
  bool retry = default_should_retry(reason, status, retry_after_s);
  if (c->cfg.reconnect_policy) {
    e.will_retry = retry;
    retry = c->cfg.reconnect_policy(c->cfg.callbacks.userdata, &e);
    if (c->state == SSE_STATE_CLOSED) {
      /* The hook called sse_client_close(): on_closed already fired and is
       * the final signal. Its return value must not resurrect the client. */
      return;
    }
  }
  if (!retry) {
    e.will_retry = false;
    if (c->cfg.callbacks.on_error) c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
    to_closed(c);
    return;
  }
  /* Compute the whole delay in 64 bits: retry_after_s * 1000 and the jitter
   * addition can both wrap uint32 for remotely supplied values, sidestepping
   * any later clamp. */
  uint64_t delay64;
  if (retry_after_s >= 0) {
    delay64 = (uint64_t)retry_after_s * 1000u;
  } else if (c->cfg.max_retry_ms == 0) {
    delay64 = c->base_retry_ms;
  } else {
    unsigned sh = c->attempts > 15 ? 15 : c->attempts;
    uint64_t d = (uint64_t)c->base_retry_ms << sh;
    delay64 = d > c->cfg.max_retry_ms ? c->cfg.max_retry_ms : d;
  }
  if (c->cfg.jitter_pct) {
    delay64 += (delay64 * (prng_next(c) % (c->cfg.jitter_pct + 1u))) / 100u;
  }
  /* Single clamp, after all arithmetic: WAITING_RETRY casts (deadline - now)
   * to int32_t, so a delay >= 2^31 ms would wrap negative and fire early. */
  uint32_t delay =
      delay64 > (uint64_t)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)delay64;
  c->attempts++;
  c->retry_deadline = c->cfg.now_ms() + delay;
  c->state = SSE_STATE_WAITING_RETRY;
  e.will_retry = true;
  e.retry_in_ms = delay;
  if (c->cfg.callbacks.on_error) c->cfg.callbacks.on_error(c->cfg.callbacks.userdata, &e);
}

static void do_connect(sse_client_t *c) {
  c->state = SSE_STATE_CONNECTING;
  build_headers(c);
  sse_request_t req;
  req.url = c->cfg.url;
  req.headers = c->headers;
  sse_response_info_t info;
  info.status_code = 0;
  info.content_type[0] = '\0';
  info.retry_after_s = -1;
  if (c->cfg.transport->open(c->cfg.transport->ctx, &req, &info) != 0) {
    fail_conn(c, SSE_ERR_TRANSPORT, 0, -1);
    return;
  }
  c->transport_open = true;
  if (info.status_code == 204) {
    fail_conn(c, SSE_ERR_SERVER_STOP, 204, -1);
    return;
  }
  if (info.status_code != 200) {
    fail_conn(c, SSE_ERR_HTTP_STATUS, info.status_code, info.retry_after_s);
    return;
  }
  if (!c->cfg.skip_content_type_check && !ct_is_event_stream(info.content_type)) {
    fail_conn(c, SSE_ERR_BAD_CONTENT_TYPE, info.status_code, -1);
    return;
  }
  sse_parser_reset(&c->parser);
  c->last_rx_ms = c->cfg.now_ms();
  c->state = SSE_STATE_OPEN;
  if (c->cfg.callbacks.on_open) {
    c->cfg.callbacks.on_open(c->cfg.callbacks.userdata, c->attempts);
  }
}

/* ---- public API ---- */

int sse_client_init(sse_client_t *c, const sse_client_config_t *cfg) {
  if (!c || !cfg || !cfg->url || !cfg->transport || !cfg->now_ms) return -1;
  if (!cfg->transport->open || !cfg->transport->read || !cfg->transport->close) return -1;
  if (!cfg->buffers.data_buf || cfg->buffers.data_buf_len < 2) return -1;
  if (!cfg->buffers.id_buf || cfg->buffers.id_buf_len < 2) return -1;
  /* Every id the parser can accept (id_buf_len - 1 chars) must fit the
   * persisted lastEventId buffer; otherwise long ids would be silently
   * dropped and reconnects would resume from a stale position. */
  if (cfg->buffers.id_buf_len > SSE_CLIENT_ID_MAX + 1) return -1;
  if (!cfg->buffers.event_buf || cfg->buffers.event_buf_len < 2) return -1;
  if (!cfg->rx_buf || cfg->rx_buf_len == 0) return -1;
  size_t extra = 0;
  if (cfg->extra_headers) {
    for (const char *const *h = cfg->extra_headers; *h; h++) extra++;
  }
  if (3 + extra > SSE_CLIENT_MAX_HEADERS) return -1;

  memset(c, 0, sizeof *c);
  c->cfg = *cfg;
  c->extra_header_count = extra;
  if (c->cfg.default_retry_ms == 0) c->cfg.default_retry_ms = 3000;
  if (c->cfg.read_timeout_ms == 0) c->cfg.read_timeout_ms = 500;
  c->base_retry_ms = c->cfg.default_retry_ms;
  c->prng = c->cfg.now_ms() ^ 0xA5A5A5A5u;
  if (!c->prng) c->prng = 1;

  sse_parser_callbacks_t pcb = {c, bridge_on_event, bridge_on_id, bridge_on_retry,
                                bridge_on_perr};
  sse_parser_init(&c->parser, &pcb, &c->cfg.buffers);
  c->state = SSE_STATE_IDLE;
  return 0;
}

uint32_t sse_client_poll(sse_client_t *c) {
  if (STOP_GET(c) && c->state != SSE_STATE_CLOSED) to_closed(c);
  switch ((sse_client_state_t)c->state) {
    case SSE_STATE_CLOSED:
      return UINT32_MAX;
    case SSE_STATE_IDLE:
    case SSE_STATE_CONNECTING:
      do_connect(c);
      /* Callbacks fired during the attempt (on_error, the policy hook,
       * on_closed) may have closed the client; honor the CLOSED contract
       * in the same invocation. */
      return c->state == SSE_STATE_CLOSED ? UINT32_MAX : 0;
    case SSE_STATE_WAITING_RETRY: {
      int32_t remain = (int32_t)(c->retry_deadline - c->cfg.now_ms());
      if (remain <= 0) {
        c->state = SSE_STATE_CONNECTING;
        return 0;
      }
      return (uint32_t)remain;
    }
    case SSE_STATE_OPEN: {
      int r = c->cfg.transport->read(c->cfg.transport->ctx, c->cfg.rx_buf,
                                     c->cfg.rx_buf_len, c->cfg.read_timeout_ms);
      if (STOP_GET(c)) {
        to_closed(c);
        return UINT32_MAX;
      }
      if (r > 0) {
        if ((size_t)r > c->cfg.rx_buf_len) {
          /* A transport must never report more bytes than the buffer holds;
           * feeding that count onward would read past caller memory. */
          fail_conn(c, SSE_ERR_TRANSPORT, 0, -1);
          return c->state == SSE_STATE_CLOSED ? UINT32_MAX : 0;
        }
        c->last_rx_ms = c->cfg.now_ms();
        sse_parser_feed(&c->parser, c->cfg.rx_buf, (size_t)r);
        /* A callback during dispatch may have closed the client; honor the
         * CLOSED contract in the same invocation. */
        return c->state == SSE_STATE_CLOSED ? UINT32_MAX : 0;
      }
      if (r == SSE_READ_TIMEOUT) {
        if (c->cfg.idle_timeout_ms != 0 &&
            (uint32_t)(c->cfg.now_ms() - c->last_rx_ms) >= c->cfg.idle_timeout_ms) {
          fail_conn(c, SSE_ERR_IDLE_TIMEOUT, 0, -1);
          if (c->state == SSE_STATE_CLOSED) return UINT32_MAX;
        }
        return c->cfg.read_timeout_ms;
      }
      fail_conn(c, SSE_ERR_TRANSPORT, 0, -1); /* EOF or error */
      return c->state == SSE_STATE_CLOSED ? UINT32_MAX : 0;
    }
  }
  return 0;
}

void sse_client_close(sse_client_t *c) { to_closed(c); }

void sse_client_request_stop(sse_client_t *c) { STOP_SET(c); }

sse_client_state_t sse_client_state(const sse_client_t *c) {
  return (sse_client_state_t)c->state;
}

const char *sse_client_last_event_id(const sse_client_t *c) {
  return c->last_event_id;
}
