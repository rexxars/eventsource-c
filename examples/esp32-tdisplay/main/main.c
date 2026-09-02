/* SSE status display for the LilyGO T-Display: streams a server-sent-events
 * endpoint and shows connection state, event count, and the latest event on
 * the built-in ST7789.
 *
 * All display updates after startup happen from the SSE client's callbacks,
 * which run on the single client task, so no locking is needed. */
#include "eventsource/sse_client.h"
#include "sse_client_task.h"
#include "sse_transport_esp.h"

#include "cJSON.h"
#include "display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "sse_tdisplay";

/* id_buf tracks the raised SSE_CLIENT_ID_MAX (see the project CMakeLists);
 * init requires id_buf_len <= SSE_CLIENT_ID_MAX + 1. */
static char db[8192], ib[SSE_CLIENT_ID_MAX + 1], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

static EventGroupHandle_t wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- status model ---- */

static struct {
  char wifi[28];
  char sse[28];
  unsigned events;
  unsigned dropped; /* oversized events discarded by the parser */
  char last_event[28];
  char last_id[101]; /* display keeps a prefix; render ellipsizes further */
  char last_data[28 * 3 + 1];
  uint16_t sse_color;
  uint32_t last_render_ms;
} st;

static void copy_trunc(char *dst, size_t cap, const char *src, size_t len) {
  size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

/* Copies src into dst, replacing the tail with "..." when it exceeds
 * `cols` characters. dst must hold cols + 1 bytes. */
static void ellipsize(char *dst, size_t cols, const char *src) {
  size_t len = strlen(src);
  if (len <= cols) {
    memcpy(dst, src, len + 1);
    return;
  }
  memcpy(dst, src, cols - 3);
  memcpy(dst + cols - 3, "...", 4);
}

static void render(void) {
  char line[40];
  display_clear();
  display_text(2, 2, 2, COL_CYAN, "eventsource-c");
  display_text(2, 22, 1, COL_WHITE, st.wifi);
  display_text(2, 34, 1, st.sse_color, st.sse);
  snprintf(line, sizeof line, "ev:%u drop:%u heap:%u", st.events, st.dropped,
           (unsigned)esp_get_free_heap_size());
  display_text(2, 46, 1, COL_WHITE, line);
  snprintf(line, sizeof line, "type: %s", st.last_event);
  display_text(2, 58, 1, COL_YELLOW, line);
  {
    /* Wikimedia-style ids are mostly-constant JSON where only an embedded
     * timestamp changes; show that slice so the line visibly updates. */
    const char *ts = strstr(st.last_id, "\"timestamp\":");
    if (ts) {
      ts += strlen("\"timestamp\":");
      snprintf(line, sizeof line, "id: ...%.*s...",
               (int)strspn(ts, "0123456789"), ts);
    } else {
      char idpart[27]; /* 30-column display minus the "id: " prefix */
      ellipsize(idpart, 26, st.last_id);
      snprintf(line, sizeof line, "id: %s", idpart);
    }
    display_text(2, 70, 1, COL_GREY, line);
  }
  /* last_data wrapped over three 28-char rows */
  size_t len = strlen(st.last_data);
  for (int row = 0; row < 3; row++) {
    size_t off = (size_t)row * 28;
    if (off >= len) break;
    char chunk[29];
    copy_trunc(chunk, sizeof chunk, st.last_data + off, len - off);
    display_text(2, 86 + row * 12, 1, COL_GREEN, chunk);
  }
  display_flush();
}

/* ---- SSE callbacks ---- */

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  snprintf(st.sse, sizeof st.sse, "sse: open (reconnects: %u)", rc);
  st.sse_color = COL_GREEN;
  ESP_LOGI(TAG, "open (reconnect_count=%u)", rc);
  render();
}

/* For JSON payloads with "user" and "title" string fields (e.g. Wikimedia's
 * recentchange stream), summarize as "[user] title" - it demos far better
 * than the payload's constant prefix. Anything else keeps the raw prefix. */
static void summarize_data(const char *data, size_t len) {
  cJSON *root = cJSON_Parse(data);
  if (root) {
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(root, "user");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    if (cJSON_IsString(user) && user->valuestring && cJSON_IsString(title) &&
        title->valuestring) {
      snprintf(st.last_data, sizeof st.last_data, "[%s] %s", user->valuestring,
               title->valuestring);
      cJSON_Delete(root);
      return;
    }
    cJSON_Delete(root);
  }
  copy_trunc(st.last_data, sizeof st.last_data, data, len);
}

static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  st.events++;
  copy_trunc(st.last_event, sizeof st.last_event, m->event, strlen(m->event));
  copy_trunc(st.last_id, sizeof st.last_id, m->last_event_id,
             strlen(m->last_event_id));
  summarize_data(m->data, m->data_len);
  ESP_LOGI(TAG, "[%s] %u bytes: %s", m->event, (unsigned)m->data_len,
           st.last_data);
  /* Throttle rendering: a full-frame flush per event would fall behind on
   * busy streams (Wikimedia's recentchange peaks at dozens per second). */
  if ((uint32_t)(now_ms() - st.last_render_ms) >= 250) {
    st.last_render_ms = now_ms();
    render();
  }
}

static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  if (e->reason == SSE_ERR_MESSAGE_TOO_LARGE) {
    /* Informational: the parser dropped an event exceeding data_buf and the
     * stream continues. Count it instead of overwriting the status line. */
    st.dropped++;
    ESP_LOGW(TAG, "oversized event dropped (%u total)", st.dropped);
    return;
  }
  if (e->will_retry) {
    snprintf(st.sse, sizeof st.sse, "sse: retry in %ums (r%d)",
             (unsigned)e->retry_in_ms, (int)e->reason);
    st.sse_color = COL_YELLOW;
  } else {
    snprintf(st.sse, sizeof st.sse, "sse: failed (r%d s%d)", (int)e->reason,
             e->http_status);
    st.sse_color = COL_RED;
  }
  ESP_LOGW(TAG, "error reason=%d status=%d will_retry=%d retry_in=%ums",
           (int)e->reason, e->http_status, (int)e->will_retry,
           (unsigned)e->retry_in_ms);
  render();
}

static void on_closed(void *ud) {
  (void)ud;
  snprintf(st.sse, sizeof st.sse, "sse: closed");
  st.sse_color = COL_RED;
  ESP_LOGI(TAG, "closed");
  render();
}

/* ---- Wi-Fi ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data) {
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
  strncpy((char *)wc.sta.ssid, CONFIG_SSE_EXAMPLE_WIFI_SSID,
          sizeof wc.sta.ssid - 1);
  strncpy((char *)wc.sta.password, CONFIG_SSE_EXAMPLE_WIFI_PASSWORD,
          sizeof wc.sta.password - 1);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                      portMAX_DELAY);
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  display_init();

  snprintf(st.wifi, sizeof st.wifi, "wifi: joining %.14s...",
           CONFIG_SSE_EXAMPLE_WIFI_SSID);
  snprintf(st.sse, sizeof st.sse, "sse: waiting for wifi");
  st.sse_color = COL_GREY;
  snprintf(st.last_event, sizeof st.last_event, "-");
  snprintf(st.last_id, sizeof st.last_id, "-");
  render();

  wifi_connect_blocking();
  snprintf(st.wifi, sizeof st.wifi, "wifi: %.20s", CONFIG_SSE_EXAMPLE_WIFI_SSID);
  snprintf(st.sse, sizeof st.sse, "sse: connecting...");
  render();

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
  ESP_ERROR_CHECK(sse_client_start_task(&client, "sse", 6144, 5) == 0
                      ? ESP_OK
                      : ESP_FAIL);
}
