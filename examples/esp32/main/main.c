#include "eventsource/sse_client.h"
#include "sse_client_task.h"
#include "sse_transport_esp.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "sse_example";

static char db[8192], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  ESP_LOGI(TAG, "open (reconnect_count=%u)", rc);
}
static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  ESP_LOGI(TAG, "[%s] (id=%s) %.*s", m->event, m->last_event_id, (int)m->data_len, m->data);
}
static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  ESP_LOGW(TAG, "error reason=%d status=%d will_retry=%d retry_in=%ums", (int)e->reason,
           e->http_status, (int)e->will_retry, (unsigned)e->retry_in_ms);
}
static void on_closed(void *ud) {
  (void)ud;
  ESP_LOGI(TAG, "closed");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
  }
}

static void wifi_connect_blocking(void) {
  wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                             wifi_event_handler, NULL));
  wifi_config_t wc = {0};
  strncpy((char *)wc.sta.ssid, CONFIG_SSE_EXAMPLE_WIFI_SSID, sizeof wc.sta.ssid - 1);
  strncpy((char *)wc.sta.password, CONFIG_SSE_EXAMPLE_WIFI_PASSWORD,
          sizeof wc.sta.password - 1);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
  ESP_LOGI(TAG, "wifi connected");
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  wifi_connect_blocking();

  sse_transport_t *tr = sse_transport_esp_http_client();
  sse_client_config_t cfg = {0};
  cfg.url = CONFIG_SSE_EXAMPLE_URL;
  cfg.buffers.data_buf = db;
  cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;
  cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb;
  cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx;
  cfg.rx_buf_len = sizeof rx;
  cfg.max_retry_ms = 30000;
  cfg.jitter_pct = 10;
  cfg.idle_timeout_ms = 60000;
  cfg.transport = tr;
  cfg.now_ms = now_ms;
  cfg.callbacks.on_open = on_open;
  cfg.callbacks.on_message = on_message;
  cfg.callbacks.on_error = on_error;
  cfg.callbacks.on_closed = on_closed;

  ESP_ERROR_CHECK(sse_client_init(&client, &cfg) == 0 ? ESP_OK : ESP_FAIL);
  ESP_ERROR_CHECK(sse_client_start_task(&client, "sse", 6144, 5) == 0 ? ESP_OK : ESP_FAIL);
}
