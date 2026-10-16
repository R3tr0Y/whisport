#include "app_identity.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_store.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

static const char *TAG = "identity";
static char s_secret[APP_SECRET_HEX_LEN]; /* 常驻 RAM，避免热路径反复读 NVS */

static void bytes_to_hex(const uint8_t *src, size_t n, char *out) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = h[src[i] >> 4];
    out[i * 2 + 1] = h[src[i] & 0xf];
  }
  out[n * 2] = '\0';
}

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void app_identity_mac(char out[13]) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac); /* eFuse 出厂值，任何刷机方式不变 */
  bytes_to_hex(mac, 6, out);
}

void app_identity_device_id(char out[APP_DEVICE_ID_LEN]) {
  char mac[13];
  app_identity_mac(mac);

  uint8_t digest[32];
  mbedtls_sha256((const unsigned char *)mac, strlen(mac), digest, 0);
  bytes_to_hex(digest, 16, out); /* 前 16 字节 */
}

bool app_identity_has_secret(void) {
  if (s_secret[0] == '\0') {
    if (!app_store_get_secret(s_secret, sizeof(s_secret))) {
      s_secret[0] = '\0';
    }
  }
  return s_secret[0] != '\0';
}

esp_err_t app_identity_save_secret(const char *secret_hex) {
  if (strlen(secret_hex) != 64) return ESP_ERR_INVALID_ARG;
  ESP_RETURN_ON_ERROR(app_store_set_secret(secret_hex), TAG, "save secret");
  strlcpy(s_secret, secret_hex, sizeof(s_secret));
  return ESP_OK;
}

const char *app_identity_secret(void) {
  return app_identity_has_secret() ? s_secret : NULL;
}

esp_err_t app_identity_sign(const char *method, const char *path,
                            char ts[16], char nonce[17], char sig[65]) {
  const char *secret = app_identity_secret();
  if (!secret) return ESP_ERR_INVALID_STATE;

  char device_id[APP_DEVICE_ID_LEN];
  app_identity_device_id(device_id);

  /* ts（秒）与 nonce（8 字节随机） */
  snprintf(ts, 16, "%lld", (long long)time(NULL));
  uint8_t raw[8];
  esp_fill_random(raw, sizeof(raw));
  bytes_to_hex(raw, 8, nonce);

  /* 规范化 path：去掉 query */
  char clean_path[256];
  strlcpy(clean_path, path, sizeof(clean_path));
  char *q = strchr(clean_path, '?');
  if (q) *q = '\0';

  /* v1\n{device_id}\n{ts}\n{nonce}\n{method}\n{path} */
  char msg[512];
  int n = snprintf(msg, sizeof(msg), "v1\n%s\n%s\n%s\n%s\n%s", device_id, ts, nonce,
                   method, clean_path);
  if (n < 0 || (size_t)n >= sizeof(msg)) return ESP_ERR_INVALID_SIZE;

  /* secret hex → bytes */
  uint8_t key[32];
  for (int i = 0; i < 32; i++) {
    int hi = hex_nibble(secret[i * 2]), lo = hex_nibble(secret[i * 2 + 1]);
    if (hi < 0 || lo < 0) return ESP_ERR_INVALID_STATE;
    key[i] = (uint8_t)((hi << 4) | lo);
  }

  uint8_t mac_out[32];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  int rc = mbedtls_md_hmac(md, key, sizeof(key), (const unsigned char *)msg, (size_t)n, mac_out);
  if (rc != 0) return ESP_FAIL;

  bytes_to_hex(mac_out, 32, sig);
  return ESP_OK;
}
