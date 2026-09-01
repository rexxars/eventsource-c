#ifndef SSE_TRANSPORT_CURL_H
#define SSE_TRANSPORT_CURL_H
#include "eventsource/sse_transport.h"
sse_transport_t *sse_transport_curl_new(void);
void sse_transport_curl_free(sse_transport_t *t);
#endif
