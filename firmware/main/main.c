/**
 * whisport 固件入口（M1：身份链路 + 公告板）
 *
 * 流程：NVS → 网络 → （无凭证则配对/重新激活）→ 周期同步公告。
 * 控制台命令：pair <配对码> / sync / status（见 app_ui）。
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_api.h"
#include "app_identity.h"
#include "app_net.h"
#include "app_store.h"
#include "app_ui.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

static int do_sync_announcements(void); /* do_pair 中先行调用 */

#define SYNC_INTERVAL_MS (5 * 60 * 1000)   /* 公告同步周期 */
#define REACT_POST_INTERVAL_US (3600LL * 1000 * 1000) /* 重新激活请求最多每小时一次 */

static int64_t s_last_react_post_us = -REACT_POST_INTERVAL_US;

static void print_status(void) {
  char device_id[APP_DEVICE_ID_LEN];
  app_identity_device_id(device_id);
  app_ui_show_status(device_id, app_identity_has_secret(), app_net_is_connected());
}
void whisport_print_status(void) { print_status(); }

/* ---------- 认领与重新激活 ---------- */

static int do_pair(const char *code) {
  char mac[13];
  app_identity_mac(mac);

  char body[160];
  snprintf(body, sizeof(body),
           "{\"pairing_code\":\"%s\",\"mac\":\"%s\",\"label\":\"AI Passport\"}", code, mac);

  app_api_resp_t r;
  esp_err_t err = app_api_anon_post("/api/devices/claim", body, &r);
  if (err != ESP_OK) {
    printf("网络错误: %s\n", esp_err_to_name(err));
    return 1;
  }
  if (r.status != 200) {
    printf("认领失败 (HTTP %d): %.*s\n", r.status, (int)r.body_len, r.body);
    app_api_free(&r);
    return 1;
  }

  cJSON *root = cJSON_ParseWithLength(r.body, r.body_len);
  app_api_free(&r);
  if (!root) {
    printf("响应解析失败\n");
    return 1;
  }
  const char *secret = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_secret"));
  if (!secret || strlen(secret) != 64) {
    printf("响应缺少 device_secret\n");
    cJSON_Delete(root);
    return 1;
  }
  if (app_identity_save_secret(secret) != ESP_OK) {
    printf("密钥写入 NVS 失败\n");
    cJSON_Delete(root);
    return 1;
  }
  cJSON *id = cJSON_GetObjectItem(root, "device_id");
  printf("认领成功 device_id=%s（密钥已写入 NVS，刷机后自动恢复）\n",
         id ? id->valuestring : "?");
  cJSON_Delete(root);

  do_sync_announcements();
  return 0;
}

/* 无凭证时的自动恢复：请求重新激活（管理员批准后轮询取回新密钥） */
static void try_reactivate(void) {
  char mac[13];
  app_identity_mac(mac);
  char body[48], path[96];
  app_api_resp_t r;

  int64_t now = esp_timer_get_time();
  if (now - s_last_react_post_us >= REACT_POST_INTERVAL_US) {
    s_last_react_post_us = now;
    snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", mac);
    if (app_api_anon_post("/api/devices/reactivate", body, &r) == ESP_OK) {
      ESP_LOGI(TAG, "reactivate request: %d %.*s", r.status, (int)r.body_len, r.body);
      app_api_free(&r);
    }
  }

  snprintf(path, sizeof(path), "/api/devices/reactivate?mac=%s", mac);
  if (app_api_anon_get(path, &r) != ESP_OK) return;
  cJSON *root = r.status == 200 ? cJSON_ParseWithLength(r.body, r.body_len) : NULL;
  app_api_free(&r);
  if (!root) return;

  const char *status = cJSON_GetStringValue(cJSON_GetObjectItem(root, "status"));
  const char *secret = cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_secret"));
  if (status && strcmp(status, "ok") == 0 && secret && strlen(secret) == 64) {
    if (app_identity_save_secret(secret) == ESP_OK) {
      ESP_LOGI(TAG, "重新激活成功，新密钥已保存");
    }
  } else {
    ESP_LOGI(TAG, "重新激活状态: %s（等待管理员批准）", status ? status : "?");
  }
  cJSON_Delete(root);
}

/* ---------- 公告同步 ---------- */

int do_sync_announcements(void) {
  if (!app_identity_has_secret()) {
    printf("尚未认领：先执行 pair <配对码>\n");
    return 1;
  }

  char path[96];
  snprintf(path, sizeof(path), "/api/announcements?since=%lld",
           (long long)app_store_get_cursor());

  app_api_resp_t r;
  esp_err_t err = app_api_signed_get(path, &r);
  if (err != ESP_OK) {
    printf("网络错误: %s\n", esp_err_to_name(err));
    return 1;
  }
  if (r.status == 401) {
    printf("鉴权失败（密钥可能已被服务端轮换），尝试重新激活流程…\n");
    app_store_clear_secret();
    app_api_free(&r);
    return 1;
  }
  if (r.status != 200) {
    printf("拉取失败 HTTP %d\n", r.status);
    app_api_free(&r);
    return 1;
  }

  cJSON *root = cJSON_ParseWithLength(r.body, r.body_len);
  app_api_free(&r);
  if (!root) return 1;

  cJSON *items = cJSON_GetObjectItem(root, "items");
  cJSON *cursor = cJSON_GetObjectItem(root, "cursor");
  size_t n = cJSON_GetArraySize(items);

  app_announcement_list_t list = {0};
  if (n > 0) {
    list.items = calloc(n, sizeof(app_announcement_t));
    list.count = n;
    size_t k = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
      app_announcement_t *a = &list.items[k++];
      a->id = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "id"));
      a->priority = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "priority"));
      a->created_at = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "created_at"));
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "title"));
      strlcpy(a->title, t ? t : "(无标题)", sizeof(a->title));
    }
  }

  long last_read = app_store_get_last_read();
  app_ui_show_announcements(&list, last_read);
  free(list.items);

  /* 控制台场景：展示即已读；屏幕 UI 接入后改为按键确认已读 */
  if (cJSON_IsNumber(cursor)) {
    app_store_set_cursor((int64_t)cursor->valuedouble);
    app_store_set_last_read((int64_t)cursor->valuedouble);
  }
  cJSON_Delete(root);
  return 0;
}

/* ---------- 任务 ---------- */

static void sync_task(void *arg) {
  (void)arg;
  app_net_wait_connected(portMAX_DELAY);
  for (;;) {
    if (!app_net_is_connected()) {
      app_net_wait_connected(portMAX_DELAY);
    }
    if (app_identity_has_secret()) {
      do_sync_announcements();
    } else {
      try_reactivate();
    }
    vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_MS));
  }
}

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(app_store_init());

  char device_id[APP_DEVICE_ID_LEN];
  app_identity_device_id(device_id);
  ESP_LOGI(TAG, "whisport M1 · device_id=%s · secret=%s", device_id,
           app_identity_has_secret() ? "present" : "absent");

  app_ui_register_actions(do_pair, do_sync_announcements);
  app_ui_init();

  ESP_ERROR_CHECK(app_net_init());
  xTaskCreate(sync_task, "sync", 8192, NULL, 5, NULL);
}
