---
"eventsource-c": minor
---

feat: add a LilyGO T-Display example (`examples/esp32-tdisplay`) that renders live SSE status on the board's ST7789 screen: connection state, event/drop counters, heap, and the latest event's type, id, and payload. Built on ESP-IDF's `esp_lcd` with a public-domain 8x8 font, no UI framework; rendering is throttled so high-rate streams do not starve the client task.
