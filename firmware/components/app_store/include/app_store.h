/**
 * app_store — NVS 持久化（独立 storage 分区，规划 §3.3 L2 凭证落点）
 */
#ifndef APP_STORE_H
#define APP_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_SECRET_HEX_LEN 65  /* 32 字节 hex + '\0' */

esp_err_t app_store_init(void);

/* device_secret（64 hex 字符） */
esp_err_t app_store_set_secret(const char *secret_hex);
bool app_store_get_secret(char *buf, size_t buflen); /* buf >= APP_SECRET_HEX_LEN */
void app_store_clear_secret(void);

/* 公告游标（最大已见 announcement id） */
int64_t app_store_get_cursor(void);
void app_store_set_cursor(int64_t cursor);

/* 已读位置（未读红点用，规划 §4.3） */
int64_t app_store_get_last_read(void);
void app_store_set_last_read(int64_t id);

#endif /* APP_STORE_H */
