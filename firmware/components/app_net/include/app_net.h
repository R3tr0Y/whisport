/**
 * app_net — Wi-Fi 驱动层（规划 §7.1 net_mgr）
 *
 * 凭据来自 NVS（配网页写入）；Kconfig 的 SSID/密码仅作为开发种子
 * （NVS 为空且 Kconfig 非空时首次写入）。
 * AP/STA 编排与配网 httpd 见 main/net_prov.c。
 */
#ifndef APP_NET_H
#define APP_NET_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define APP_NET_SSID_MAX 33
#define APP_NET_PASS_MAX 65
#define APP_NET_AP_SSID_MAX 20

/* 初始化 wifi 驱动（不连接） */
esp_err_t app_net_init(void);

/* 用 NVS 凭据启动 STA；无凭据返回 ESP_ERR_NOT_FOUND */
esp_err_t app_net_start_sta(void);

/* 保存凭据并（重）连 STA —— 配网页调用；成功后 NVS 持久化 */
esp_err_t app_net_start_sta_creds(const char *ssid, const char *pass);

/* AP 配网热点（APSTA 模式，STA 保持可用以便扫描） */
esp_err_t app_net_start_ap(const char *ssid, const char *pass);
void app_net_stop_ap(void);

bool app_net_has_creds(void);

/* 阻塞等待拿到 IP（含重连），timeout 按 FreeRTOS 惯例 */
esp_err_t app_net_wait_connected(TickType_t timeout);

/* 阻塞等待 SNTP 对时完成（HMAC 签名时间戳依赖系统时钟） */
bool app_net_wait_time(TickType_t timeout);

bool app_net_is_connected(void);

/* 当前 STA IP（点分十进制；未连接返回 false） */
bool app_net_get_ip(char *buf, size_t len);

/* 扫描附近 AP（APSTA 下可用）；返回结果写入调用方缓冲 */
typedef struct {
  char ssid[APP_NET_SSID_MAX];
  int8_t rssi;
  bool secure;
} app_net_ap_t;

int app_net_scan(app_net_ap_t *out, int max);

/* AP 名称/密码（由 device_id 派生，界面展示给用户） */
const char *app_net_ap_ssid(void);
const char *app_net_ap_pass(void);

#endif /* APP_NET_H */
