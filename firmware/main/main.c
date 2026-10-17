/**
 * whisport 固件入口（M1.5：配网页 + 主菜单 + 公告板）
 *
 * 板级能力走官方 FoloToy AI-Passport BSP（bsp_*）；UI 为独立重设计（main/ui.c）。
 * 用户流程：开机 → 无 Wi-Fi 凭据则开热点配网（手机浏览器 192.168.4.1）→
 * 主菜单（公告板 / 对讲机-占位 / 设置 / 关于）。
 *
 * 按键：UP/DN 移动/滚动 · OK 短按 进入/打开 · OK 双击 返回 · OK 长按 同步
 * 控制台（开发用）：pair <码> / sync / status
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_actions.h"
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
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_prov.h"
#include "nvs_flash.h"
#include "ui.h"

static const char *TAG = "main";

#define SYNC_INTERVAL_MS (5 * 60 * 1000)
#define REACT_POST_INTERVAL_US (3600LL * 1000 * 1000)
#define PROV_AP_GRACE_MS (120 * 1000)        /* 联网后保留热点供用户完成配置的时长 */
#define OFFLINE_AP_FALLBACK_US (180LL * 1000 * 1000) /* 离线超过 3 分钟自动开配网热点 */

/* ---------------- 公告本地缓存 ---------------- */

#define ANN_CACHE_MAX 6
static app_announcement_t s_items[ANN_CACHE_MAX];
static size_t s_count = 0;
static SemaphoreHandle_t s_act_mutex; /* 业务动作互斥（httpd/按键/同步任务） */

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
  if (pos >= ANN_CACHE_MAX) return;
  size_t end = s_count < ANN_CACHE_MAX ? s_count : ANN_CACHE_MAX - 1;
  memmove(&s_items[pos + 1], &s_items[pos], (end - pos) * sizeof(*a));
  s_items[pos] = *a;
  if (s_count < ANN_CACHE_MAX) s_count++;
}

static int unread_count(void) {
  long lr = (long)app_store_get_last_read();
  int n = 0;
  for (size_t i = 0; i < s_count; i++) {
    if (s_items[i].id > lr) n++;
  }
  return n;
}

static void render_announcements(void) {
  /* S4：过期公告在渲染前就地剔除（缓存只增不删会留下已过期/已删除卡片） */
  long now = (long)time(NULL);
  size_t w = 0;
  for (size_t i = 0; i < s_count; i++) {
    if (s_items[i].expires_at && s_items[i].expires_at < now) continue;
    if (w != i) s_items[w] = s_items[i];
    w++;
  }
  s_count = w;

  app_announcement_list_t view = { .items = s_items, .count = s_count };
  ui_show_announcements(&view, app_store_get_last_read());
}

/* ---------------- 主菜单 ---------------- */

enum { MENU_ANN = 0, MENU_INTERCOM, MENU_SETUP, MENU_ABOUT, MENU_COUNT };
static int s_menu_sel = 0;
static char s_menu_labels[MENU_COUNT][48];

static void show_menu(void) {
  int unread = unread_count();
  if (unread > 0) {
    snprintf(s_menu_labels[MENU_ANN], sizeof(s_menu_labels[0]), "Announcements (%d)", unread);
  } else {
    snprintf(s_menu_labels[MENU_ANN], sizeof(s_menu_labels[0]), "Announcements");
  }
  snprintf(s_menu_labels[MENU_INTERCOM], sizeof(s_menu_labels[0]), "Intercom");
  snprintf(s_menu_labels[MENU_SETUP], sizeof(s_menu_labels[0]), "Device Setup");
  snprintf(s_menu_labels[MENU_ABOUT], sizeof(s_menu_labels[0]), "About");

  const char *items[MENU_COUNT];
  for (int i = 0; i < MENU_COUNT; i++) items[i] = s_menu_labels[i];
  ui_show_menu(items, MENU_COUNT, s_menu_sel, unread);
}

static void show_setup_screen(void) {
  char host[32] = "-";
  net_prov_setup_host(host, sizeof(host));
  static char l1[48], l2[48], l3[48], l4[48];
  if (net_prov_ap_mode()) {
    snprintf(l1, sizeof(l1), "Wi-Fi: %s", app_net_ap_ssid());
    snprintf(l2, sizeof(l2), "PASS:  %s", app_net_ap_pass());
    snprintf(l3, sizeof(l3), "Open http://%s", host);
    snprintf(l4, sizeof(l4), "PIN:   %s", net_prov_pin());
    ui_show_info("Device Setup", l1, l2, l3, l4);
  } else {
    snprintf(l1, sizeof(l1), "Open http://%s", host);
    snprintf(l2, sizeof(l2), "PIN: %s (to change)", net_prov_pin());
    ui_show_info("Device Setup", l1, l2, "Wi-Fi / name / pairing", NULL);
  }
}

static void show_about(void) {
  static char l1[48], l2[80];
  snprintf(l1, sizeof(l1), "id %s", app_device_id());
  snprintf(l2, sizeof(l2), "%s", CONFIG_WS_SERVER_URL);
  ui_show_info("About", l1, l2, "whisport M1.5 · IDF 5.5.3", NULL);
}

/* ---------------- 动作桥（app_actions.h 实现） ---------------- */

const char *app_device_id(void) {
  static char s_id[APP_DEVICE_ID_LEN];
  if (s_id[0] == '\0') app_identity_device_id(s_id);
  return s_id;
}

bool app_has_secret(void) {
  return app_identity_has_secret();
}

bool app_get_label(char *buf, size_t len) {
  return app_store_get_label(buf, len);
}

const char *app_server_url(void) {
  return CONFIG_WS_SERVER_URL;
}

/* 离线回退开关（sync_task 持有；重配后重新武装，凭据错误可再次回退） */
static bool s_fallback_armed = true;

void app_wifi_apply(const char *ssid, const char *pass) {
  if (net_prov_ap_mode()) {
    /* 热点信道与目标路由不一致时 APSTA 无法关联：换配置必须先关热点 */
    app_net_stop_ap();
    net_prov_set_ap_mode(false);
    s_fallback_armed = true; /* 若新凭据仍连不上，3 分钟后可再次回退 */
  }
  app_net_start_sta_creds(ssid, pass);
}

void app_refresh_status(void) {
  ui_set_status(app_device_id(), app_identity_has_secret(), app_net_is_connected());
}

void app_show_setup_screen(void) {
  show_setup_screen();
}

int app_claim(const char *code, const char *label) {
  xSemaphoreTakeRecursive(s_act_mutex, portMAX_DELAY);
  char mac[13];
  app_identity_mac(mac);
  char body[192];
  snprintf(body, sizeof(body),
           "{\"pairing_code\":\"%s\",\"mac\":\"%s\",\"label\":\"%s\"}", code, mac,
           label ? label : "");

  app_api_resp_t r;
  esp_err_t err = app_api_anon_post("/api/devices/claim", body, &r);
  if (err != ESP_OK) {
    xSemaphoreGiveRecursive(s_act_mutex);
    return 1;
  }
  if (r.status != 200) {
    ESP_LOGW(TAG, "claim http %d", r.status);
    app_api_free(&r);
    xSemaphoreGiveRecursive(s_act_mutex);
    return 2;
  }
  cJSON *root = cJSON_ParseWithLength(r.body, r.body_len);
  app_api_free(&r);
  const char *secret = root ? cJSON_GetStringValue(cJSON_GetObjectItem(root, "device_secret")) : NULL;
  int rc = 0;
  if (!secret || strlen(secret) != 64 || app_identity_save_secret(secret) != ESP_OK) {
    rc = 3;
  } else if (label && label[0]) {
    app_store_set_label(label);
  }
  if (root) cJSON_Delete(root);
  ESP_LOGI(TAG, "claim %s", rc == 0 ? "ok" : "failed");
  app_refresh_status();
  xSemaphoreGiveRecursive(s_act_mutex);
  if (rc == 0) app_sync_announcements();
  return rc;
}

int app_set_label(const char *label) {
  if (!app_identity_has_secret()) return 1;
  xSemaphoreTakeRecursive(s_act_mutex, portMAX_DELAY);
  char body[96];
  snprintf(body, sizeof(body), "{\"label\":\"%s\"}", label);
  app_api_resp_t r;
  esp_err_t err = app_api_signed_patch("/api/devices/me", body, &r);
  int rc = (err == ESP_OK && r.status == 200) ? 0 : 2;
  if (err == ESP_OK) app_api_free(&r);
  if (rc == 0) app_store_set_label(label);
  ESP_LOGI(TAG, "set label: %d", rc);
  xSemaphoreGiveRecursive(s_act_mutex);
  return rc;
}

/** 通用抓取：path 为完整查询串；cursor_monotonic=true 时游标只增不减（recent 种缓存用）。
 *  S6：不加全局锁——缓存单写者（同步任务/按键事务）、NVS 线程安全 */
static int fetch_announcements(const char *path, bool cursor_monotonic) {
  if (!app_identity_has_secret()) {
    return 1;
  }
  if (!app_net_wait_time(pdMS_TO_TICKS(15000))) {
    ESP_LOGW(TAG, "time not synced, skip sync");
    return 2;
  }

  ui_set_syncing(true);
  app_api_resp_t r;
  esp_err_t err = app_api_signed_get(path, &r);
  if (err != ESP_OK) {
    ui_set_syncing(false);
    return 3;
  }
  if (r.status == 401) {
    bool key_gone = false;
    cJSON *ej = cJSON_ParseWithLength(r.body, r.body_len);
    if (ej) {
      cJSON *err_obj = cJSON_GetObjectItem(ej, "error");
      const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(err_obj, "code"));
      if (code && (strcmp(code, "bad_signature") == 0 || strcmp(code, "unknown_device") == 0)) {
        key_gone = true;
      }
      cJSON_Delete(ej);
    }
    if (key_gone) {
      ESP_LOGW(TAG, "key invalid -> reactivation");
      app_store_clear_secret();
      app_refresh_status();
    }
    app_api_free(&r);
    ui_set_syncing(false);
    return 4;
  }
  if (r.status != 200) {
    app_api_free(&r);
    ui_set_syncing(false);
    return 5;
  }

  cJSON *root = cJSON_ParseWithLength(r.body, r.body_len);
  app_api_free(&r);
  if (!root) {
    ui_set_syncing(false);
    return 6;
  }

  cJSON *items = cJSON_GetObjectItem(root, "items");
  cJSON *cursor = cJSON_GetObjectItem(root, "cursor");
  size_t n = cJSON_GetArraySize(items);
  if (n > 0) {
    app_announcement_list_t list = {0};
    list.items = calloc(n, sizeof(app_announcement_t));
    list.count = n;
    size_t k = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, items) {
      if (k >= n) break;
      app_announcement_t *a = &list.items[k++];
      a->id = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "id"));
      a->priority = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "priority"));
      a->created_at = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "created_at"));
      const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "title"));
      strlcpy(a->title, t ? t : "(no title)", sizeof(a->title));
      const char *bd = cJSON_GetStringValue(cJSON_GetObjectItem(it, "body"));
      strlcpy(a->body, bd ? bd : "", sizeof(a->body));
      a->expires_at = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(it, "expires_at"));
      cache_insert(a);
    }
    free(list.items);
    ESP_LOGI(TAG, "synced %u new announcement(s)", (unsigned)n);
  }
  render_announcements();

  if (cJSON_IsNumber(cursor)) {
    int64_t c = (int64_t)cursor->valuedouble;
    if (cursor_monotonic) {
      if (c > app_store_get_cursor()) app_store_set_cursor(c); /* recent：游标只增不减 */
    } else {
      app_store_set_cursor(c);
    }
  }
  cJSON_Delete(root);
  ui_set_syncing(false);
  return 0;
}

int app_sync_announcements(void) {
  char path[96];
  snprintf(path, sizeof(path), "/api/announcements?since=%d", (int)app_store_get_cursor());
  return fetch_announcements(path, false);
}

/** 开机种缓存：拉最近 6 条（刷机/重启后 RAM 缓存为空、游标已越过旧公告的场景） */
static int seed_recent_announcements(void) {
  return fetch_announcements("/api/announcements?recent=6", true);
}

/* ---------------- 重新激活（无凭证自动恢复） ---------------- */

static int64_t s_last_react_post_us = -REACT_POST_INTERVAL_US;
static TaskHandle_t s_sync_task = NULL; /* 同步任务句柄：按键只发通知，不阻塞输入任务 */

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
      ESP_LOGI(TAG, "reactivate request: %d", r.status);
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
      ESP_LOGI(TAG, "reactivation ok, new key saved");
      app_refresh_status();
    }
  } else {
    ESP_LOGI(TAG, "reactivate status: %s", status ? status : "?");
  }
  cJSON_Delete(root);
}

/* ---------------- 按键 ---------------- */

static QueueHandle_t s_keys;

typedef struct {
  bsp_btn_t btn;
  bsp_btn_ev_t ev;
} key_evt_t;

/* 背光与屏保的空闲节奏 */
#define BL_DIM_AFTER_US (30LL * 1000 * 1000)   /* 30s 调暗 */
#define SAVER_AFTER_US (60LL * 1000 * 1000)    /* 60s 进屏保（像素风开机画面） */
#define BL_OFF_AFTER_US (150LL * 1000 * 1000)  /* 150s 熄屏 */
#define WAKE_SWALLOW_US (700LL * 1000)         /* 唤醒/退屏保后抑制窗：吞掉同一次按压的后续事件 */
static int64_t s_last_activity_us = 0;
static int64_t s_wake_until_us = 0;
static int s_bl = 100;

static void bl_set(int pct) {
  if (pct != s_bl) {
    bsp_display_backlight(pct);
    s_bl = pct;
  }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
  (void)user;
  key_evt_t e = { .btn = btn, .ev = ev };
  xQueueSend(s_keys, &e, 0); /* 回调禁阻塞：仅入队 */
}

static void back_to_menu(void) {
  show_menu();
}

static void input_task(void *arg) {
  (void)arg;
  s_last_activity_us = esp_timer_get_time();
  key_evt_t e;
  for (;;) {
    if (xQueueReceive(s_keys, &e, pdMS_TO_TICKS(5000)) != pdTRUE) {
      int64_t idle = esp_timer_get_time() - s_last_activity_us;
      if (idle > BL_OFF_AFTER_US) {
        bl_set(0);
      } else if (idle > SAVER_AFTER_US) {
        if (!ui_splash_visible()) {
          ui_show_splash();
          bl_set(40); /* 屏保常亮降低亮度 */
        }
      } else if (idle > BL_DIM_AFTER_US) {
        if (!ui_splash_visible()) bl_set(20);
      }
      continue;
    }
    /* 熄屏/屏保/调暗的按键语义：
     *  - 熄屏时按任意键 → 点亮并进入屏保（不执行按键），再次按键才退出屏保
     *  - 屏保中按任意键 → 退出屏保回到原界面（不执行按键）
     *  - 仅调暗（未熄屏/无屏保）→ 恢复亮度，该按键照常生效
     * 一次物理按压会依次产生 PRESS→CLICK→RELEASE，唤醒类按键必须用抑制窗吞掉全部后续事件 */
    {
      int64_t now = esp_timer_get_time();
      bool from_off = (s_bl == 0);
      bool from_splash = ui_splash_visible();
      if (from_off || from_splash || s_bl != 100) {
        bl_set(100);
        s_last_activity_us = now;
        if (from_off) {
          ui_show_splash(); /* 熄屏唤醒 → 屏保界面 */
          s_wake_until_us = now + WAKE_SWALLOW_US;
          continue;
        }
        if (from_splash) {
          ui_hide_splash(); /* 屏保中按键 → 退出屏保 */
          s_wake_until_us = now + WAKE_SWALLOW_US;
          continue;
        }
      }
      if (now < s_wake_until_us) {
        s_last_activity_us = now;
        continue; /* 唤醒/退屏保同一按压的尾随事件：吞掉 */
      }
      s_last_activity_us = now;
    }

    ui_screen_t scr = ui_current_screen();

    if (scr == UI_SCR_DETAIL) {
      if (e.btn != BSP_BTN_OK) {
        if (e.ev == BSP_BTN_PRESS) ui_scroll_detail(e.btn == BSP_BTN_UP ? -1 : 1);
        continue;
      }
      if (e.ev == BSP_BTN_CLICK || e.ev == BSP_BTN_DOUBLE || e.ev == BSP_BTN_LONG) {
        ui_hide_detail();
      }
      continue;
    }

    if (scr == UI_SCR_INFO) {
      if (e.btn == BSP_BTN_OK && (e.ev == BSP_BTN_CLICK || e.ev == BSP_BTN_DOUBLE)) {
        back_to_menu();
      }
      continue;
    }

    if (scr == UI_SCR_MENU) {
      switch (e.ev) {
        case BSP_BTN_PRESS:
          if (e.btn == BSP_BTN_UP && s_menu_sel > 0) {
            s_menu_sel--;
            ui_menu_highlight(s_menu_sel);
          } else if (e.btn == BSP_BTN_DOWN && s_menu_sel < MENU_COUNT - 1) {
            s_menu_sel++;
            ui_menu_highlight(s_menu_sel);
          }
          break;
        case BSP_BTN_CLICK:
          if (e.btn != BSP_BTN_OK) break;
          switch (s_menu_sel) {
            case MENU_ANN:
              render_announcements();
              ui_show_ann_list();
              /* 缓存为空（离线启动过）：通知同步任务后台补拉，输入任务不阻塞 */
              if (s_count == 0 && s_sync_task) xTaskNotifyGive(s_sync_task);
              break;
            case MENU_INTERCOM:
              ui_show_intercom();
              break;
            case MENU_SETUP:
              show_setup_screen();
              break;
            case MENU_ABOUT:
              show_about();
              break;
          }
          break;
        case BSP_BTN_LONG:
          if (e.btn == BSP_BTN_OK) app_sync_announcements();
          break;
        default:
          break;
      }
      continue;
    }

    /* 公告列表 */
    switch (e.ev) {
      case BSP_BTN_PRESS:
        if (e.btn == BSP_BTN_UP) ui_move_selection(-1);
        else if (e.btn == BSP_BTN_DOWN) ui_move_selection(1);
        break;
      case BSP_BTN_CLICK: {
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
      case BSP_BTN_DOUBLE:
        back_to_menu();
        break;
      case BSP_BTN_LONG:
        /* 长按 OK：请求立即同步（后台执行，输入任务不阻塞；M2 通话中为 PTT） */
        if (e.btn == BSP_BTN_OK && s_sync_task) xTaskNotifyGive(s_sync_task);
        break;
      default:
        break;
    }
  }
}

/* ---------------- 周期任务 ---------------- */

static void sync_task(void *arg) {
  (void)arg;
  s_sync_task = xTaskGetCurrentTaskHandle();
  bool ap_started = net_prov_ap_mode(); /* 是否由我们启动的热点（含离线回退） */
  int64_t connected_since_us = 0;
  int64_t down_since_us = 0;

  for (;;) {
    if (app_net_is_connected()) {
      if (!connected_since_us) {
        connected_since_us = esp_timer_get_time();
        app_refresh_status();
        /* 配网/回退后联网成功：立即进入主菜单（设置页仍可在局域网 http://IP 访问） */
        if (ap_started && ui_current_screen() == UI_SCR_INFO) back_to_menu();
      }
      down_since_us = 0;
    } else {
      connected_since_us = 0;
      if (!down_since_us) down_since_us = esp_timer_get_time();
      /* 离线兜底：超过 3 分钟连不上 → 自动开配网热点并显示设置屏。
       * 覆盖路由器改密码/搬家等场景（否则局域网页面不可达且无串口，用户无从重配） */
      if (s_fallback_armed && !net_prov_ap_mode() &&
          esp_timer_get_time() - down_since_us > OFFLINE_AP_FALLBACK_US) {
        ESP_LOGW(TAG, "offline >180s -> provisioning AP fallback");
        net_prov_set_ap_mode(true);
        app_net_start_ap(app_net_ap_ssid(), app_net_ap_pass());
        ap_started = true;
        s_fallback_armed = false;
        app_show_setup_screen();
      }
    }
    if (!app_net_is_connected()) {
      /* 离线：不做网络动作（DNS/HTTPS 必失败），10s 后再查；只维护离线回退逻辑 */
    } else if (app_identity_has_secret()) {
      if (s_count == 0) seed_recent_announcements(); /* 开机/刷机后种入最近公告 */
      else app_sync_announcements();
    } else {
      try_reactivate();
    }
    /* 联网后 2 分钟关闭热点（配网完成或离线回退已无需要） */
    if (ap_started && connected_since_us &&
        esp_timer_get_time() - connected_since_us > PROV_AP_GRACE_MS * 1000LL) {
      app_net_stop_ap();
      net_prov_set_ap_mode(false);
      ap_started = false;
      s_fallback_armed = true;
      ESP_LOGI(TAG, "provisioning ap stopped");
      if (ui_current_screen() == UI_SCR_INFO) back_to_menu();
    }
    /* I7：±60s 抖动（惊群）；离线 10s 一轮驱动回退检查；已联网但缓存为空时 3s 快速重试 */
    uint32_t tmo;
    if (!app_net_is_connected()) {
      tmo = 10000;
    } else if (s_count == 0 && app_identity_has_secret()) {
      tmo = 3000; /* 开机种子慢（节拍+SNTP 等待）会拖长空列表窗口 */
    } else {
      tmo = SYNC_INTERVAL_MS + (esp_random() % 60000);
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tmo));
  }
}

/* ---------------- 控制台（开发用） ---------------- */

static int cmd_pair(int argc, char **argv) {
  if (argc != 2) {
    printf("用法: pair <6位配对码>\n");
    return 1;
  }
  int rc = app_claim(argv[1], NULL);
  printf(rc == 0 ? "认领成功\n" : "认领失败 (%d)\n", rc);
  return rc;
}

static int cmd_sync(int argc, char **argv) {
  (void)argc; (void)argv;
  int rc = app_sync_announcements();
  printf(rc == 0 ? "同步完成\n" : "同步失败 (%d)\n", rc);
  return rc;
}

static int cmd_status(int argc, char **argv) {
  (void)argc; (void)argv;
  char ip[20] = "-";
  app_net_get_ip(ip, sizeof(ip));
  printf("\n---- 设备状态 ----\n");
  printf("device_id : %s\n", app_device_id());
  printf("凭证      : %s\n", app_identity_has_secret() ? "已保存（L2 NVS）" : "缺失（待配对/重新激活）");
  printf("网络      : %s (%s)\n", app_net_is_connected() ? "在线" : "离线", ip);
  printf("配网      : %s\n", net_prov_ap_mode() ? "AP 模式" : "STA 模式");
  printf("heap 空闲 : %u bytes\n", (unsigned)esp_get_free_heap_size());
  printf("------------------\n");
  app_refresh_status();
  return 0;
}

/* 清 Wi-Fi 凭据并重启 → 下次开机进入 AP 配网（救急/重配用） */
static int cmd_prov(int argc, char **argv) {
  (void)argc; (void)argv;
  app_store_clear_wifi();
  printf("Wi-Fi 凭据已清除，重启后进入配网模式（whisport-XXXX 热点）\n");
  vTaskDelay(pdMS_TO_TICKS(300));
  esp_restart();
  return 0;
}

/* 开发/测试用：打印当前配网 PIN */
static int cmd_pin(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("配网 PIN: %s\n", net_prov_pin());
  return 0;
}

/* 诊断：LVGL 内存池 + 系统堆 */
static int cmd_lvmem(int argc, char **argv) {
  (void)argc; (void)argv;
  char buf[160];
  ui_mem_info(buf, sizeof(buf));
  printf("%s\n", buf);
  printf("heap 空闲: %u, 最小历史: %u\n", (unsigned)esp_get_free_heap_size(),
         (unsigned)esp_get_minimum_free_heap_size());
  return 0;
}

/* 诊断：LVGL 内实际文本导出（缺字排查：字符串损坏 vs 渲染缺字） */
static int cmd_uitree(int argc, char **argv) {
  (void)argc; (void)argv;
  static char buf[768];
  ui_debug_dump(buf, sizeof(buf));
  printf("%s", buf);
  return 0;
}

/* 开发/测试用：注入按键事件（免手动）
 * 用法：key <up|down|ok> [press|click|double|long]  或  key <press|click|double|long>（等价 OK + 该事件） */
static int cmd_key(int argc, char **argv) {
  if (argc < 2) {
    printf("用法: key <up|down|ok> [press|click|double|long] | key <press|click|double|long>\n");
    return 1;
  }
  static const struct { const char *n; bsp_btn_t v; } btns[] = {
      { "up", BSP_BTN_UP }, { "down", BSP_BTN_DOWN }, { "ok", BSP_BTN_OK } };
  static const struct { const char *n; bsp_btn_ev_t v; } evs[] = {
      { "press", BSP_BTN_PRESS }, { "click", BSP_BTN_CLICK },
      { "double", BSP_BTN_DOUBLE }, { "long", BSP_BTN_LONG } };

  bsp_btn_t btn = BSP_BTN_OK;
  bool btn_set = false;
  for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
    if (strcmp(argv[1], btns[i].n) == 0) { btn = btns[i].v; btn_set = true; break; }
  }
  bsp_btn_ev_t ev = BSP_BTN_CLICK;
  const char *ev_arg = btn_set ? (argc > 2 ? argv[2] : NULL) : argv[1];
  if (ev_arg) {
    for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
      if (strcmp(ev_arg, evs[i].n) == 0) { ev = evs[i].v; break; }
    }
  }
  key_evt_t e = { .btn = btn, .ev = ev };
  if (xQueueSend(s_keys, &e, pdMS_TO_TICKS(100)) != pdTRUE) {
    printf("key queue full\n");
    return 1;
  }
  printf("key injected: btn=%s ev=%s\n", btn_set ? argv[1] : "ok", ev_arg ? ev_arg : "click");
  return 0;
}

/* 诊断：公告本地缓存 */
static int cmd_cache(int argc, char **argv) {
  (void)argc; (void)argv;
  printf("cache: %u items, last_read=%d cursor=%d\n", (unsigned)s_count,
         (int)app_store_get_last_read(), (int)app_store_get_cursor());
  for (size_t i = 0; i < s_count; i++) {
    printf("  [%u] id=%ld %s\n", (unsigned)i, s_items[i].id, s_items[i].title);
  }
  return 0;
}

/* 开发/测试用：直接设置 Wi-Fi 凭据（正式用户走配网页） */
static int cmd_wifi(int argc, char **argv) {
  if (argc < 2) {
    printf("用法: wifi <ssid> [password]\n");
    return 1;
  }
  app_wifi_apply(argv[1], argc > 2 ? argv[2] : "");
  printf("已保存，正在连接\n");
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
    ESP_LOGW(TAG, "console unavailable");
    return;
  }
  esp_console_cmd_t cmds[] = {
      { .command = "pair", .help = "pair <6位配对码> — 认领设备", .func = cmd_pair },
      { .command = "sync", .help = "立即拉取公告", .func = cmd_sync },
      { .command = "status", .help = "查看设备状态", .func = cmd_status },
      { .command = "prov", .help = "清除 Wi-Fi 凭据并重启（进入配网模式）", .func = cmd_prov },
      { .command = "wifi", .help = "wifi <ssid> [密码] — 直接设置 Wi-Fi（开发/恢复用）", .func = cmd_wifi },
      { .command = "pin", .help = "打印当前配网 PIN", .func = cmd_pin },
      { .command = "lvmem", .help = "LVGL 内存池/堆诊断", .func = cmd_lvmem },
      { .command = "key", .help = "key <up|down|ok> [press|click|double|long] — 注入按键", .func = cmd_key },
      { .command = "uitree", .help = "导出 LVGL 当前文本（缺字排查）", .func = cmd_uitree },
      { .command = "cache", .help = "查看公告本地缓存", .func = cmd_cache },
  };
  for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    esp_console_cmd_register(&cmds[i]);
  }
  esp_console_start_repl(repl);
}

/* ---------------- 启动 ---------------- */

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }
  ESP_ERROR_CHECK(app_store_init());
  s_act_mutex = xSemaphoreCreateRecursiveMutex();

  ESP_LOGI(TAG, "whisport M1.5 · device_id=%s · secret=%s", app_device_id(),
           app_identity_has_secret() ? "present" : "absent");

  ESP_ERROR_CHECK(bsp_i2c_init());
  ESP_ERROR_CHECK(bsp_display_init());
  if (bsp_lvgl_init() == NULL) {
    ESP_LOGE(TAG, "lvgl init failed");
  } else {
    ui_init();
    app_refresh_status();
    bsp_display_backlight(100); /* BSP 初始化背光为 0，必须显式点亮 */
    ui_show_splash();           /* 开机：像素风启动画面 */
    vTaskDelay(pdMS_TO_TICKS(2500));
  }
  ESP_ERROR_CHECK(bsp_battery_init());

  s_keys = xQueueCreate(8, sizeof(key_evt_t));
  ESP_ERROR_CHECK(bsp_button_init(on_key, NULL));
  xTaskCreate(input_task, "input", 4096, NULL, 5, NULL);
  console_init();

  ESP_ERROR_CHECK(app_net_init());
  ESP_ERROR_CHECK(net_prov_init()); /* 先于界面分支：PIN 需先生成（设置屏会显示） */

  if (app_net_has_creds()) {
    show_menu();
    app_net_start_sta();
  } else {
    /* 首次开机：AP 配网模式 */
    net_prov_set_ap_mode(true);
    app_net_start_ap(app_net_ap_ssid(), app_net_ap_pass());
    show_setup_screen();
    ESP_LOGI(TAG, "no wifi creds -> provisioning ap %s", app_net_ap_ssid());
  }
  xTaskCreate(sync_task, "sync", 8192, NULL, 5, NULL);
}
