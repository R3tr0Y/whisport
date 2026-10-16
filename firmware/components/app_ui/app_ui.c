#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "ui";

/* 由 main 注册的业务动作（避免 UI 组件反向依赖业务组件） */
static struct {
  int (*pair)(const char *code);
  int (*sync)(void);
} s_actions;

void app_ui_register_actions(int (*pair)(const char *), int (*sync)(void)) {
  s_actions.pair = pair;
  s_actions.sync = sync;
}

static int cmd_pair(int argc, char **argv) {
  if (argc != 2) {
    printf("用法: pair <6位配对码>\n");
    return 1;
  }
  return s_actions.pair ? s_actions.pair(argv[1]) : 1;
}

static int cmd_sync(int argc, char **argv) {
  (void)argc;
  (void)argv;
  return s_actions.sync ? s_actions.sync() : 1;
}

static int cmd_status(int argc, char **argv);

static esp_console_repl_t *s_repl;

void app_ui_init(void) {
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "whisport> ";
  repl_cfg.max_cmdline_length = 128;

  /* AI Passport 板卡为 USB-Serial-JTAG 控制台；若为 UART 控制台，改用
   * esp_console_new_repl_uart() 即可（见 components/console 的头文件说明）。 */
  esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &s_repl);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "usb-serial-jtag repl unavailable (%s), console UI disabled", esp_err_to_name(err));
    return;
  }

  esp_console_cmd_t pair_cmd = {
      .command = "pair",
      .help = "pair <6位配对码> — 认领设备并保存密钥",
      .hint = NULL,
      .func = cmd_pair,
  };
  esp_console_cmd_register(&pair_cmd);

  esp_console_cmd_t sync_cmd = {
      .command = "sync",
      .help = "立即拉取一次公告",
      .hint = NULL,
      .func = cmd_sync,
  };
  esp_console_cmd_register(&sync_cmd);

  esp_console_cmd_t status_cmd = {
      .command = "status",
      .help = "查看设备状态",
      .hint = NULL,
      .func = cmd_status,
  };
  esp_console_cmd_register(&status_cmd);

  esp_console_start_repl(s_repl);
}

void app_ui_show_announcements(const app_announcement_list_t *list, long last_read) {
  printf("\n==== 公告（%u 条）====\n", (unsigned)list->count);
  for (size_t i = 0; i < list->count; i++) {
    const app_announcement_t *a = &list->items[i];
    const char *flag = (a->id > last_read) ? "●" : " "; /* 未读红点 */
    const char *prio = a->priority >= 2 ? "[紧急]" : (a->priority == 1 ? "[重要]" : "");
    time_t t = (time_t)a->created_at;
    char tbuf[24];
    strftime(tbuf, sizeof(tbuf), "%m-%d %H:%M", localtime(&t));
    printf("%s #%ld %s %s %s\n", flag, a->id, tbuf, prio, a->title);
  }
  if (list->count == 0) printf("（无新公告）\n");
  printf("======================\n");
}

void app_ui_show_status(const char *device_id, bool has_secret, bool online) {
  printf("\n---- 设备状态 ----\n");
  printf("device_id : %s\n", device_id);
  printf("凭证      : %s\n", has_secret ? "已保存（L2 NVS）" : "缺失（待配对/重新激活）");
  printf("网络      : %s\n", online ? "在线" : "离线");
  printf("------------------\n");
}

static int cmd_status(int argc, char **argv) {
  (void)argc;
  (void)argv;
  extern void whisport_print_status(void);
  whisport_print_status();
  return 0;
}

void app_ui_dispatch_key(app_key_t key) {
  /* M1：长按=立即同步（占位）；M2 接 PTT 语义 */
  if (key == KEY_LONG && s_actions.sync) {
    s_actions.sync();
  }
}
