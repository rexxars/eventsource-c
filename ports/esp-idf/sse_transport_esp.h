#ifndef SSE_TRANSPORT_ESP_H
#define SSE_TRANSPORT_ESP_H
#include "eventsource/sse_transport.h"
sse_transport_t *sse_transport_esp_http_client(void);
void sse_transport_esp_http_client_free(sse_transport_t *t);
#endif
