---
"eventsource-c": patch
---

fix: map esp_http_client's -ESP_ERR_HTTP_EAGAIN to a read timeout in the ESP-IDF transport. Mid-stream read timeouts on quiet streams (any gap between events longer than read_timeout_ms) were treated as transport errors, putting the client into a permanent reconnect loop. Found on hardware against a real SSE endpoint; a QEMU regression scenario with a silent-start stream now covers it in CI.
