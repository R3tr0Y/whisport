/**
 * net_prov — 设备配网/配置台（SoftAP + HTTP，规划 §7.1 net_mgr 用户侧）
 *
 * 无凭据时开热点 whisport-XXXX（密码=device_id 前 8 位，屏幕显示），
 * 手机浏览器访问 http://192.168.4.1：扫描 Wi-Fi、保存密码、改显示名、输入配对码认领。
 * 已联网时同一页面在局域网 http://<设备IP> 也可访问（改 Wi-Fi/改名）。
 */
#ifndef NET_PROV_H
#define NET_PROV_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* 启动配置台（HTTP 服务；AP 模式由调用方先经 app_net_start_ap 开启） */
esp_err_t net_prov_init(void);

/* 是否处于 AP 配网模式 */
bool net_prov_ap_mode(void);
void net_prov_set_ap_mode(bool ap);

/* 页面访问地址（"192.168.4.1" 或 STA IP）；返回 false 表示暂不可用 */
bool net_prov_setup_host(char *buf, size_t len);

/* 屏幕展示的 4 位管理 PIN（所有写操作需携带，防广播域/局域网内他人改配置） */
const char *net_prov_pin(void);

#endif /* NET_PROV_H */
