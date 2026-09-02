---
"eventsource-c": minor
---

feat: drop events with unrepresentable types and resume past dropped events. An `event:` name that does not fit `event_buf` (or contains a NUL byte) now discards that event entirely, surfaced as the new `SSE_ERR_EVENT_TYPE_INVALID` error reason, instead of delivering the payload under the default `"message"` type. Dropped events (whether by type or payload size) now commit their `id`, so reconnects resume past them instead of replaying an event that can never fit; the `on_error` callback remains the data-loss signal.
