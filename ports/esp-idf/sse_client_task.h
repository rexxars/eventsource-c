#ifndef SSE_CLIENT_TASK_H
#define SSE_CLIENT_TASK_H
#include "eventsource/sse_client.h"
/* Runs sse_client_poll() in a FreeRTOS task. The task self-deletes when the
 * client reaches SSE_STATE_CLOSED (via sse_client_request_stop() or a
 * non-retryable failure). Returns 0 on success. */
int sse_client_start_task(sse_client_t *c, const char *name, uint32_t stack_bytes,
                          unsigned priority);
#endif
