/* QEMU on-target integration test for the eventsource ESP-IDF port.
 *
 * Runs inside qemu-system-xtensa with the OpenCores ethernet MAC and talks
 * to tools/sse_fixture_server.py on the host (10.0.2.2 under QEMU user
 * networking). Scenarios print "QEMU-TEST ..." markers that
 * tests/qemu/pytest_qemu.py asserts on, covering the behaviors the host
 * suites cannot reach: the esp_http_client EOF-vs-timeout mapping
 * (close-delimited and chunked), Last-Event-ID resume, redirect following
 * with request headers persisting across the reopen, and the cross-origin
 * redirect refusal.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "eventsource/sse_client.h"
#include "sse_transport_esp.h"

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define FIXTURE_BASE "http://10.0.2.2:8085" /* host, via QEMU user networking */

static EventGroupHandle_t net_events;
#define GOT_IP_BIT BIT0

static char db[4096], ib[128], eb[64];
static uint8_t rx[1024];
static sse_client_t client;

typedef struct {
  const char *name;
  int msgs;
  uint32_t last_msg_ms;
} scen_t;
static scen_t scen;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void on_open(void *ud, unsigned rc) {
  (void)ud;
  printf("QEMU-TEST open name=%s count=%u\n", scen.name, rc);
}

static void on_message(void *ud, const sse_message_t *m) {
  (void)ud;
  scen.msgs++;
  scen.last_msg_ms = now_ms();
  printf("QEMU-TEST msg name=%s id=%s data=%.*s\n", scen.name, m->last_event_id,
         (int)m->data_len, m->data);
}

static void on_error(void *ud, const sse_error_t *e) {
  (void)ud;
  /* latency since the last message exposes a broken EOF mapping: a prompt
   * EOF arrives within the read timeout, a misreported one only when the
   * idle timeout (15 s here) fires. */
  uint32_t lat = scen.last_msg_ms ? now_ms() - scen.last_msg_ms : 0;
  printf("QEMU-TEST err name=%s reason=%d status=%d retry=%d latency_ms=%" PRIu32 "\n",
         scen.name, (int)e->reason, e->http_status, (int)e->will_retry, lat);
}

static void on_closed(void *ud) {
  (void)ud;
  printf("QEMU-TEST closed name=%s\n", scen.name);
}

typedef bool (*until_fn)(void);
static bool six_msgs(void) { return scen.msgs >= 6; }
static bool one_msg(void) { return scen.msgs >= 1; }
static bool never(void) { return false; }

static void run_scenario(const char *name, const char *url,
                         const char *const *extra_headers, until_fn until) {
  memset(&scen, 0, sizeof scen);
  scen.name = name;

  sse_transport_t *tr = sse_transport_esp_http_client();
  if (!tr) {
    printf("QEMU-TEST fatal name=%s transport alloc failed\n", name);
    return;
  }

  sse_client_config_t cfg = {0};
  cfg.url = url;
  cfg.extra_headers = extra_headers;
  cfg.buffers.data_buf = db;
  cfg.buffers.data_buf_len = sizeof db;
  cfg.buffers.id_buf = ib;
  cfg.buffers.id_buf_len = sizeof ib;
  cfg.buffers.event_buf = eb;
  cfg.buffers.event_buf_len = sizeof eb;
  cfg.rx_buf = rx;
  cfg.rx_buf_len = sizeof rx;
  cfg.default_retry_ms = 500;  /* fast reconnects keep the test short */
  cfg.idle_timeout_ms = 15000; /* must never be what detects EOF */
  cfg.read_timeout_ms = 250;
  cfg.transport = tr;
  cfg.now_ms = now_ms;
  cfg.callbacks.on_open = on_open;
  cfg.callbacks.on_message = on_message;
  cfg.callbacks.on_error = on_error;
  cfg.callbacks.on_closed = on_closed;

  if (sse_client_init(&client, &cfg) != 0) {
    printf("QEMU-TEST fatal name=%s init failed\n", name);
    sse_transport_esp_http_client_free(tr);
    return;
  }

  uint32_t deadline = now_ms() + 30000;
  while (sse_client_state(&client) != SSE_STATE_CLOSED) {
    if (until() ) sse_client_close(&client);
    if ((int32_t)(now_ms() - deadline) > 0) {
      printf("QEMU-TEST timeout name=%s\n", name);
      sse_client_close(&client);
    }
    uint32_t s = sse_client_poll(&client);
    if (s == UINT32_MAX) break;
    if (s > 50) s = 50;
    if (s) vTaskDelay(pdMS_TO_TICKS(s));
  }
  sse_transport_esp_http_client_free(tr);
  printf("QEMU-TEST done name=%s msgs=%d\n", name, scen.msgs);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)base;
  (void)id;
  (void)data;
  xEventGroupSetBits(net_events, GOT_IP_BIT);
}

static void net_up_openeth(void) {
  net_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(
      esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.reset_gpio_num = -1;
  phy_config.autonego_timeout_ms = 100;
  esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
  esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth = NULL;
  ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth));
  esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t *netif = esp_netif_new(&netif_cfg);
  ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
  ESP_ERROR_CHECK(esp_eth_start(eth));

  xEventGroupWaitBits(net_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

void app_main(void) {
  net_up_openeth();
  printf("QEMU-TEST net up\n");

  static const char *echo_headers[] = {"X-Test: qemu-marker", NULL};

  /* Close-delimited EOF + Last-Event-ID resume across the reconnect. */
  run_scenario("eof", FIXTURE_BASE "/limited", NULL, six_msgs);
  /* Chunked EOF (terminating zero chunk). */
  run_scenario("eofchunked", FIXTURE_BASE "/limited-chunked", NULL, six_msgs);
  /* Same-origin redirect; the echoed header proves request headers
   * persisted across the redirect reopen. */
  run_scenario("redirect", FIXTURE_BASE "/redirect-echo", echo_headers, one_msg);
  /* Cross-origin redirect must be refused, surfacing the 302. */
  run_scenario("xorigin", FIXTURE_BASE "/xorigin", NULL, never);

  printf("QEMU-TEST all done\n");
}
