# Proposal: an EventSource (SSE) client library for C

A small, allocation-free C99 library implementing the client side of the Server-Sent Events protocol, designed to run reliably on constrained devices (primary target: ESP32 under ESP-IDF/FreeRTOS) while remaining fully portable to POSIX for development, testing and fuzzing.

This is the C sibling of the `eventsource-parser` and `eventsource` JS packages, and it mirrors their proven layering: a pure protocol parser with no I/O, and a client on top that owns connection lifecycle, `Last-Event-ID` tracking and reconnection. It deliberately does not mimic the web `EventSource` API surface (no EventTarget, no DOM events): same protocol semantics, C-idiomatic API.

## Goals

- Correct, spec-informed SSE parsing: `\n`, `\r` and `\r\n` line endings (including a CRLF split across two reads), leading BOM, comment lines, `data`/`event`/`id`/`retry` field semantics, multi-line data joined with `\n`.
- `lastEventId` tracking with browser-equivalent semantics, sent as `Last-Event-ID` on reconnect (only when non-empty).
- Automatic reconnection honoring the server-sent `retry:` value, with optional backoff and jitter for unattended devices.
- Callbacks for open, message, error/reconnecting and terminal close, with enough information to tell a first connect from a reconnect.
- Hard upper bounds on every buffer: max message size is a configuration input, not a hope. No heap allocation in the core library; all buffers are caller-provided so fully static builds work.
- Deterministic behavior at every chunk boundary: feeding the same byte stream in any split produces the same callback sequence.
- Portable core (C99, no OS or network includes) with thin transport ports for ESP-IDF and POSIX.

## Non-goals

- Not an HTTP implementation. TLS, redirects, chunked transfer decoding and proxies belong to the transport port (esp_http_client already does all of this on ESP32).
- No CORS, `withCredentials`, origin or other browser-security semantics.
- No UTF-8 validation or decoding: payload bytes are passed through as-is with an explicit length.
- No multi-listener event dispatch. One callback set per client; fan-out is the application's job.

## Architecture

Three layers, mirroring the JS packages plus an explicit transport seam that JS gets for free from `fetch`:

- `sse_parser`: streaming SSE parser. Pure function of bytes in, callbacks out. No I/O, no clock, no allocation. The C counterpart of `eventsource-parser`.
- `sse_client`: connection state machine. Owns readyState-style state, `lastEventId`, the retry timer, reconnect policy and user callbacks. Talks to the network only through a transport vtable and a monotonic clock function. The C counterpart of the `eventsource` package.
- Transport ports: small adapters implementing the vtable. Shipped: ESP-IDF (`esp_http_client`) and POSIX (for host testing and non-embedded use).

### Client driving model: approaches considered

- Poll-driven state machine with a transport vtable (recommended). The application calls `sse_client_poll()` from its own task or loop; the library never creates threads or timers. Bounded blocking comes from the transport read timeout. Portable, testable with a mock transport, and gives the application full control over task priorities and stack sizes, which matters on FreeRTOS.
- Library-owned FreeRTOS task. Idiomatic on ESP-IDF and convenient, but not portable, hides stack sizing, and makes shutdown ordering harder. Rejected as the core model, but the ESP-IDF port ships a thin `sse_client_start_task()` convenience wrapper on top of the poll API in v1: it takes stack size and priority as arguments and pairs with `sse_client_request_stop()` for shutdown, so the core stays task-free.
- Pure sans-I/O (application feeds bytes and timestamps, library only returns instructions). The most testable design, but every user has to write the I/O pump themselves. The vtable model keeps nearly all of the testability (mock the vtable) at a fraction of the integration cost. Rejected.

## The parser (`sse_parser`)

### Design

A byte-oriented incremental state machine rather than a port of the JS parser's line-buffering design. The JS parser buffers partial lines and re-scans them because that is what is fast on a JS engine; in C the state machine is both simpler and stricter about memory:

- Parser state (which field we are in, CR pending, position within a field name) survives across `feed()` calls, so chunk boundaries need no special handling and no pending-line buffer exists at all.
- Bytes of `data:` values are appended directly into the caller-provided data buffer as they arrive. Multi-line data gets `\n` separators appended between lines, matching the spec's join semantics.
- Comment lines and unknown fields are skipped without buffering a single byte, so a malicious or misbehaving server cannot grow memory with garbage lines. This is the property the JS parser needs `maxBufferSize` plus discard-mode bookkeeping for; the byte machine gets it for free.
- Within a chunk, value scanning can use `memchr` for the next `\r`/`\n` and a single `memcpy` of the span, so the per-byte state machine only runs on structural bytes. This keeps throughput high without giving up boundary determinism. This is an internal optimization, not an API concern, and can land after correctness is proven.

States (roughly): `LINE_START`, `FIELD_NAME`, `AFTER_COLON` (optionally swallow one space), `VALUE` (routing to data/event/id/retry/comment/discard), `SAW_CR` (swallow one following LF). A 3-byte replay buffer at stream start handles the UTF-8 BOM (`EF BB BF`); if the first bytes are not exactly the BOM they are replayed through the machine.

### Field semantics (parity with the reference implementations)

- Blank line dispatches the block: `on_id` first (if a valid `id` field was seen in the block, even when the block has no data, matching `eventsource-parser`'s `onId`), then `on_event` if at least one `data` field was seen (so `data:` alone dispatches an empty-string event).
- `id` values containing a NUL byte are ignored. An `id` value longer than the id buffer is ignored with an error callback rather than truncated: a truncated id would silently corrupt resume semantics on reconnect.
- `event:` with an empty value resets the type to none. A value longer than the event-type buffer is ignored with an error callback (a truncated name would misroute messages).
- `retry` accepts ASCII digits only; anything else is ignored and reported. Values above `UINT32_MAX` ms are clamped.
- Field names are matched exactly (no case folding) against `data`, `event`, `id`, `retry`; everything else is reported once via the error callback and skipped.
- Comment lines are parsed and skipped without buffering, and there is no comment callback. Servers mostly use comments as heartbeats, and heartbeat detection needs no parser involvement: the client's idle timeout counts raw received bytes (see Liveness).

### Overflow policy

When a block's data exceeds the data buffer, the parser switches the block to a discarded state: remaining data bytes are skipped, `on_error` fires once with `SSE_PARSE_ERR_DATA_TOO_LARGE`, the event is not delivered, and the `id` from that block is NOT committed (so a resuming server will resend it rather than silently skipping past an undelivered message). `retry` fields in the block are still honored. Parsing resumes cleanly at the next blank line.

This deliberately differs from the JS parser, which terminates until `reset()`. A terminate-and-rethrow model makes sense in JS where the caller owns the read loop; inside an auto-reconnecting embedded client, discard-and-resync keeps the stream alive and lets the application decide in `on_error` whether to close.

### API sketch

```c
/* sse_parser.h - streaming SSE parser. No I/O, no clock, no heap. */

typedef struct {
  const char *event;    /* event type, NULL when the block had none */
  const char *id;       /* id committed in this block, NULL when none */
  const char *data;     /* NUL-terminated for convenience; may contain embedded NULs */
  size_t data_len;
} sse_parser_event_t;

typedef enum {
  SSE_PARSE_ERR_DATA_TOO_LARGE,
  SSE_PARSE_ERR_ID_INVALID,
  SSE_PARSE_ERR_EVENT_TYPE_TOO_LARGE,
  SSE_PARSE_ERR_INVALID_RETRY,
  SSE_PARSE_ERR_UNKNOWN_FIELD
} sse_parse_error_t;

typedef struct {
  void *userdata;
  void (*on_event)(void *ud, const sse_parser_event_t *ev);
  void (*on_id)(void *ud, const char *id, size_t len);          /* block end, before on_event */
  void (*on_retry)(void *ud, uint32_t retry_ms);
  void (*on_error)(void *ud, sse_parse_error_t err);
} sse_parser_callbacks_t;

typedef struct {
  char *data_buf;  size_t data_buf_len;   /* max message size, the big one */
  char *id_buf;    size_t id_buf_len;
  char *event_buf; size_t event_buf_len;
} sse_parser_buffers_t;

/* Struct declared in the header so it can live in static storage;
 * fields are private by convention. */
typedef struct sse_parser sse_parser_t;

void sse_parser_init(sse_parser_t *p, const sse_parser_callbacks_t *cb,
                    const sse_parser_buffers_t *buf);
void sse_parser_feed(sse_parser_t *p, const void *chunk, size_t len);
void sse_parser_reset(sse_parser_t *p);   /* call between connections */
```

Pointers passed to callbacks point into the parser's buffers and are valid only for the duration of the callback. Applications copy what they need to keep.

## The client (`sse_client`)

### State machine

States: `IDLE`, `CONNECTING`, `OPEN`, `WAITING_RETRY`, `CLOSED` (terminal). The flow follows `eventsource`'s `EventSource.ts` closely:

- `CONNECTING`: transport `open()` with headers `Accept: text/event-stream`, `Cache-Control: no-store`, `Last-Event-ID` (only when `lastEventId` is non-empty), plus user-supplied extra headers (auth tokens etc.).
- On response: HTTP 204 fires `on_error` with a "server requested stop" reason, then transitions to `CLOSED` (browser parity: 204 means never reconnect). A non-200 status or a content type not starting with `text/event-stream` fails the attempt and consults the reconnect policy (below). Otherwise: reset the parser, fire `on_open(reconnect_count)`, go `OPEN`.
- `OPEN`: read loop through `sse_client_poll()`. Every read's bytes go to `sse_parser_feed()`. EOF or read error transitions to `WAITING_RETRY` via the policy.
- `WAITING_RETRY`: fire `on_error` with the reason, `will_retry = true` and the computed delay (this is the "reconnecting" notification). When the deadline passes, back to `CONNECTING`.
- `sse_client_close()` from the polling task, or `sse_client_request_stop()` from any other task (atomic flag, observed within one read timeout), transitions to `CLOSED`. `on_closed` fires on every transition into `CLOSED`, whether user-initiated or via HTTP 204, so it is the single "this client is done" signal.

`reconnect_count` resets to zero after a connection that delivered at least one event, so applications can distinguish flapping from a healthy long-lived stream.

### Reconnect delay

- Base delay: the server-sent `retry:` value if one has been received, else `default_retry_ms` (3000, matching the spec and the JS client).
- Optional backoff for consecutive failed attempts: `delay = min(base << attempts, max_retry_ms)`, plus `jitter_pct` random jitter. Browsers wait a flat interval, but a fleet of unattended devices reconnecting in lockstep to a recovering server is exactly the thundering-herd case backoff and jitter exist for. Defaults: backoff on, capped at 30 s, 10% jitter. Setting `max_retry_ms = 0` gives strict flat-interval behavior.
- When the failed response carried a `Retry-After` header (typical on 429 and 503), that value replaces the computed delay for the next attempt.

### Reconnect policy

The browser spec says any non-200 response fails the connection permanently. That is correct for a browser tab a human can refresh, but too eager for a sensor in a ceiling: a proxy returning 502 during a deploy would permanently kill the stream until reboot. The opposite extreme (retry everything) is also wrong: a 404 or 401 is not going to fix itself, and hammering an endpoint that said no wastes battery and radio.

Default policy, by failure class:

- Retry: transport errors (connect/TLS/read), EOF and mid-stream disconnects, idle timeout, HTTP 5xx, HTTP 429 (inherently transient even without a header), and any other 4xx that carries a `Retry-After` header (the server is explicitly inviting another attempt).
- Stop permanently: HTTP 204 (spec: the server asked us to stop) and every other non-200 response, notably 4xx without `Retry-After`.
- Wrong content type on a 200: retry. On a device this is usually a captive portal or intercepting proxy rather than a server bug, and it clears when the network does.

Every failed attempt fires `on_error` with the status code before the policy is applied, so applications that can fix the cause get a chance to: refresh an auth token on 401 and force a retry through the `reconnect_policy` hook, or call `sse_client_close()` to be stricter than the default. The hook gets the final say on every failure, so custom schemes fit without the library growing an option per scenario. Retrying 5xx at all is still a documented deviation from the browser, which never retries any non-200.

### Liveness (embedded-specific)

Half-open TCP connections are routine on flaky Wi-Fi: the server is gone but the socket never errors, and a client blocked on a silent socket cannot tell a dead connection from a quiet stream. Well-behaved SSE servers solve their half of this by sending a comment heartbeat (`: keepalive`) when no real messages have flowed for a while; the client half of that contract is an idle timeout.

Config `idle_timeout_ms` (0 = disabled): if no bytes arrive for that long, the connection is treated as dead and the normal retry path runs (`on_error` with `SSE_ERR_IDLE_TIMEOUT`, then reconnect with `Last-Event-ID` so nothing is lost on a resuming server). The timer counts raw bytes off the transport, so comment heartbeats reset it even though comments never reach a callback. Rule of thumb: 2-3x the server's heartbeat interval, so 15-30 s heartbeats pair with a 45-60 s timeout. Off by default because the protocol does not mandate heartbeats, and a forced reconnect against a legitimately quiet server costs a TLS handshake, which is real CPU and battery on ESP32. The ESP32 example enables it.

### API sketch

```c
/* sse_client.h */

typedef struct {
  const char *event;          /* "message" when the server sent no event field */
  const char *data;           /* NUL-terminated; may contain embedded NULs */
  size_t data_len;
  const char *last_event_id;  /* current lastEventId, "" when none (browser parity) */
} sse_message_t;

typedef enum {
  SSE_ERR_TRANSPORT,        /* connect/read failure */
  SSE_ERR_HTTP_STATUS,      /* non-200 (status in .http_status) */
  SSE_ERR_BAD_CONTENT_TYPE,
  SSE_ERR_SERVER_STOP,      /* HTTP 204 */
  SSE_ERR_IDLE_TIMEOUT,
  SSE_ERR_MESSAGE_TOO_LARGE /* forwarded from the parser */
} sse_error_reason_t;

typedef struct {
  sse_error_reason_t reason;
  int http_status;         /* 0 when not applicable */
  bool will_retry;
  uint32_t retry_in_ms;    /* valid when will_retry */
} sse_error_t;

typedef struct {
  void *userdata;
  void (*on_open)(void *ud, unsigned reconnect_count);  /* 0 = first connect */
  void (*on_message)(void *ud, const sse_message_t *msg);
  void (*on_error)(void *ud, const sse_error_t *err);    /* also the "reconnecting" signal */
  void (*on_closed)(void *ud);                          /* terminal */
} sse_client_callbacks_t;

typedef struct {
  const char *url;                  /* borrowed; must outlive the client */
  const char *const *extra_headers; /* NULL-terminated "Name: value" strings, or NULL */

  uint32_t default_retry_ms;        /* 0 -> 3000 */
  uint32_t max_retry_ms;            /* backoff cap; 0 -> flat interval, spec-style */
  uint8_t  jitter_pct;
  uint32_t idle_timeout_ms;         /* 0 -> disabled */
  uint32_t read_timeout_ms;         /* max blocking time per poll, 0 -> 500 */
  bool     skip_content_type_check; /* false = require text/event-stream */

  sse_parser_buffers_t buffers;      /* caller-provided, see parser */
  uint8_t *rx_buf;  size_t rx_buf_len;  /* transport read scratch */

  sse_transport_t *transport;
  uint32_t (*now_ms)(void);         /* monotonic; ports provide a default */
  sse_client_callbacks_t callbacks;

  /* Optional policy override; NULL -> default policy described above. */
  bool (*reconnect_policy)(void *ud, const sse_error_t *err);
} sse_client_config_t;

typedef struct sse_client sse_client_t;   /* struct in header, fields private */

int      sse_client_init(sse_client_t *c, const sse_client_config_t *cfg);
uint32_t sse_client_poll(sse_client_t *c);       /* returns max ms until next call is needed */
void     sse_client_close(sse_client_t *c);      /* from the polling task */
void     sse_client_request_stop(sse_client_t *c); /* safe from other tasks */
const char *sse_client_last_event_id(const sse_client_t *c);
```

`sse_client_poll()` drives everything: connects, reads (blocking at most `read_timeout_ms` via the transport), feeds the parser, fires callbacks, and manages the retry deadline. Its return value is a sleep hint so tickless callers in `WAITING_RETRY` can sleep the full delay instead of spinning. All callbacks fire on the task calling `sse_client_poll()`; the library is single-threaded by design and `sse_client_request_stop()` is the only cross-task entry point.

### Usage sketch

```c
static char data_buf[8192], id_buf[128], event_buf[64];
static uint8_t rx_buf[1024];
static sse_client_t client;

static void on_message(void *ud, const sse_message_t *msg) {
  printf("[%s] %.*s\n", msg->event, (int)msg->data_len, msg->data);
}

void app_main(void) {
  sse_client_config_t cfg = {
    .url = "https://example.com/stream",
    .buffers = { data_buf, sizeof data_buf, id_buf, sizeof id_buf,
                 event_buf, sizeof event_buf },
    .rx_buf = rx_buf, .rx_buf_len = sizeof rx_buf,
    .idle_timeout_ms = 60000,
    .transport = sse_transport_esp_http_client(),
    .callbacks = { .on_message = on_message },
  };
  sse_client_init(&client, &cfg);
  for (;;) {
    uint32_t sleep_ms = sse_client_poll(&client);
    if (sleep_ms) vTaskDelay(pdMS_TO_TICKS(sleep_ms));
  }
}
```

## Transport ports

```c
typedef struct {
  const char *url;
  const char *const *headers;   /* composed by the client: Accept, Last-Event-ID, extras */
} sse_request_t;

typedef struct {
  int status_code;
  char content_type[64];        /* media type only, params stripped */
  int32_t retry_after_s;        /* Retry-After header value, -1 when absent */
} sse_response_info_t;

/* read() return values */
#define SSE_READ_TIMEOUT  0      /* no data yet, not an error */
#define SSE_READ_EOF     -1
#define SSE_READ_ERROR   -2

typedef struct sse_transport {
  void *ctx;
  int  (*open)(void *ctx, const sse_request_t *req, sse_response_info_t *out);
  int  (*read)(void *ctx, void *buf, size_t len, uint32_t timeout_ms);
  void (*close)(void *ctx);
} sse_transport_t;
```

Ports parse `Retry-After` in its delta-seconds form only; the HTTP-date form needs a wall clock and is reported as absent.

- ESP-IDF port: wraps `esp_http_client` in streaming mode, which brings TLS (with the certificate bundle), redirects and chunked decoding. Compiled only in ESP-IDF builds; the whole library is packaged as one IDF component (see Distribution).
- POSIX port: built on libcurl, primarily for host-side testing, examples and CI. TLS, chunked decoding and redirects come for free, and HTTP correctness on the host is not the point of this library, so owning a raw-socket HTTP client is not worth the code. Host-only dependency; nothing on the device links curl.
- Mock transport (test-only): scripted responses driving the client state machine deterministically.

The core never includes OS headers; ports own all platform dependencies.

## Memory model

- Zero heap allocation in `sse_parser` and `sse_client`. Every buffer is caller-provided, so `static` allocation works and worst-case memory is visible at compile time.
- RAM cost = the buffers you choose + small fixed structs (parser state well under 100 bytes, client state around 200 plus space for composing headers). With the suggested defaults (8 KiB data, 128 B id, 64 B event type, 1 KiB rx) a client costs roughly 9.5 KiB plus whatever the transport uses. TLS dominates real-world RAM on ESP32 and lives entirely in the transport.
- Code size target: a few KiB each for parser and client core (rough estimate, to be validated once real).

## Deviations from the browser EventSource (summary)

- C-idiomatic callback API instead of EventTarget; one callback set per client.
- Default reconnect policy retries transport failures, 5xx, 429 and `Retry-After`-bearing 4xx (spec says any non-200 fails permanently); other 4xx and 204 stop for good. Override hook available.
- Optional backoff and jitter on top of the `retry` interval (browsers wait a flat interval). Disable via `max_retry_ms = 0`.
- Oversized messages are dropped with an error and the stream continues; the block's id is not committed.
- Ids and event types that exceed their buffers are ignored (with an error), never truncated.
- Data is bytes + length, no UTF-8 validation; `event` defaults to `"message"` at the client layer (the parser reports it as NULL, matching `eventsource-parser`).
- Optional idle timeout, which the browser API does not have.
- No CORS/credentials/origin semantics; redirects are delegated to the transport.

## Testing strategy

- Parser unit tests on the host, porting the test corpus from `../parser/test` (field semantics, CR/LF/CRLF, BOM, NUL-in-id, invalid retry, comments, empty data dispatch).
- Chunk-boundary invariance as a property test: every chunking of a fixture stream must produce an identical callback sequence. Exhaustive splits for short fixtures, randomized splits for long ones. This is the single highest-value test for this kind of parser and directly encodes the "deterministic at every boundary" goal.
- Fuzzing with libFuzzer under ASan/UBSan: random bytes with random chunk splits; the parser must never touch memory out of bounds and never grow state.
- Client core tests against the scripted mock transport: 204 close, reconnect policy per status class (5xx retries, 4xx stops, 4xx with `Retry-After` retries using the header's delay), bad content type, mid-event disconnect (partial event must not leak into the next connection, i.e. parser reset), `retry:` honored, backoff progression, `Last-Event-ID` present only when non-empty, idle timeout, stop-from-other-task.
- Host integration test against a real SSE server (the dev servers in the sibling JS repos can serve as fixtures).
- ESP32: an example app doubling as an on-target smoke test; CI gate is at minimum an ESP-IDF cross-build of the component and examples.

## Repository layout

```
c/
  idf_component.yml     (repo root doubles as the ESP-IDF component)
  CMakeLists.txt        (dual-mode: IDF component or host build)
  LICENSE
  README.md
  include/eventsource/sse_parser.h
  include/eventsource/sse_client.h
  include/eventsource/sse_transport.h
  src/sse_parser.c
  src/sse_client.c
  ports/esp-idf/        (esp_http_client transport, ESP-IDF builds only)
  ports/posix/
  tests/                (host unit + property tests, mock transport)
  fuzz/
  examples/esp32/
  examples/posix/
```

The top-level CMakeLists is dual-mode: when `ESP_PLATFORM` is set it calls `idf_component_register()` with the core sources plus the esp_http_client transport and returns; otherwise it is a plain host project building the core, the POSIX port (requires libcurl) and the tests. Core compiles with `-std=c99 -Wall -Wextra -Werror`; sanitizers on in host test builds.

## Distribution

ESP-IDF Component Registry only, for now. Consumers run `idf.py add-dependency "<namespace>/eventsource"`; the component is fetched into `managed_components/` with semver resolution and pinned via the project's `dependencies.lock`. What this requires:

- The repo root is the component: `idf_component.yml` at the root (version, description, url, `dependencies:` declaring the supported IDF range and nothing else) plus the dual-mode CMakeLists above. Registry uploads pack the component directory, so the core sources must live inside it; this is why the root doubles as the component rather than `ports/esp-idf/`.
- `files:` exclude patterns in the manifest keep host-only content (`tests/`, `fuzz/`, `ports/posix/`, `examples/posix/`) out of the packed artifact.
- `LICENSE` and `README.md` at the root; the registry renders the README as the component page.
- Versioning by git tag; CI publishes on tag push via `espressif/upload-components-ci-action`. The namespace is the GitHub org hosting the repo.
- `examples/esp32/` follows registry conventions so `idf.py create-project-from-example` works and the example is browsable on the registry page.

Other channels (PlatformIO, CMake install/export for `find_package`, vcpkg/Conan, an amalgamated single-file pair for vendoring) are deliberately deferred. They are all additive metadata or CI artifacts on top of this layout, so nothing here blocks adding them later.

## Suggested defaults

- `data_buf`: application-chosen; examples use 8 KiB
- `id_buf`: 128 bytes, `event_buf`: 64 bytes, `rx_buf`: 1 KiB
- `default_retry_ms`: 3000, `max_retry_ms`: 30000, `jitter_pct`: 10
- `read_timeout_ms`: 500 (bounds stop latency and callback jitter)
- `idle_timeout_ms`: 0 (disabled; examples enable 60000)
- `skip_content_type_check`: false (named so that a zero-initialized config gets the strict default)
