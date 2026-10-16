#include "app_store.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "store";
static const char *NS = "whisport";

/* storage 分区未初始化时的 NVS 句柄降级逻辑见 app_store_init() */
static nvs_handle_t s_nvs;

esp_err_t app_store_init(void) {
  const char *part = "storage";
  esp_err_t err = nvs_flash_init_partition(part);
  if (err == ESP_ERR_NOT_FOUND) {
    /* 自定义分区表缺失（第三方工程未采用 partitions.csv）时退回默认 nvs 分区 */
    ESP_LOGW(TAG, "partition 'storage' not found, falling back to default nvs");
    part = NVS_DEFAULT_PART_NAME;
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      err = nvs_flash_init();
    }
  }
  if (err != ESP_OK) return err;
  return nvs_open_from_partition(part, NS, NVS_READWRITE, &s_nvs);
}

esp_err_t app_store_set_secret(const char *secret_hex) {
  esp_err_t err = nvs_set_str(s_nvs, "secret", secret_hex);
  if (err == ESP_OK) err = nvs_commit(s_nvs);
  return err;
}

bool app_store_get_secret(char *buf, size_t buflen) {
  if (buflen < APP_SECRET_HEX_LEN) return false;
  size_t len = buflen;
  if (nvs_get_str(s_nvs, "secret", buf, &len) != ESP_OK) return false;
  return len == 64;
}

void app_store_clear_secret(void) {
  nvs_erase_key(s_nvs, "secret");
  nvs_commit(s_nvs);
}

int64_t app_store_get_cursor(void) {
  int64_t v = 0;
  nvs_get_i64(s_nvs, "cursor", &v);
  return v;
}

void app_store_set_cursor(int64_t cursor) {
  nvs_set_i64(s_nvs, "cursor", cursor);
  nvs_commit(s_nvs);
}

int64_t app_store_get_last_read(void) {
  int64_t v = 0;
  nvs_get_i64(s_nvs, "last_read", &v);
  return v;
}

void app_store_set_last_read(int64_t id) {
  nvs_set_i64(s_nvs, "last_read", id);
  nvs_commit(s_nvs);
}
