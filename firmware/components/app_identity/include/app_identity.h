/**
 * app_identity — 设备身份与签名（规划 §3 / §8）
 *
 * device_id = SHA-256(规范化 MAC)[:16] hex —— 与服务端 worker/src/auth.ts 严格一致。
 * 签名规范化串：
 *   REST: "v1\n{device_id}\n{ts}\n{nonce}\n{METHOD}\n{path}"
 *   WSS : "v1\n{device_id}\n{ts}\n{nonce}\nws"     （M2 使用）
 */
#ifndef APP_IDENTITY_H
#define APP_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define APP_DEVICE_ID_LEN 33  /* 32 hex + '\0' */

/* eFuse MAC（小写无分隔符，12 hex 字符） */
void app_identity_mac(char out[13]);

/* 派生 device_id（确定性，刷机不变） */
void app_identity_device_id(char out[APP_DEVICE_ID_LEN]);

bool app_identity_has_secret(void);
esp_err_t app_identity_save_secret(const char *secret_hex);
const char *app_identity_secret(void); /* 64 hex，未设置时为 NULL */

/**
 * 生成 REST 签名材料（ts/nonce/sig 均为输出缓冲）。
 * path 可含 query，规范化时自动截断到 '?' 前 —— 与服务端一致。
 */
esp_err_t app_identity_sign(const char *method, const char *path,
                            char ts[16], char nonce[17], char sig[65]);

#endif /* APP_IDENTITY_H */
