---
"eventsource-c": minor
---

Initial release: allocation-free C99 Server-Sent Events (EventSource) client with spec-accurate parsing, automatic reconnection (`retry`/`Retry-After`, backoff, jitter), `Last-Event-ID` resume, idle-timeout dead-connection detection, same-origin-only redirect handling, an ESP-IDF port (esp_http_client transport plus FreeRTOS task wrapper), and a libcurl POSIX port.
