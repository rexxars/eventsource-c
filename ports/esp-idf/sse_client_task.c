#include "sse_client_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void sse_task_main(void *arg) {
  sse_client_t *c = arg;
  while (sse_client_state(c) != SSE_STATE_CLOSED) {
    uint32_t s = sse_client_poll(c);
    if (s == UINT32_MAX) break;
    if (s > 0) {
      if (s > 250) s = 250; /* stay responsive to sse_client_request_stop() */
      vTaskDelay(pdMS_TO_TICKS(s));
    }
  }
  vTaskDelete(NULL);
}

int sse_client_start_task(sse_client_t *c, const char *name, uint32_t stack_bytes,
                          unsigned priority) {
  BaseType_t ok = xTaskCreate(sse_task_main, name, stack_bytes / sizeof(StackType_t), c,
                              (UBaseType_t)priority, NULL);
  return ok == pdPASS ? 0 : -1;
}
