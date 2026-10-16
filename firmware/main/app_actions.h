/**
 * app_actions — main.c 的业务动作桥（供 net_prov 配网页 / UI 调用）
 * 所有动作内部持互斥锁，可从任意任务调用。
 */
#ifndef APP_ACTIONS_H
#define APP_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>

/* device_id（静态缓冲，32 hex） */
const char *app_device_id(void);

bool app_has_secret(void);
void app_refresh_status(void);

/* 认领设备（配对码 + 可选显示名）；返回 0 成功，非 0 为失败码（见实现注释） */
int app_claim(const char *code, const char *label);

/* 修改设备显示名（服务端 PATCH /devices/me）；返回 0 成功 */
int app_set_label(const char *label);

/* 立即拉取公告；返回 0 成功 */
int app_sync_announcements(void);

/* 云服务地址（配网页展示） */
const char *app_server_url(void);

/* 本地缓存的显示名（配网页展示）；无则返回 false */
bool app_get_label(char *buf, size_t len);

/* 打开配置台页面信息屏（AP 地址或局域网 IP） */
void app_show_setup_screen(void);

#endif /* APP_ACTIONS_H */
