#include "app_api.h"

#include <stdio.h>
#include <string.h>

#include "app_identity.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "api";

#define MAX_URL 512
#define INITIAL_BODY 2048

static esp_err_t do_request(const char *url, const char *method, const char *json_body,
                            const char *sig_headers[4][2], app_api_resp_t *out) {
  memset(out, 0, sizeof(*out));

  esp_http_client_config_t cfg = {
      .url = url,
      .method = strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 15000,
      .buffer_size = 2048,   /* 无 PSRAM，压缩缓冲（规划 §7.3） */
      .buffer_size_tx = 1024,
  };
  esp_http_client_handle_t http = esp_http_client_init(&cfg);
  if (!http) return ESP_FAIL;

  if (json_body) {
    esp_http_client_set_header(http, "Content-Type", "application/json");
  }
  if (sig_headers) {
    for (int i = 0; i < 4; i++) {
      esp_http_client_set_header(http, sig_headers[i][0], sig_headers[i][1]);
    }
  }

  size_t cap = INITIAL_BODY;
  char *buf = malloc(cap);
  if (!buf) {
    esp_http_client_cleanup(http);
    return ESP_ERR_NO_MEM;
  }
  size_t len = 0;

  size_t body_len = json_body ? strlen(json_body) : 0;
  esp_err_t err = esp_http_client_open(http, body_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "open failed: %s url=%s", esp_err_to_name(err), url);
    free(buf);
    esp_http_client_cleanup(http);
    return err;
  }

  /* 手动 open 流程必须显式写入 body（set_post_field 仅 perform 流程自动生效） */
  if (body_len > 0) {
    int w = esp_http_client_write(http, json_body, body_len);
    if (w < 0 || (size_t)w != body_len) {
      ESP_LOGE(TAG, "body write failed (%d/%u) url=%s", w, (unsigned)body_len, url);
      free(buf);
      esp_http_client_close(http);
      esp_http_client_cleanup(http);
      return ESP_FAIL;
    }
  }

  int content_len = esp_http_client_fetch_headers(http);
  /* chunked 时 content_len < 0，循环读到 EOF */
  while (1) {
    if (len + 1024 > cap) {
      char *nb = realloc(buf, cap * 2);
      if (!nb) {
        free(buf);
        esp_http_client_cleanup(http);
        return ESP_ERR_NO_MEM;
      }
      buf = nb;
      cap *= 2;
      if (cap > 64 * 1024) break; /* 响应体上限 64KB */
    }
    int n = esp_http_client_read(http, buf + len, cap - len - 1);
    if (n <= 0) break;
    len += (size_t)n;
    if (content_len >= 0 && (int)len >= content_len) break;
  }
  buf[len] = '\0';

  out->status = esp_http_client_get_status_code(http);
  out->body = buf;
  out->body_len = len;

  esp_http_client_close(http);
  esp_http_client_cleanup(http);

  ESP_LOGI(TAG, "%s %s -> %d (%u bytes)", method, url, out->status, (unsigned)len);
  return ESP_OK;
}

esp_err_t app_api_signed_get(const char *path_query, app_api_resp_t *out) {
  if (!app_identity_has_secret()) return ESP_ERR_INVALID_STATE;

  char ts[16], nonce[17], sig[65], device_id[APP_DEVICE_ID_LEN];
  ESP_RETURN_ON_ERROR(app_identity_sign("GET", path_query, ts, nonce, sig), TAG, "sign");
  app_identity_device_id(device_id);

  const char *headers[4][2] = {
      {"X-Device-Id", device_id},
      {"X-Timestamp", ts},
      {"X-Nonce", nonce},
      {"X-Signature", sig},
  };

  char url[MAX_URL];
  snprintf(url, sizeof(url), "%s%s", CONFIG_WS_SERVER_URL, path_query);
  return do_request(url, "GET", NULL, headers, out);
}

esp_err_t app_api_anon_post(const char *path, const char *json_body, app_api_resp_t *out) {
  char url[MAX_URL];
  snprintf(url, sizeof(url), "%s%s", CONFIG_WS_SERVER_URL, path);
  return do_request(url, "POST", json_body, NULL, out);
}

esp_err_t app_api_anon_get(const char *path_query, app_api_resp_t *out) {
  char url[MAX_URL];
  snprintf(url, sizeof(url), "%s%s", CONFIG_WS_SERVER_URL, path_query);
  return do_request(url, "GET", NULL, NULL, out);
}

void app_api_free(app_api_resp_t *r) {
  free(r->body);
  r->body = NULL;
  r->body_len = 0;
}
