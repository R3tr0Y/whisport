#include "app_net.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool s_connected = false;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        s_connected = false;
        ESP_LOGW(TAG, "disconnected, auto-reconnecting");
        esp_wifi_connect(); /* IDF 内建指数退避由 retry 计数实现，此处简化为立即重试 */
        break;
      }
      default:
        break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
    s_connected = true;
    if (s_events) xEventGroupSetBits(s_events, GOT_IP_BIT);
  }
}

esp_err_t app_net_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  s_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

  wifi_config_t wifi = {0};
  strlcpy((char *)wifi.sta.ssid, CONFIG_WS_WIFI_SSID, sizeof(wifi.sta.ssid));
  strlcpy((char *)wifi.sta.password, CONFIG_WS_WIFI_PASS, sizeof(wifi.sta.password));
  wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi sta started, connecting to \"%s\"", CONFIG_WS_WIFI_SSID);
  return ESP_OK;
}

esp_err_t app_net_wait_connected(TickType_t timeout) {
  EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, timeout);
  return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool app_net_is_connected(void) {
  return s_connected;
}
