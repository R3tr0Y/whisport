/**
 * whisport 固件入口（M1：身份链路 + 公告板）
 *
 * 板级能力全部走官方 FoloToy AI-Passport BSP（components/bsp，经 EXTRA_COMPONENT_DIRS 引入）：
 *   bsp_i2c / bsp_display+LVGL / bsp_button / bsp_battery。
 * UI 为独立重设计（main/ui.c），不沿用官方 demo 测试菜单与 ui_pixel 外壳（官方 fork 规范）。
 *
 * 控制台命令（USB-Serial-JTAG）：pair <6位码> / sync / status
 * 按键（M1）：长按=立即同步；短按/双击=列表滚动（预留 PTT 语义见规划 §7.2）
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_api.h"
#include "app_identity.h"
#include "app_net.h"
#include "app_store.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "main";

#define SYNC_INTERVAL_MS (5 * 60 * 1000)
#define REACT_POST_INTERVAL_US (3600LL * 1000 * 1000)

static int64_t s_last_react_post_us = -REACT_POST_INTERVAL_US;
static QueueHandle_t s_keys;

/* 公告本地缓存（增量空响应不清屏；id 降序，最新在前） */
#define ANN_CACHE_MAX 6
static app_announcement_t s_items[ANN_CACHE_MAX];
static size_t s_count = 0;

static void cache_insert(const app_announcement_t *a) {
  for (size_t i = 0; i < s_count; i++) {
    if (s_items[i].id == a->id) {
      s_items[i] = *a;
      return;
    }
  }
  size_t pos = s_count;
  for (size_t i = 0; i < s_count; i++) {
    if (s_items[i].id < a->id) {
      pos = i;
      break;
    }
  }
  if (pos >= ANN_CACHE_MAX) return; /* 比现有 6 条都旧 */
  size_t end = s_count < ANN_CACHE_MAX ? s_count : ANN_CACHE_MAX - 1;
  memmove(&s_items[pos + 1], &s_items[pos], (end - pos) * sizeof(*a));
  s_items[pos] = *a;
  if (s_count < ANN_CACHE_MAX) s_count++;
}

static void render_announcements(void) {
  app_announcement_list_t view = { .items = s_items, .count = s_count };
  ui_show_announcements(&view, app_store_get_last_read());
}

typedef struct {
  bsp_btn_t btn;
  bsp_btn_ev_t ev;
} key_evt_t;

/* ---------- 状态展示 ---------- */

static void refresh_status(void) {
  char device_id[APP_DEVICE_ID_LEN];
  app_identity_device_id(device_id);
  ui_set_status(device_id, app_identity_has_secret(), app_net_is_connected());
}

void whisport_print_status(void) {
  char device_id[APP_DEVICE_ID_LEN];
  app_identity_device_id(device_id);
  printf("\n---- 设备状态 ----\n");
  printf("device_id : %s\n", device_id);
  printf("凭证      : %s\n", app_identity_has_secret() ? "已保存（L2 NVS）" : "缺失（待配对/重新激活）");
  printf("网络      : %s\n", app_net_is_connected() ? "在线" : "离线");
  printf("------------------\n");
  refresh_status();
}

/* ---------- 认领与重新激活 ---------- */

static int do_sync_announcements(void);

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

  refresh_status();
  return do_sync_announcements();
}

/* 无凭证时自动恢复：请求重新激活（管理员批准后轮询取回新密钥） */
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
      refresh_status();
    }
  } else {
    ESP_LOGI(TAG, "重新激活状态: %s（等待管理员批准）", status ? status : "?");
  }
  cJSON_Delete(root);
}

/* ---------- 公告同步 ---------- */

static int do_sync_announcements(void) {
  if (!app_identity_has_secret()) {
    printf("尚未认领：先执行 pair <配对码>\n");
    return 1;
  }
  if (!app_net_wait_time(pdMS_TO_TICKS(15000))) {
    printf("系统时钟未同步（SNTP 进行中），跳过本次同步\n");
    return 1;
  }

  ui_set_syncing(true);
  char path[96];
  /* newlib-nano 不支持 %lld（会打出字面量 "ld"）——统一用 %d */
  snprintf(path, sizeof(path), "/api/announcements?since=%d",
           (int)app_store_get_cursor());

  app_api_resp_t r;
  esp_err_t err = app_api_signed_get(path, &r);
  if (err != ESP_OK) {
    printf("网络错误: %s\n", esp_err_to_name(err));
    ui_set_syncing(false);
    return 1;
  }
  if (r.status == 401) {
    /* 分类处理：只有密钥真失效才清本地凭证，时钟问题保留密钥 */
    bool key_gone = false;
    cJSON *ej = cJSON_ParseWithLength(r.body, r.body_len);
    if (ej) {
      cJSON *err = cJSON_GetObjectItem(ej, "error");
      const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err, "code"));
      if (code && (strcmp(code, "bad_signature") == 0 || strcmp(code, "unknown_device") == 0)) {
        key_gone = true;
      } else if (code && strcmp(code, "stale_timestamp") == 0) {
        printf("签名时间戳超窗（时钟未同步），保留密钥\n");
      }
      cJSON_Delete(ej);
    }
    if (key_gone) {
      printf("密钥已失效（服务端轮换），转入重新激活流程…\n");
      app_store_clear_secret();
      refresh_status();
    }
    app_api_free(&r);
    ui_set_syncing(false);
    return 1;
  }
  if (r.status != 200) {
    printf("拉取失败 HTTP %d\n", r.status);
    app_api_free(&r);
    ui_set_syncing(false);
    return 1;
  }

  cJSON *root = cJSON_ParseWithLength(r.body, r.body_len);
  app_api_free(&r);
  if (!root) {
    ui_set_syncing(false);
    return 1;
  }

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
      strlcpy(a->title, t ? t : "(no title)", sizeof(a->title));
      const char *bd = cJSON_GetStringValue(cJSON_GetObjectItem(it, "body"));
      strlcpy(a->body, bd ? bd : "", sizeof(a->body));
    }
    printf("公告 ×%u（最新 #%ld）\n", (unsigned)n, list.items[n - 1].id);
  } else {
    printf("无新公告\n");
  }

  /* 并入本地缓存后渲染：增量空响应不清屏（每次只渲染缓存里的最新 6 条） */
  for (size_t i = 0; i < list.count; i++) cache_insert(&list.items[i]);
  free(list.items);
  render_announcements();

  /* 游标推进；已读标记由按键确认触发（见 input_task CLICK） */
  if (cJSON_IsNumber(cursor)) {
    app_store_set_cursor((int64_t)cursor->valuedouble);
  }
  cJSON_Delete(root);
  ui_set_syncing(false);
  return 0;
}

/* ---------- 按键（回调只入队，禁阻塞——官方 BSP 规则） ---------- */

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
  (void)user;
  key_evt_t e = { .btn = btn, .ev = ev };
  xQueueSend(s_keys, &e, 0);
}

static void input_task(void *arg) {
  (void)arg;
  key_evt_t e;
  for (;;) {
    if (xQueueReceive(s_keys, &e, portMAX_DELAY) != pdTRUE) continue;

    /* 详情视图内：仅 UP/DN 的 PRESS 滚动正文；OK 的短按/双击/长按=返回 */
    if (ui_detail_visible()) {
      if (e.btn != BSP_BTN_OK) {
        if (e.ev == BSP_BTN_PRESS) {
          ui_scroll_detail(e.btn == BSP_BTN_UP ? -1 : 1);
        }
        continue; /* UP/DN 的 CLICK/DOUBLE 等一律忽略，避免误退出 */
      }
      if (e.ev == BSP_BTN_CLICK || e.ev == BSP_BTN_DOUBLE || e.ev == BSP_BTN_LONG) {
        ui_hide_detail();
      }
      continue;
    }

    /* 列表视图 */
    switch (e.ev) {
      case BSP_BTN_PRESS:
        if (e.btn == BSP_BTN_UP) ui_move_selection(-1);
        else if (e.btn == BSP_BTN_DOWN) ui_move_selection(1);
        break;
      case BSP_BTN_CLICK: {
        /* 确认键：打开详情（并标记已读） */
        if (e.btn != BSP_BTN_OK) break;
        int sel = ui_selected_index();
        if (sel >= 0 && (size_t)sel < s_count) {
          ui_show_detail(&s_items[sel]);
          if (s_items[sel].id > app_store_get_last_read()) {
            app_store_set_last_read(s_items[sel].id);
            render_announcements();
          }
        }
        break;
      }
      case BSP_BTN_LONG:
        /* 长按 OK：立即同步（M2 在通话/频道内为 PTT 说话） */
        if (e.btn == BSP_BTN_OK) do_sync_announcements();
        break;
      default:
        break;
    }
  }
}

/* ---------- 周期同步 ---------- */

static void sync_task(void *arg) {
  (void)arg;
  app_net_wait_connected(portMAX_DELAY);
  refresh_status();
  for (;;) {
    if (!app_net_is_connected()) {
      app_net_wait_connected(portMAX_DELAY);
      refresh_status();
    }
    if (app_identity_has_secret()) {
      do_sync_announcements();
    } else {
      try_reactivate();
    }
    vTaskDelay(pdMS_TO_TICKS(SYNC_INTERVAL_MS));
  }
}

/* ---------- 控制台（bring-up 输入通道：六位配对码） ---------- */

static int cmd_pair(int argc, char **argv) {
  return (argc == 2) ? do_pair(argv[1]) : (printf("用法: pair <6位配对码>\n"), 1);
}

static int cmd_sync(int argc, char **argv) {
  (void)argc; (void)argv;
  return do_sync_announcements();
}

static int cmd_status(int argc, char **argv) {
  (void)argc; (void)argv;
  whisport_print_status();
  return 0;
}

static void console_init(void) {
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "whisport> ";
  repl_cfg.max_cmdline_length = 128;

  esp_console_dev_usb_serial_jtag_config_t hw =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  esp_console_repl_t *repl = NULL;
  if (esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl) != ESP_OK) {
    ESP_LOGW(TAG, "console unavailable; pair via NVS tools or M2 keypad UI");
    return;
  }

  esp_console_cmd_t pair_cmd = { .command = "pair", .help = "pair <6位配对码> — 认领设备并保存密钥", .func = cmd_pair };
  esp_console_cmd_register(&pair_cmd);
  esp_console_cmd_t sync_cmd = { .command = "sync", .help = "立即拉取公告", .func = cmd_sync };
  esp_console_cmd_register(&sync_cmd);
  esp_console_cmd_t status_cmd = { .command = "status", .help = "查看设备状态", .func = cmd_status };
  esp_console_cmd_register(&status_cmd);
  esp_console_start_repl(repl);
}

/* ---------- 启动（对齐官方 main.c 启动序） ---------- */

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

  ESP_ERROR_CHECK(bsp_i2c_init());
  ESP_ERROR_CHECK(bsp_display_init());
  if (bsp_lvgl_init() == NULL) {
    ESP_LOGE(TAG, "lvgl init failed");
  } else {
    ui_init();
    refresh_status();
    bsp_display_backlight(100); /* BSP 初始化背光为 0，必须显式点亮 */
  }
  ESP_ERROR_CHECK(bsp_battery_init());

  s_keys = xQueueCreate(8, sizeof(key_evt_t));
  ESP_ERROR_CHECK(bsp_button_init(on_key, NULL));
  xTaskCreate(input_task, "input", 4096, NULL, 5, NULL);

  console_init();
  ESP_ERROR_CHECK(app_net_init());
  xTaskCreate(sync_task, "sync", 8192, NULL, 5, NULL);
}
