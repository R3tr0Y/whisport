/**
 * app_api — HTTPS 客户端（规划 §6.3 REST 部分）
 *
 * - app_api_signed_*：携带设备 HMAC 签名头（X-Device-Id/X-Timestamp/X-Nonce/X-Signature）
 * - app_api_anon_*  ：匿名请求（认领 / 重新激活）
 *
 * 详细协议见 docs/api.md。
 */
#ifndef APP_API_H
#define APP_API_H

#include <stddef.h>

#include "esp_err.h"

typedef struct {
  int status;      /* HTTP 状态码 */
  char *body;      /* 响应体（NUL 结尾，堆分配，用 app_api_free 释放） */
  size_t body_len;
} app_api_resp_t;

/* GET（签名） */
esp_err_t app_api_signed_get(const char *path_query, app_api_resp_t *out);

/* 匿名 POST，json_body 为请求体 */
esp_err_t app_api_anon_post(const char *path, const char *json_body, app_api_resp_t *out);

/* 匿名 GET（重新激活轮询用） */
esp_err_t app_api_anon_get(const char *path_query, app_api_resp_t *out);

void app_api_free(app_api_resp_t *r);

#endif /* APP_API_H */
