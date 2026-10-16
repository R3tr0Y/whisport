#include "app_net.h"

#include <string.h>
#include <time.h>

#include "app_identity.h"
#include "app_store.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool s_connected = false;
static bool s_sntp_started = false;
static bool s_ap_active = false;
static esp_netif_t *s_sta_netif = NULL;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
      case WIFI_EVENT_STA_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected, reconnecting");
        esp_wifi_connect();
        break;
      default:
        break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
    s_connected = true;
    if (!s_sntp_started) {
      /* 首次拿到 IP 后启动 SNTP：HMAC 签名时间戳依赖系统时钟（云端 ±120s 窗口） */
      esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
      if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "sntp started");
      } else {
        ESP_LOGW(TAG, "sntp init failed");
      }
    }
    if (s_events) xEventGroupSetBits(s_events, GOT_IP_BIT);
  }
}

esp_err_t app_net_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  s_events = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

  /* 开发种子：NVS 无凭据且 Kconfig 非空时写入（正式用户走配网页） */
  char ssid[APP_NET_SSID_MAX], pass[APP_NET_PASS_MAX];
  if (!app_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) &&
      CONFIG_WS_WIFI_SSID[0] != '\0') {
    ESP_LOGI(TAG, "seeding wifi creds from Kconfig (dev)");
    app_store_set_wifi(CONFIG_WS_WIFI_SSID, CONFIG_WS_WIFI_PASS);
  }
  return ESP_OK;
}

bool app_net_has_creds(void) {
  char ssid[APP_NET_SSID_MAX], pass[APP_NET_PASS_MAX];
  return app_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
}

static esp_err_t apply_sta_config(const char *ssid, const char *pass) {
  wifi_config_t wifi = {0};
  strlcpy((char *)wifi.sta.ssid, ssid, sizeof(wifi.sta.ssid));
  strlcpy((char *)wifi.sta.password, pass ? pass : "", sizeof(wifi.sta.password));
  wifi.sta.threshold.authmode = WIFI_AUTH_OPEN;
  return esp_wifi_set_config(WIFI_IF_STA, &wifi);
}

esp_err_t app_net_start_sta(void) {
  char ssid[APP_NET_SSID_MAX], pass[APP_NET_PASS_MAX];
  if (!app_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
    return ESP_ERR_NOT_FOUND;
  }
  ESP_RETURN_ON_ERROR(apply_sta_config(ssid, pass), TAG, "sta config");

  wifi_mode_t mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "set mode");
  esp_wifi_start(); /* 已启动返回错误，忽略 */
  esp_wifi_connect();
  ESP_LOGI(TAG, "sta started, connecting to \"%s\"", ssid);
  return ESP_OK;
}

/**
 * 保存凭据并（重）连（配网页/console 调用）。
 * 注意：驱动处于"连接中"时不能 set_config（否则返回错误）——先 disconnect 并等待；
 * 全程不得用 ESP_ERROR_CHECK（网络调用失败会 abort 重启，曾致配网页改 Wi-Fi 即崩）。
 */
esp_err_t app_net_start_sta_creds(const char *ssid, const char *pass) {
  ESP_RETURN_ON_ERROR(app_store_set_wifi(ssid, pass ? pass : ""), TAG, "save wifi");

  esp_wifi_disconnect();
  vTaskDelay(pdMS_TO_TICKS(150));

  wifi_mode_t mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
  esp_err_t err = esp_wifi_set_mode(mode);
  if (err != ESP_OK) return err;
  err = apply_sta_config(ssid, pass);
  if (err != ESP_OK) return err;

  esp_wifi_start(); /* 已在运行属正常路径 */
  xEventGroupClearBits(s_events, GOT_IP_BIT);
  esp_wifi_connect();
  ESP_LOGI(TAG, "sta reconfigured -> \"%s\"", ssid);
  return ESP_OK;
}

const char *app_net_ap_ssid(void) {
  static char s_ap_ssid[APP_NET_AP_SSID_MAX];
  if (s_ap_ssid[0] == '\0') {
    char device_id[APP_DEVICE_ID_LEN];
    app_identity_device_id(device_id);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "whisport-%.4s", device_id);
  }
  return s_ap_ssid;
}

const char *app_net_ap_pass(void) {
  /* S2：开机随机 8 位（十六进制）——不可由广播的 MAC/device_id 推导；屏幕展示给用户 */
  static char s_ap_pass[9];
  if (s_ap_pass[0] == '\0') {
    uint32_t r = esp_random();
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08x", (unsigned)r);
  }
  return s_ap_pass;
}

esp_err_t app_net_start_ap(const char *ssid, const char *pass) {
  static bool netif_created = false;
  if (!netif_created) {
    esp_netif_create_default_wifi_ap();
    netif_created = true;
  }
  wifi_config_t ap = {0};
  strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
  ap.ap.ssid_len = strlen(ssid);
  ap.ap.channel = 1;
  ap.ap.max_connection = 2;
  if (pass && pass[0]) {
    strlcpy((char *)ap.ap.password, pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    ap.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
  esp_err_t err = esp_wifi_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    return err;
  }
  s_ap_active = true;
  ESP_LOGI(TAG, "ap started: ssid=%s", ssid);
  return ESP_OK;
}

void app_net_stop_ap(void) {
  if (!s_ap_active) return;
  s_ap_active = false;
  esp_wifi_set_mode(WIFI_MODE_STA);
  ESP_LOGI(TAG, "ap stopped");
}

bool app_net_get_ip(char *buf, size_t len) {
  if (!s_sta_netif) return false;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.ip.addr == 0) return false;
  snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
  return true;
}

int app_net_scan(app_net_ap_t *out, int max) {
  wifi_scan_config_t sc = {0};
  if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;

  uint16_t count = max;
  wifi_ap_record_t recs[16];
  if (count > 16) count = 16;
  if (esp_wifi_scan_get_ap_records(&count, recs) != ESP_OK) return 0;

  int n = 0;
  for (int i = 0; i < count && n < max; i++) {
    if (recs[i].ssid[0] == '\0') continue;
    strlcpy(out[n].ssid, (const char *)recs[i].ssid, sizeof(out[n].ssid));
    out[n].rssi = recs[i].rssi;
    out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
    n++;
  }
  return n;
}

esp_err_t app_net_wait_connected(TickType_t timeout) {
  EventBits_t bits = xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, timeout);
  return (bits & GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool app_net_wait_time(TickType_t timeout) {
  if (time(NULL) > 1700000000) return true; /* 已是 2023 年后，视为已同步 */
  if (!s_sntp_started) return false;
  esp_err_t err = esp_netif_sntp_sync_wait(timeout);
  return err == ESP_OK && time(NULL) > 1700000000;
}

bool app_net_is_connected(void) {
  return s_connected;
}
