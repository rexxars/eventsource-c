# eventsource-c

Server-Sent Events (EventSource) client for (embedded) C. Spec-accurate SSE parsing, automatic reconnection with `retry`/`Retry-After` support, `Last-Event-ID` resume, optional idle timeout for dead-connection detection, and zero heap allocation in the core. Primary target: ESP32 under ESP-IDF; the core is portable C99 and also builds on POSIX.

## Install (ESP-IDF)

```sh
idf.py add-dependency "rexxars/eventsource"
```

## Usage (ESP-IDF)

```c
#include "eventsource/sse_client.h"
#include "sse_client_task.h"   /* FreeRTOS task wrapper, from the ESP-IDF port */
#include "sse_transport_esp.h" /* esp_http_client transport, from the ESP-IDF port */
#include "esp_timer.h"

/* You own all memory: the library never allocates. See "Buffer sizing". */
static char db[8192], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

/* Monotonic millisecond clock; the client uses it for reconnect deadlines
 * and the idle timeout. Wrapping is fine, only differences are used. */
static uint32_t now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

/* `ud` is cfg.callbacks.userdata, passed through to every callback (NULL
 * here). `m` and everything it points to is only valid during the callback:
 * copy what you want to keep. */
static void on_message(void *ud, const sse_message_t *m) {
  /* m->event:         event type; "message" unless the server sent `event:`
   * m->data:          payload, NUL-terminated (may contain embedded NULs)
   * m->data_len:      payload length in bytes
   * m->last_event_id: current Last-Event-ID, "" when none so far */
  printf("[%s] %.*s\n", m->event, (int)m->data_len, m->data);
}

void start_stream(void) {
  sse_client_config_t cfg = {0};

  /* Borrowed pointer: must stay alive and unchanged until the client is
   * CLOSED. Same-origin redirects are followed automatically (up to 5
   * hops); cross-origin ones are surfaced instead. See "Redirects". */
  cfg.url = "https://example.com/stream";

  /* Caller-provided buffers; data_buf caps the event payload (N bytes
   * holds events up to N-1 bytes). See "Buffer sizing". */
  cfg.buffers.data_buf = db;  cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;    cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx;            cfg.rx_buf_len = sizeof rx;

  /* Reconnect delays double per consecutive failure, capped at
   * max_retry_ms (0 disables backoff: flat interval, browser-style), with
   * up to jitter_pct percent random extra so a fleet of devices does not
   * reconnect in lockstep. See "Reconnect behavior". */
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;

  /* Reconnect when NO bytes arrive for this long. Any received byte
   * counts, including `: keepalive` comment heartbeats. 0 disables. See
   * "Idle timeout and heartbeats". */
  cfg.idle_timeout_ms = 60000;

  /* How bytes reach the client. Two transports ship with the library:
   * this one (esp_http_client, heap-allocated by the constructor) and
   * sse_transport_curl_new() in the POSIX port. Or write your own: see
   * "Custom transports". */
  cfg.transport = sse_transport_esp_http_client();

  cfg.now_ms = now_ms;
  cfg.callbacks.on_message = on_message;
  /* Also available: on_open, on_error, on_closed. See "Callbacks". */

  sse_client_init(&client, &cfg);

  /* Runs the poll loop in a FreeRTOS task. Arguments: client, task name,
   * stack size in BYTES, task priority. The task deletes itself once the
   * client reaches SSE_STATE_CLOSED; stop it from any other task with
   * sse_client_request_stop(&client). */
  sse_client_start_task(&client, "sse", 6144, 5);
}
```

See `examples/esp32` for a complete application including Wi-Fi setup.

## Usage (POSIX)

Same client, different transport and clock, and you drive the poll loop yourself (the FreeRTOS task wrapper is ESP-IDF only). Requires libcurl 7.62 or newer.

```c
#include "eventsource/sse_client.h"
#include "sse_transport_curl.h" /* libcurl transport, from the POSIX port */
#include "sse_clock_posix.h"    /* sse_now_ms_posix(), CLOCK_MONOTONIC in ms */
#include <unistd.h>

static sse_client_t client;

int main(void) {
  /* Heap-allocated (ports may allocate; the core never does). Release
   * with sse_transport_curl_free() after the client is closed. */
  sse_transport_t *tr = sse_transport_curl_new();

  sse_client_config_t cfg = {0};
  /* ... url, buffers, timeouts as in the ESP-IDF example ... */
  cfg.transport = tr;
  cfg.now_ms = sse_now_ms_posix;

  sse_client_init(&client, &cfg);

  /* sse_client_poll() connects, reads, parses, and dispatches callbacks.
   * It returns a max-sleep hint in milliseconds: 0 means call again
   * immediately, UINT32_MAX means the client is CLOSED, stop calling. */
  for (;;) {
    uint32_t s = sse_client_poll(&client);
    if (s == UINT32_MAX) break;
    if (s) usleep(s * 1000);
  }
  sse_transport_curl_free(tr);
  return 0;
}
```

See `examples/posix` for the complete program.

## Buffer sizing

- `data_buf` caps the event payload: a buffer of N bytes holds events up to N-1 bytes. Larger events are discarded, see "Oversized messages".
- `id_buf`/`event_buf`: 128 and 64 bytes are good defaults. `id_buf_len` may not exceed `SSE_CLIENT_ID_MAX + 1` (129 by default) so every accepted id can be persisted for reconnect resume; `sse_client_init` fails otherwise.
- `rx_buf` is the transport read scratch; 1 KiB is plenty.
- All buffers are caller-provided and borrowed until the client reaches `SSE_STATE_CLOSED`; the core never allocates.

## Callbacks

All callbacks fire on the task that calls `sse_client_poll()` (the wrapper task when using `sse_client_start_task`). Every callback receives `cfg.callbacks.userdata` as its first argument. Calling `sse_client_close()` from inside a callback is allowed and stops further dispatch; never call `sse_client_poll()` from a callback or free the client's memory inside one.

- `on_open(ud, reconnect_count)`: connection established and validated. `reconnect_count` is 0 on the first connect and counts consecutive reconnects since the last delivered message, so you can tell flapping from a healthy stream.
- `on_message(ud, msg)`: see the usage example for the `sse_message_t` fields.
- `on_error(ud, err)`: every failure, including each scheduled reconnect. `err->reason` is an `sse_error_reason_t`; `err->http_status` holds the HTTP status when relevant (0 otherwise); `err->will_retry` says whether a reconnect is scheduled, and `err->retry_in_ms` says when.
- `on_closed(ud)`: fires exactly once when the client reaches `SSE_STATE_CLOSED`, whether via `sse_client_close()`, `sse_client_request_stop()`, HTTP 204, or a non-retryable failure. It is the final signal; nothing fires after it.

## Reconnect behavior

By default the client retries transport failures (connect/read errors, EOF, idle timeout), wrong content types, HTTP 5xx, 429, and any 4xx carrying a `Retry-After` header. It stops permanently on HTTP 204 (the spec's "stop reconnecting" signal) and every other non-200 response: a 404 or 401 does not fix itself, and an unattended device hammering one just burns battery.

The delay honors the server's `retry:` field (default 3000 ms), doubling per consecutive failed attempt up to `max_retry_ms` (0 disables backoff), plus up-only jitter of at most `jitter_pct` percent. A `Retry-After` header on the failed response overrides the computed delay. A delivered message resets the backoff.

## Overriding the reconnect policy

Set `cfg.reconnect_policy` to take the retry decision yourself. The hook runs on every connection failure, receives the default policy's decision in `err->will_retry`, and whatever it returns is final. It runs before the `on_error` callback, which then reports the final decision.

```c
/* Browser-style strictness: never retry any non-200 HTTP response. */
static bool spec_strict(void *ud, const sse_error_t *err) {
  (void)ud;
  if (err->reason == SSE_ERR_HTTP_STATUS) return false;
  return err->will_retry; /* keep the default for everything else */
}

cfg.reconnect_policy = spec_strict;
```

Returning `true` for something the default would stop on works the same way, e.g. `return true;` on `err->http_status == 404` for a server that is known to 404 during deploys. Calling `sse_client_close()` inside the hook is allowed and final: the client stays CLOSED regardless of the return value. Note that credentials cannot be rotated in-flight (the `extra_headers` strings are borrowed and must not change), so the pattern for a 401 with an expired token is to close, refresh the token, and `sse_client_init` again with the new headers.

## Oversized messages

An event whose payload exceeds `data_buf` is dropped: the parser discards its bytes, delivers nothing, does not advance `Last-Event-ID` past it, and resumes cleanly at the next event boundary. The connection stays open. You are told about it through `on_error` with `err->reason == SSE_ERR_MESSAGE_TOO_LARGE` (informational: `will_retry` is true and `retry_in_ms` is 0 because no reconnect is involved).

You can react three ways: ignore it (log and move on), size `data_buf` for your largest expected event, or treat it as fatal. One caveat makes the choice matter: because `Last-Event-ID` is not advanced past the dropped event, a resuming server will replay it after the next reconnect, and an event that can never fit would then be dropped again on every reconnect.

```c
static void on_error(void *ud, const sse_error_t *err) {
  if (err->reason == SSE_ERR_MESSAGE_TOO_LARGE) {
    /* This device cannot process that event; give up rather than loop. */
    sse_client_close(&client);
    return;
  }
}
```

## Redirects

Both shipped transports follow redirects only within the same origin (same scheme, host, and port), up to 5 hops. A cross-origin redirect, including any HTTPS-to-HTTP downgrade, is not followed: it surfaces through `on_error` as `SSE_ERR_HTTP_STATUS` with the 3xx status, and the default policy stops. This is not configurable, deliberately: `extra_headers` typically carry credentials, and following a redirect to another host would hand them to it. If you need to follow one, react to the surfaced 3xx by closing and re-initializing the client with the target URL.

## Idle timeout and heartbeats

"Idle" means the transport delivered no bytes for `idle_timeout_ms`, whatever those bytes are: SSE comment heartbeats (`: keepalive`) reset the timer just like events do, without triggering any callback. When the timeout fires, the connection is treated as dead (the usual half-open TCP situation on flaky Wi-Fi) and goes through the normal retry path as `SSE_ERR_IDLE_TIMEOUT`, resuming with `Last-Event-ID` so nothing is lost against a well-behaved server. 0 disables it. Pick 2-3x the server's heartbeat interval; if the server sends no heartbeats and legitimately quiet periods are expected, leave it disabled, since a forced reconnect costs a TLS handshake.

## Stopping the client

From the polling task or any callback: `sse_client_close(&client)`. From any other task: `sse_client_request_stop(&client)`, the only function that is safe cross-task; the client closes on its next poll. Either way `on_closed` fires once and `sse_client_poll()` returns `UINT32_MAX` from then on.

## Custom transports

A transport is a small vtable (`sse_transport_t` in `eventsource/sse_transport.h`): `open()` performs the HTTP request and blocks until response headers are available, filling status, content type, and `Retry-After`; `read()` returns received bytes, `SSE_READ_TIMEOUT`, `SSE_READ_EOF`, or `SSE_READ_ERROR` within the given timeout; `close()` tears the connection down. The two shipped implementations are `sse_transport_esp_http_client()` (`sse_transport_esp.h`, ESP-IDF port) and `sse_transport_curl_new()` (`sse_transport_curl.h`, POSIX port); either serves as a reference for wrapping another HTTP client. The core never touches the network directly, so porting to a new platform is exactly this vtable plus a `now_ms` clock.

## License

MIT
