# eventsource-c

## 0.2.0

### Minor Changes

- drop events with unrepresentable types and resume past dropped events. An `event:` name that does not fit `event_buf` (or contains a NUL byte) now discards that event entirely, surfaced as the new `SSE_ERR_EVENT_TYPE_INVALID` error reason, instead of delivering the payload under the default `"message"` type. Dropped events (whether by type or payload size) now commit their `id`, so reconnects resume past them instead of replaying an event that can never fit; the `on_error` callback remains the data-loss signal. ([#3](https://github.com/rexxars/eventsource-c/pull/3)) ([465bbc1](https://github.com/rexxars/eventsource-c/commit/465bbc14115a9a85bec676ab39c36610386bacc7))
- add a LilyGO T-Display example (`examples/esp32-tdisplay`) that renders live SSE status on the board's ST7789 screen: connection state, event/drop counters, heap, and the latest event's type, id, and payload. Built on ESP-IDF's `esp_lcd` with a public-domain 8x8 font, no UI framework; rendering is throttled so high-rate streams do not starve the client task. ([#6](https://github.com/rexxars/eventsource-c/pull/6)) ([be3ec4d](https://github.com/rexxars/eventsource-c/commit/be3ec4da7eee9467f673f4b79fa385ce9ba6bb0a))

### Patch Changes

- map esp_http_client's -ESP_ERR_HTTP_EAGAIN to a read timeout in the ESP-IDF transport. Mid-stream read timeouts on quiet streams (any gap between events longer than read_timeout_ms) were treated as transport errors, putting the client into a permanent reconnect loop. Found on hardware against a real SSE endpoint; a QEMU regression scenario with a silent-start stream now covers it in CI. ([#5](https://github.com/rexxars/eventsource-c/pull/5)) ([02c5df7](https://github.com/rexxars/eventsource-c/commit/02c5df7d3e254f131a7913bf2ee7dbf2b3477d39))

## 0.1.0

### Minor Changes

- Initial release: allocation-free C99 Server-Sent Events (EventSource) client with spec-accurate parsing, automatic reconnection (`retry`/`Retry-After`, backoff, jitter), `Last-Event-ID` resume, idle-timeout dead-connection detection, same-origin-only redirect handling, an ESP-IDF port (esp_http_client transport plus FreeRTOS task wrapper), and a libcurl POSIX port. ([254f3f9](https://github.com/rexxars/eventsource-c/commit/254f3f9597c193c4abde9e7be7af3bf481f55805))
