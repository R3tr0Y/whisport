#include "net_prov.h"

#include <stdio.h>
#include <string.h>

#include "app_actions.h"
#include "app_net.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "prov";

static httpd_handle_t s_httpd = NULL;
static bool s_ap_mode = false;
static char s_pin[5]; /* 4 位管理 PIN，开机随机，屏幕展示 */

const char *net_prov_pin(void) {
  return s_pin;
}

/* ---------------- 页面（HTML，中文可直接使用：浏览器渲染） ---------------- */

static const char PAGE_HTML[] =
  "<!doctype html><html lang=zh-CN><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>whisport 设备配置</title><style>"
  "body{font:15px/1.6 system-ui,'PingFang SC','Microsoft YaHei',sans-serif;"
  "background:#0f1318;color:#e6e9ef;margin:0;padding:16px;max-width:560px}"
  "h1{font-size:18px;color:#e8a33d;margin:4px 0 14px}"
  "h2{font-size:15px;margin:18px 0 8px;border-bottom:1px solid #2a3240;padding-bottom:4px}"
  ".card{background:#1a212b;border:1px solid #2a3240;border-radius:10px;padding:12px;margin:10px 0}"
  "input,select,button{font:inherit;width:100%;box-sizing:border-box;margin:4px 0;padding:8px;"
  "background:#0f1318;color:#e6e9ef;border:1px solid #2a3240;border-radius:8px}"
  "button{background:#e8a33d;color:#0b1524;font-weight:600;border:none}"
  ".muted{color:#8b94a3;font-size:13px}.ok{color:#6fcf87}.bad{color:#e07070}"
  "ul{list-style:none;padding:0;margin:0}li{padding:6px 0;border-bottom:1px solid #232b36}"
  "</style></head><body>"
  "<h1>whisport 设备配置</h1>"
  "<div class=card><div>设备 ID：<span id=did class=muted>-</span></div>"
  "<div>状态：<span id=st class=muted>-</span></div>"
  "<div>设备名：<b id=label>-</b></div>"
  "<input id=pin inputmode=numeric maxlength=4 placeholder='屏幕上的 4 位 PIN（修改配置必填）'></div>"

  "<h2>网络</h2><div class=card>"
  "<div id=netst class=muted>读取中…</div>"
  "<button id=scanbtn>扫描附近 Wi-Fi</button>"
  "<ul id=aps></ul>"
  "<input id=pwd type=password placeholder='Wi-Fi 密码（开放网络留空）'>"
  "<button id=savebtn>保存并连接</button></div>"

  "<h2>设备名称</h2><div class=card>"
  "<input id=name maxlength=32 placeholder='给这台工牌起个名字'>"
  "<button id=namebtn>保存名称</button></div>"

  "<h2>设备认领</h2><div class=card>"
  "<div class=muted>输入管理员生成的 6 位配对码（已认领则可忽略）</div>"
  "<input id=code inputmode=numeric maxlength=6 placeholder='6 位配对码'>"
  "<button id=claimbtn>认领设备</button></div>"

  "<p class=muted>whisport · 公告板 + 对讲系统</p>"
  "<script>"
  "let sel='';"
  "function esc(s){return String(s).replace(/[\\u0026\\u003c\\u003e\\u0022\\u0027]/g,function(c){return '&#'+c.charCodeAt(0)+';'})}"
  "function pinv(){return document.getElementById('pin').value}"
  "async function j(u,m,b){let r=await fetch(u,{method:m||'GET',headers:{'content-type':'application/json'},body:b&&JSON.stringify(b)});return r.json()}"
  "async function st(){try{let d=await j('/api/status');"
  "document.getElementById('did').textContent=d.device_id;"
  "document.getElementById('st').innerHTML=(d.online?'<span class=ok>已联网</span>':'<span class=bad>未联网</span>')+(d.has_secret?' · <span class=ok>已认领</span>':' · <span class=bad>未认领</span>');"
  "document.getElementById('label').textContent=d.label||'(未命名)';"
  "document.getElementById('netst').textContent=d.online?('已连接，IP '+(d.ip||'?')):'未连接';"
  "document.getElementById('name').placeholder=d.label||'给这台工牌起个名字';"
  "}catch(e){}}"
  "document.getElementById('scanbtn').onclick=async()=>{"
  "document.getElementById('aps').innerHTML='<li class=muted>扫描中…</li>';"
  "try{let d=await j('/api/scan');"
  "document.getElementById('aps').innerHTML=d.aps.map(a=>'<li><label><input type=radio name=ap style=\"width:auto\" value=\"'+esc(a.ssid)+'\"> '+esc(a.ssid)+' <span class=muted>'+a.rssi+'dBm'+(a.secure?' 加密':' 开放')+'</span></label></li>').join('')||'<li class=muted>未发现</li>';"
  "document.querySelectorAll('input[name=ap]').forEach(r=>r.onclick=()=>sel=r.value);"
  "}catch(e){}};"
  "document.getElementById('savebtn').onclick=async()=>{"
  "if(!sel)return alert('请先扫描并选择一个 Wi-Fi');"
  "let d=await j('/api/wifi','POST',{ssid:sel,pass:document.getElementById('pwd').value,pin:pinv()});"
  "alert(d.ok?'已保存，正在连接…稍后回到本页刷新查看':'失败：'+(d.error||''));st()};"
  "document.getElementById('namebtn').onclick=async()=>{"
  "let d=await j('/api/name','POST',{label:document.getElementById('name').value,pin:pinv()});"
  "alert(d.ok?'名称已保存':'失败：'+(d.error||''));st()};"
  "document.getElementById('claimbtn').onclick=async()=>{"
  "let d=await j('/api/claim','POST',{code:document.getElementById('code').value,label:document.getElementById('name').value,pin:pinv()});"
  "alert(d.ok?'认领成功！':'失败：'+(d.error||''));st()};"
  "st();setInterval(st,5000);"
  "</script></body></html>";

/* ---------------- 工具 ---------------- */

static void reply_json(httpd_req_t *req, const char *json) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_sendstr(req, json);
}

static void reply_ok(httpd_req_t *req, bool ok, const char *err) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", ok);
  if (!ok && err) cJSON_AddStringToObject(o, "error", err);
  char *s = cJSON_PrintUnformatted(o);
  reply_json(req, s);
  cJSON_free(s);
  cJSON_Delete(o);
}

/* 读 JSON body（上限 512B）；I3：按 content_len 循环读满（网络分段下单次 recv 可能少读） */
static bool read_body(httpd_req_t *req, cJSON **out) {
  if (req->content_len <= 0 || req->content_len > 512) return false;
  char buf[513];
  int total = 0;
  while (total < req->content_len) {
    int got = httpd_req_recv(req, buf + total, req->content_len - total);
    if (got <= 0) {
      if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
      return false;
    }
    total += got;
  }
  buf[total] = '\0';
  *out = cJSON_Parse(buf);
  return *out != NULL;
}

/* I2/S2：写操作必须携带屏幕展示的 PIN */
static bool pin_ok(cJSON *o) {
  const char *pin = cJSON_GetStringValue(cJSON_GetObjectItem(o, "pin"));
  return pin && s_pin[0] && strcmp(pin, s_pin) == 0;
}

static const char *json_str(cJSON *o, const char *k, const char *dflt) {
  const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
  return v ? v : dflt;
}

/* ---------------- handlers ---------------- */

static esp_err_t h_page(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_sendstr(req, PAGE_HTML);
}

static esp_err_t h_status(httpd_req_t *req) {
  char ip[20] = "";
  app_net_get_ip(ip, sizeof(ip));
  char label[64] = "";
  app_get_label(label, sizeof(label));

  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "device_id", app_device_id());
  cJSON_AddBoolToObject(o, "online", app_net_is_connected());
  cJSON_AddBoolToObject(o, "has_secret", app_has_secret());
  cJSON_AddStringToObject(o, "label", label);
  cJSON_AddStringToObject(o, "ip", ip);
  cJSON_AddStringToObject(o, "server", app_server_url());
  char *s = cJSON_PrintUnformatted(o);
  reply_json(req, s);
  cJSON_free(s);
  cJSON_Delete(o);
  return ESP_OK;
}

static esp_err_t h_scan(httpd_req_t *req) {
  static app_net_ap_t aps[16];
  int n = app_net_scan(aps, 16);
  cJSON *o = cJSON_CreateObject();
  cJSON *arr = cJSON_AddArrayToObject(o, "aps");
  for (int i = 0; i < n; i++) {
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "ssid", aps[i].ssid);
    cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
    cJSON_AddBoolToObject(a, "secure", aps[i].secure);
    cJSON_AddItemToArray(arr, a);
  }
  char *s = cJSON_PrintUnformatted(o);
  reply_json(req, s);
  cJSON_free(s);
  cJSON_Delete(o);
  return ESP_OK;
}

static esp_err_t h_wifi(httpd_req_t *req) {
  cJSON *o = NULL;
  if (!read_body(req, &o)) return reply_ok(req, false, "bad body"), ESP_OK;
  if (!pin_ok(o)) {
    cJSON_Delete(o);
    return reply_ok(req, false, "wrong pin"), ESP_OK;
  }
  const char *ssid = json_str(o, "ssid", "");
  const char *pass = json_str(o, "pass", "");
  if (!ssid[0]) {
    cJSON_Delete(o);
    return reply_ok(req, false, "ssid required"), ESP_OK;
  }
  esp_err_t err = app_net_start_sta_creds(ssid, pass);
  cJSON_Delete(o);
  return reply_ok(req, err == ESP_OK, esp_err_to_name(err)), ESP_OK;
}

static esp_err_t h_name(httpd_req_t *req) {
  cJSON *o = NULL;
  if (!read_body(req, &o)) return reply_ok(req, false, "bad body"), ESP_OK;
  if (!pin_ok(o)) {
    cJSON_Delete(o);
    return reply_ok(req, false, "wrong pin"), ESP_OK;
  }
  const char *label = json_str(o, "label", "");
  int rc = label[0] ? app_set_label(label) : -1;
  cJSON_Delete(o);
  return reply_ok(req, rc == 0, rc == 0 ? NULL : "set name failed"), ESP_OK;
}

static esp_err_t h_claim(httpd_req_t *req) {
  cJSON *o = NULL;
  if (!read_body(req, &o)) return reply_ok(req, false, "bad body"), ESP_OK;
  if (!pin_ok(o)) {
    cJSON_Delete(o);
    return reply_ok(req, false, "wrong pin"), ESP_OK;
  }
  const char *code = json_str(o, "code", "");
  const char *label = json_str(o, "label", "");
  int rc = code[0] ? app_claim(code, label[0] ? label : NULL) : -1;
  cJSON_Delete(o);
  return reply_ok(req, rc == 0, rc == 0 ? NULL : "claim failed (check code)"), ESP_OK;
}

/* ---------------- 启动 ---------------- */

esp_err_t net_prov_init(void) {
  snprintf(s_pin, sizeof(s_pin), "%04u", (unsigned)(esp_random() % 10000));

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_open_sockets = 2;      /* 玩具规模：手机一两个连接足够，省 RAM */
  cfg.stack_size = 10240;        /* S1：handler 内做 TLS 握手（认领/改名）需 5-8KB 栈 */
  cfg.lru_purge_enable = true;
  cfg.uri_match_fn = httpd_uri_match_wildcard;

  ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start");

  const httpd_uri_t routes[] = {
      { .uri = "/", .method = HTTP_GET, .handler = h_page },
      { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
      { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan },
      { .uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi },
      { .uri = "/api/name", .method = HTTP_POST, .handler = h_name },
      { .uri = "/api/claim", .method = HTTP_POST, .handler = h_claim },
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    httpd_register_uri_handler(s_httpd, &routes[i]);
  }
  ESP_LOGI(TAG, "config portal ready (%s)", s_ap_mode ? "ap mode" : "sta mode");
  return ESP_OK;
}

bool net_prov_ap_mode(void) {
  return s_ap_mode;
}

void net_prov_set_ap_mode(bool ap) {
  s_ap_mode = ap;
}

bool net_prov_setup_host(char *buf, size_t len) {
  if (s_ap_mode) {
    strlcpy(buf, "192.168.4.1", len);
    return true;
  }
  return app_net_get_ip(buf, len);
}
