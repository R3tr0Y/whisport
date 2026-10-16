/**
 * app_ui — 设备端 UI 抽象（规划 §7.1 ui）
 *
 * M1 骨架：esp_console 命令行 UI（pair/status/sync），用于 bring-up。
 * 接入 AI-Passport 官方 BSP（LVGL 240x320）后，将 show_* 换成屏幕渲染，
 * 并把按键事件（短按/双击/长按）接到 ui_dispatch_key()。
 */
#ifndef APP_UI_H
#define APP_UI_H

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
  long id;
  char title[128];
  int priority;
  long created_at;
} app_announcement_t;

typedef struct {
  app_announcement_t *items;
  size_t count;
  long max_id;       /* 本次响应游标 */
} app_announcement_list_t;

void app_ui_init(void);

void app_ui_show_announcements(const app_announcement_list_t *list, long last_read);

void app_ui_show_status(const char *device_id, bool has_secret, bool online);

/* 按键语义（规划 §7.2）：KEY_SHORT / KEY_DOUBLE / KEY_LONG */
typedef enum { KEY_SHORT, KEY_DOUBLE, KEY_LONG } app_key_t;
void app_ui_dispatch_key(app_key_t key);

#endif /* APP_UI_H */
