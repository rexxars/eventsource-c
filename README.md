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

/* The library never allocates. See "Buffer sizing" section. */
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
   * CLOSED. Same-origin redirects are followed automatically (up to 5 hops);
   * cross-origin ones are surfaced instead. See "Redirects" section. */
  cfg.url = "https://example.com/stream";

  /* Caller-provided buffers; data_buf caps the event payload (N bytes
   * holds events up to N-1 bytes). See "Buffer sizing" section. */
  cfg.buffers.data_buf = db;  cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;    cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx;            cfg.rx_buf_len = sizeof rx;

  /* Reconnect delays double per consecutive failure, capped at
   * max_retry_ms (0 disables backoff: flat interval, browser-style), with
   * up to `jitter_pct` percent random extra so a fleet of devices does not
   * reconnect in lockstep. See "Reconnect behavior" section. */
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;

  /* Reconnect when NO bytes arrive for this long. Any received byte
   * counts, including `: keepalive` comment heartbeats. 0 disables.
   * See "Idle timeout" section. */
  cfg.idle_timeout_ms = 60000;

  /* How bytes reach the client. Two transports ship with the library:
   * this one (esp_http_client, heap-allocated by the constructor) and
   * `sse_transport_curl_new()` in the POSIX port. Or write your own:
   * see "Custom transports" section. */
  cfg.transport = sse_transport_esp_http_client();

  cfg.now_ms = now_ms;
  cfg.callbacks.on_message = on_message;
  /* Also available: on_open, on_error, on_closed. See "Callbacks" section. */

  sse_client_init(&client, &cfg);

  /* Runs the poll loop in a FreeRTOS task. Arguments: client, task name,
   * stack size in BYTES, task priority. 6144 fits TLS plus light logging
   * callbacks; see "Sizing the task stack" section for how to pick and verify
   * a value. The task deletes itself once the client reaches SSE_STATE_CLOSED;
   * stop it from any other task with `sse_client_request_stop(&client)`. */
  sse_client_start_task(&client, "sse", 6144, 5);
}
```

See `examples/esp32` for a complete application including Wi-Fi setup, and `examples/esp32-tdisplay` for a LilyGO T-Display variant that renders live stream status on the built-in screen.

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
   * with `sse_transport_curl_free()` after the client is closed. */
  sse_transport_t *tr = sse_transport_curl_new();

  sse_client_config_t cfg = {0};
  /* ... url, buffers, timeouts as in the ESP-IDF example ... */
  cfg.transport = tr;
  cfg.now_ms = sse_now_ms_posix;

  sse_client_init(&client, &cfg);

  /* `sse_client_poll()` connects, reads, parses, and dispatches callbacks.
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

The three parser buffers map one-to-one onto the fields of the SSE wire format, so size them by looking at what your API actually sends. Given a stream like:

```
event: mutation                                  <- event_buf holds this value
id: e62d82e6-4f28#apc42s.9c31h830xn@PVo0MAf_kDZ  <- id_buf holds this value
data: {"documentId":"reD-g3ner4al.p0poV","transi <- data_buf accumulates this
data: tion":"update", ...}                       <- ...across multiple data lines

: keepalive                                      <- comments cost no buffer space
```

All of it additionally streams through `rx_buf` in raw network-sized chunks on its way to the parser.

- `event_buf`: `event:` field value, i.e. the event type name (`mutation` above). Size it for the longest event name your API emits, plus 1 for the NUL terminator. A name that does not fit is dropped, not truncated, and the event is delivered with the default type `"message"`.
- `id_buf`: `id:` field value. Size for the longest id you expect plus 1. An id that does not fit is dropped, not truncated, and this is not surfaced through `on_error`: the visible symptom is that `Last-Event-ID` stops advancing, so a later reconnect resumes from an older position. `id_buf_len` may not exceed `SSE_CLIENT_ID_MAX + 1` (129 by default; `sse_client_init` fails otherwise) so every accepted id can be persisted for resume. For APIs with longer ids, raise the ceiling at compile time with `-DSSE_CLIENT_ID_MAX=256` or similar.
- `data_buf`: payload of the `data:` field of the event currently being parsed; multiple `data:` lines are joined with `\n` and count toward the same total. A buffer of N bytes holds payloads up to N-1 bytes. Size it for the _largest_ event your API can send, not the _typical_ one. Getting this wrong can have consequences beyond a lost message (see "Oversized messages").
- `rx_buf` is the network read scratch and limits nothing. An event larger than `rx_buf` arrives across several reads and is reassembled in `data_buf`. Each poll drains at most `rx_buf_len` bytes from the transport, so with a 1 KiB buffer an 8 KiB event is consumed in eight read-and-parse rounds where an 8 KiB buffer does it in one; fewer, larger reads cost slightly less CPU per byte. For streams of small, occasional events the difference is unmeasurable and 1 KiB is plenty; if your API routinely ships events of tens of KiB, sizing `rx_buf` at a few KiB trims loop overhead. There is little point going beyond `data_buf`'s size, and latency is unaffected either way since the poll loop re-polls immediately while data is pending.
- All buffers are caller-provided and borrowed until the client reaches `SSE_STATE_CLOSED`; the core never allocates.

## Sizing the task stack

The stack given to `sse_client_start_task` must hold the deepest call chain the polling task ever makes: the poll loop itself (small), the transport, and above all mbedTLS, whose handshake runs on this task's stack during every `https://` connect and reconnect and typically needs 4-6 KB on its own. Your callbacks run on this task too: `printf`/`ESP_LOG` formatting costs another 1-2 KB (more with floats), and stack buffers in `on_message` add directly. Rules of thumb: 6-8 KB for `https://`, around 4 KB for plain `http://`, plus whatever your callbacks use. Heavy payload processing is better moved to another task via a queue, which also keeps the poll loop responsive.

Measure instead of guessing: `uxTaskGetStackHighWaterMark(NULL)` called from inside a callback reports the task's minimum-ever free stack (in bytes on ESP-IDF). Trigger the worst case deliberately (force a TLS reconnect, since the handshake is the peak), then size to the observed use plus 25-30% headroom. Undersizing panics with `Stack canary watchpoint triggered (sse)` naming the task; oversizing merely spends RAM.

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

> [!WARNING]
> **A dropped event can silently corrupt derived state**.
> If your stream is _stateful_, where later events build on earlier ones (incremental patches, mutation logs, anything with a "previous revision" notion) - every event applied after a dropped one may be applied to a stale base. > For such streams, ignoring the error is a terrible choice - treat it as fatal, resynchronize out of band (a full refetch of the current state), and only then resume the stream.

Also note that because `Last-Event-ID` is not advanced past the dropped event, a resuming server will replay it after the next reconnect, and an event that can never fit would then be dropped again on every reconnect.

```c
static void on_error(void *ud, const sse_error_t *err) {
  if (err->reason == SSE_ERR_MESSAGE_TOO_LARGE) {
    /* This device cannot process that event, and later events may depend
     * on it. Stop the stream; resync state out of band before resuming. */
    sse_client_close(&client);
    return;
  }
}
```

## Redirects

Both shipped transports follow redirects only within the same origin (same scheme, host, and port), up to 5 hops. A cross-origin redirect, including any HTTPS-to-HTTP downgrade, is not followed: it surfaces through `on_error` as `SSE_ERR_HTTP_STATUS` with the 3xx status, and the default policy stops. This is not configurable, deliberately: `extra_headers` typically carry credentials, and following a redirect to another host would hand them to it. If you need to follow one, react to the surfaced 3xx by closing and re-initializing the client with the target URL.

## Idle timeout

"Idle" means the transport delivered no bytes for `idle_timeout_ms` - these do not have to be event payload bytes - they can be newlines, comments and similar. When the timeout fires, the connection is treated as dead (the usual half-open TCP situation on flaky Wi-Fi) and goes through the normal retry path as `SSE_ERR_IDLE_TIMEOUT`, resuming with `Last-Event-ID` so nothing is lost against a well-behaved server. 0 disables it. If the server sends "heartbeats" (usually "comments" - lines prefixed with `:`, see below) - pick 2-3x the server's heartbeat interval; if the server sends no heartbeats and legitimately quiet periods are expected, leave it disabled, since a forced reconnect costs a TLS handshake.

## Heartbeats and comments

"Comments" in the Server-Sent Events protocol are lines prefixed with `:`. These are discarded by the parser, and often used as "heartbeats" - a server might send them every few seconds in order for the connection not to be treated as "dead".

A comment is recognized on its first byte (the leading `:`) and every following byte up to the line ending is discarded as it arrives. Nothing is accumulated, none of your buffers are touched, and no callback fires - a comment of any length costs zero memory.

## Stopping the client

From the polling task or any callback: `sse_client_close(&client)`. From any other task: `sse_client_request_stop(&client)`, the only function that is safe cross-task; the client closes on its next poll. Either way `on_closed` fires once and `sse_client_poll()` returns `UINT32_MAX` from then on.

## Custom transports

A transport is a small vtable (`sse_transport_t` in `eventsource/sse_transport.h`): `open()` performs the HTTP request and blocks until response headers are available, filling status, content type, and `Retry-After`; `read()` returns received bytes, `SSE_READ_TIMEOUT`, `SSE_READ_EOF`, or `SSE_READ_ERROR` within the given timeout; `close()` tears the connection down. The two shipped implementations are `sse_transport_esp_http_client()` (`sse_transport_esp.h`, ESP-IDF port) and `sse_transport_curl_new()` (`sse_transport_curl.h`, POSIX port); either serves as a reference for wrapping another HTTP client. The core never touches the network directly, so porting to a new platform is exactly this vtable plus a `now_ms` clock.

## License

MIT © [Espen Hovlandsdal](https://espen.codes/)
