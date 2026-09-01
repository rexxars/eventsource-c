# eventsource-c

Server-Sent Events (EventSource) client for embedded C. Spec-accurate SSE parsing, automatic reconnection with `retry`/`Retry-After` support, `Last-Event-ID` resume, optional idle timeout for dead-connection detection, and zero heap allocation in the core. Primary target: ESP32 under ESP-IDF; the core is portable C99 and also builds on POSIX.

## Install (ESP-IDF)

    idf.py add-dependency "rexxars/eventsource"

## Usage

    #include "eventsource/sse_client.h"
    #include "sse_client_task.h"
    #include "sse_transport_esp.h"

    static char db[8192], ib[128], eb[64];
    static uint8_t rx[1024];
    static sse_client_t client;

    static void on_message(void *ud, const sse_message_t *m) {
      printf("[%s] %.*s\n", m->event, (int)m->data_len, m->data);
    }

    void start_stream(void) {
      sse_client_config_t cfg = {0};
      cfg.url = "https://example.com/stream";
      cfg.buffers.data_buf = db;  cfg.buffers.data_buf_len = sizeof db;
      cfg.buffers.id_buf = ib;    cfg.buffers.id_buf_len = sizeof ib;
      cfg.buffers.event_buf = eb; cfg.buffers.event_buf_len = sizeof eb;
      cfg.rx_buf = rx;            cfg.rx_buf_len = sizeof rx;
      cfg.max_retry_ms = 30000;
      cfg.jitter_pct = 10;
      cfg.idle_timeout_ms = 60000;
      cfg.transport = sse_transport_esp_http_client();
      cfg.now_ms = /* esp_timer_get_time()/1000 wrapper */;
      cfg.callbacks.on_message = on_message;
      sse_client_init(&client, &cfg);
      sse_client_start_task(&client, "sse", 6144, 5);
    }

See `examples/esp32` for a complete application and `examples/posix` for the host variant (libcurl).

## Buffer sizing

- `data_buf` caps the event payload: a buffer of N bytes holds events up to N-1 bytes. Oversized events are dropped with `SSE_ERR_MESSAGE_TOO_LARGE` and the stream continues.
- `id_buf`/`event_buf`: 128 and 64 bytes are good defaults.
- All buffers are caller-provided; the core never allocates.

## Reconnect behavior

Retries transport failures, HTTP 5xx, 429, and any 4xx carrying `Retry-After`. Stops permanently on HTTP 204 and other 4xx. Override per-failure with the `reconnect_policy` hook. Delays honor the server's `retry:` field, with optional exponential backoff (`max_retry_ms`) and jitter (`jitter_pct`).

## License

MIT
