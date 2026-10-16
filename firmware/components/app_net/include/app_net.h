/**
 * app_net — Wi-Fi STA 连接管理（规划 §7.1 net_mgr）
 * M1：Kconfig 静态配置 SSID/密码；断线由 IDF 自动重连 + 事件回调记录。
 */
#ifndef APP_NET_H
#define APP_NET_H

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t app_net_init(void);

/* 阻塞等待拿到 IP（含重连），timeout 按 FreeRTOS 惯例 */
esp_err_t app_net_wait_connected(TickType_t timeout);

/* 阻塞等待 SNTP 对时完成（HMAC 签名时间戳依赖系统时钟） */
bool app_net_wait_time(TickType_t timeout);

bool app_net_is_connected(void);

#endif /* APP_NET_H */
