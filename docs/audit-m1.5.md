# Whisport M1.5 审计报告

审计对象：`firmware/`（ESP32-C3，IDF 5.5.3）+ `web/`（Pages Functions + D1/KV/DO）+ `web/public/` 后台
基线：HEAD `e277db0` + 工作树未提交的 M1.5 WIP（配网页 `main/net_prov.c`、业务桥 `app_actions.h`、主菜单 UI）
方法：源码逐文件审读 + `build/whisport.map` / `riscv32-esp-elf-size` 实测 + 后端路由与迁移逐条核对
注意：`build/whisport.bin`（1,335,744 B，02:36 构建）**不含配网页**，配网相关体积为估算值。

## 度量基线（实测，.flash.text + .flash.rodata 按归档归并）

| 组件 | flash | 说明 |
|---|---|---|
| LVGL | 332 KB | 其中 125 个编译单元；未用控件约 60 KB |
| Wi-Fi（net80211/pp/wpa/phy） | ~336 KB | 含 SAE 10 KB、Enterprise 支持 |
| mbedTLS（crypto+TLS+x509） | ~200 KB | 其中 `x509_crt_bundle.S.obj` 69 KB（全量根证书） |
| lwIP | 114 KB | |
| 其余（nvs/spi/crypto/console…） | ~270 KB | console+linenoise ~22 KB |

静态 RAM：`.dram0.data` 13,444 B + `.dram0.bss` 48,256 B ≈ 61.7 KB（LVGL 自身堆 24 KB 另计）。
app 分区 2.5 MB，当前占用 1.27 MB（余 49%）。**结论：flash 不是当前瓶颈，RAM/栈才是。**

---

## 严重

**S1 · 配网页在 4 KB 栈的 httpd 任务里做 TLS 握手 → 栈溢出崩溃**
- `firmware/main/net_prov.c:205`（`cfg.stack_size = 4096`）+ `net_prov.c:190`（`h_claim`）/ `net_prov.c:181`（`h_name`）
- 现象：两个 handler 分别经 `app_claim` / `app_set_label` 调 `app_api_*`，在 handler 上下文中跑完整 mbedTLS 握手（典型需 5–8 KB 栈）。ESP-IDF 官方明确警示 httpd 默认栈不足以做 TLS。
- 建议：`stack_size = 10240`；或 handler 只投递请求到专用任务并立即返回 `{"ok":true,"pending":true}`。
- 成本/收益：+6 KB RAM/任务，收益=消除手机点「认领/改名」即 panic 重启的高概率崩溃。

**S2 · SoftAP 密码可在广播域内推导出来 + 配网页零鉴权**
- `firmware/components/app_net/app_net.c:138-147`（密码 = `device_id[0..7]`，而 `device_id = SHA256(MAC)[:16]`）
- 现象：AP 的 BSSID 就是设备 MAC（在 beacon 里明文广播），任何在射频范围内的人算出 `device_id` 即得 Wi-Fi 密码；`net_prov.c:202` 的 HTTP 服务无任何鉴权/PIN，进页即可改 Wi-Fi、改名、提交认领码。LAN 模式下（`main.c:582` 无条件启动）同一 Wi-Fi 内任何人亦可。
- 建议：AP 密码改为开机随机 8 位（`esp_random`，存 NVS，屏幕显示）；配网页对**所有写操作**加「屏幕上显示的 4 位 PIN」校验（proof-of-possession），不改 Wi-Fi 时至少要求 PIN。
- 成本/收益：约 40 行代码 + 300 B flash；收益=堵住「陌生人可劫持设备网络、可指到恶意 AP 做中间人」这条链。

**S3 · 公告响应 64 KB 硬截断 → JSON 解析失败 → 游标不推进 → 永久无法同步**
- `firmware/components/app_api/app_api.c:72-88`（`cap > 64*1024` 时 `break`，留下被截断的 body）+ `web/core/routes/announcements.ts:30`（`LIMIT 50`）与 `announcements.ts:52`（`body` 允许 8000 字符）
- 现象：50 × 8 KB 正文可达 400 KB；固件缓冲封顶 64 KB 后截断，`cJSON_ParseWithLength` 失败，`main.c:264-268` 直接返回 6，**游标不前进** → 此后每 5 分钟必然重复失败，除非服务端数据变化。
- 建议：服务端给 `GET /api/announcements` 加响应体积上限（如 ≤24 KB）+ `has_more` 让设备循环拉；固件侧检测截断并至少推进游标/记录错误。
- 成本/收益：后端零成本、固件约 20 行；收益=消除「离线久了就再也收不到公告」的静默故障。

**S4 · 已删除/已过期公告在设备端永不消失**
- `firmware/main/main.c:49-68`（`cache_insert` 只增不删）+ `main.c:270-295`（同步只处理 `id > cursor` 的新条目）
- 现象：后台删除或过期后，卡片会一直留在 RAM 缓存里直到重启（或积满 6 条被挤掉）；固件甚至没有读取响应里的 `expires_at`（`main.c:281-287`）。
- 建议：`app_announcement_t` 增加 `expires_at`，每次同步后剔除过期项；删除靠服务端在拉取响应里带 `removed_ids`（或改为「每次重取最近 N 条」）。
- 成本/收益：+8 B/条 RAM（≈48 B）、约 30 行；收益=修复「后台删了、设备还在播」的内容一致性缺陷。

**S5 · Wi-Fi 口令明文编进固件镜像并可被提取**
- `firmware/sdkconfig:503`（`CONFIG_WS_WIFI_PASS="l4qy4cbz"`）、`:502`（SSID）；`app_net.c:74-78` 会把它作为「开发种子」持久化进 NVS
- 现象：`sdkconfig` 已被 gitignore（未提交，OK），但口令会进入 `whisport.bin`，`strings` 即可读出；任何拿到固件/OTA 包的人得到现场 Wi-Fi 口令；且种子一旦写入 NVS 就长期驻留。
- 建议：把 SSID/口令改为可通过私有 `wifi_secrets.h`（gitignore）注入或仅用 `menuconfig`（即从 `sdkconfig.defaults` 移除默认值），发版前清空；并用配网页作为唯一正式入口。
- 成本/收益：零 flash；收益=消除现场口令随固件外泄。

**S6 · 配网页与同步任务互锁 + handler 内阻塞，可能丢响应**
- `firmware/main/main.c:159/198/212`（`s_act_mutex` 递归锁）与 `main.c:455-482`（`sync_task` 每 5 min 持锁跑最长 15 s HTTPS）
- 现象：手机点「认领」时若正好撞上周期同步，httpd 任务会阻塞在 `xSemaphoreTakeRecursive` 上等待，而 httpd 默认 `send_wait_timeout=5s` → 浏览器拿到空响应/超时，用户以为失败并重复提交（服务端配对码一次性的，第二次直接 404）。
- 建议：`app_claim`/`app_set_label` 改为「失败即以 409/202 明确告知重试」；把同步锁粒度收紧到只保护 NVS/缓存的写入，HTTP 阶段不加锁（或用独立的「动作队列」串行化）。
- 成本/收益：约 30 行重构；收益=消除配网页「点了没反应」的偶发体验故障。

---

## 重要

**I1 · `sync_task` 8 KB 栈对 TLS 偏紧**
- `firmware/main/main.c:583`（`xTaskCreate(..., 8192, ...)`）与 `app_api.c:26`（`buffer_size = 2048`）
- 建议：加一次性 `uxTaskGetStackHighWaterMark` 日志实测；若余量 <1.5 KB 提到 10 KB。成本 +2 KB RAM。

**I2 · 配网页非 TLS 且解析未防 XSS**
- `firmware/main/net_prov.c:72`（把 `a.ssid` 直接拼进 `innerHTML`）
- 现象：恶意 SSID（含 `<img onerror=…>`）可在配网页执行 JS，进而调用 `/api/wifi` 改写设备配置。
- 建议：页面里加一个 `esc()` 再做插值（后端 HTML 已有 `textContent` 的用法可照搬）。成本约 5 行。

**I3 · `httpd_req_recv` 未循环读满 body**
- `firmware/main/net_prov.c:106-114`
- 现象：`httpd_req_recv` 允许少读，短 body 通常一次读完，但网络分段下会截断 JSON → 报 `bad body`。
- 建议：按 `req->content_len` 循环 `recv` 直到读满或超时。成本约 8 行。

**I4 · 服务端公告字段上限与设备能力不匹配（正文 8000 vs 480）**
- `web/core/routes/announcements.ts:52`、`firmware/main/ui.h:18-19`（`ANN_BODY_MAX 480`）
- 现象：正文超 480 字符被 `strlcpy` 静默截断，屏幕上的「UP/DN scroll」滚不到后面的内容，用户以为正文就这些。
- 建议：`?fields=summary` 或服务端直接按 480 校验并在后台表单提示计数；顺带减小 S3 的截断概率。成本=0，收益=一致性与带宽。

**I5 · 完全没有 OTA，但分区表为双 OTA 预留了 2.5 MB**
- `firmware/partitions.csv:10-11`（`ota_0`/`ota_1` 各 2.5 MB）、`main.c` 无任何 `esp_https_ota` 调用
- 现象：M1.5 已真机跑通，但现场设备改一行代码也要 USB 重刷；`ota_1` 2.5 MB 完全闲置。
- 建议：M2 前接入 `esp_https_ota`（IDF 自带、可复用现有 HMAC 鉴权下载固件 URL），或在确定不做 OTA 前把 `ota_1` 划给 `assets`。成本：OTA ≈ +30 KB flash（`app_update` 已在链接中，增量小）+ 1 个后台接口。

**I6 · 省电与交互缺口：背光常亮、无空闲休眠、无离线显式提示**
- `firmware/main/main.c:561`（`bsp_display_backlight(100)` 后无任何调节）、`ui.c:443-475`（无公告时全卡片隐藏 → 白屏一片）
- 现象：屏幕 100% 常亮，5 分钟一次轮询，无背光超时/深睡；公告为 0 条时用户看到空白区域不知道是空还是坏。
- 建议：加 30 s 无操作调暗 / 60 s 熄屏（按键唤醒），空列表显示「暂无公告」。成本约 40 行 + 1 个 `esp_timer`；收益=待机功耗与首次开机体验。

**I7 · 5 分钟同步无 jitter（惊群）+ Wi-Fi 重连无退避**
- `firmware/main/main.c:38`（固定 `SYNC_INTERVAL_MS`）、`firmware/components/app_net/app_net.c:33-37`（`DISCONNECTED` 立即 `esp_wifi_connect()`）
- 现象：批量上电/断电恢复后所有设备同刻打 Cloudflare（免费额度下会互相挤压）；密码错误时形成高频重连循环（费电、刷日志、路由侧异常）。
- 建议：`SYNC_INTERVAL_MS + esp_random()%60s` 抖动；重连改为最多 5 次后每 30 s 一次。成本约 10 行。

**I8 · 匿名 `reactivate` 可为任意伪造 MAC 建行 → D1 膨胀**
- `web/core/routes/devices.ts:62-79`（未知 MAC 直接 `INSERT ... 'pending'`，限速桶按 MAC：`devices.ts:66`）
- 现象：攻击者换 MAC 即可无限写入 `devices` 行；清理只在管理员打开后台时惰性触发（`core/cron.ts:63-73`）→ 后台设备列表被垃圾记录污染。
- 建议：该接口同时按 IP 限速（如 20/h）并对未知 MAC 只记 KV 计数、不落 D1。成本约 5 行。

**I9 · 后台缺分页/编辑，公告编辑还与游标语义冲突**
- `web/core/routes/announcements.ts:127`（`LIMIT 100` 无分页）、`web/core/routes/admin.ts:20-29`（设备列表全量）、`web/public/assets/views/announcements.js:4-8`（每屏 3 个接口全量拉）
- 现象：公告 >100 条后后台看不到旧条目；公告只能删不能改（D7 记录为有意取舍）。**注意：即使加编辑，设备的 `since=cursor` 增量语义也不会回传旧 id 的修改**，必须靠 `revision` 字段或再次推送提示。
- 建议：编辑=「新 id 重发 + 旧条作废」或加 `revision` 并让设备按 `since` 拉 `rev > n`；列表加 `before_id` 分页（审计页已有范式可抄）。成本：后端约 40 行。

**I10 · 密钥不可轮换、无备份**
- `docs/deployment.md`（`SECRET_ENC_KEY` 一旦设置不可更换）、`web/core/env.ts:11`；仓库内无 D1 导出/备份脚本
- 现象：密钥泄露或误删即全体设备重新认领；D1 无定期导出。
- 建议：`secret_enc` 增加 `key_id` 前缀，双密钥并存下惰性重加密；加 `scripts/backup.mjs`（`wrangler d1 export` + 定时执行）。成本低；收益=可恢复性与轮换能力。

**I11 · 限速的两处边界（NAT 误伤 + KV 最终一致）**
- `web/core/auth.ts:101-118`、`web/core/util.ts:58-59`（`cf-connecting-ip` 直接作桶键）
- 现象：家庭/企业 NAT 下多设备共享出口 IP，`claim` 10 次/小时会互相挤兑（`auth.ts:14`）；KV 最终一致，突发可绕过；KV 故障时按 D9 直接放行。
- 建议：`claim` 桶改为 `ip+mac` 双计数（任一超限才拒绝）；429 附带 `Retry-After`。成本约 10 行。

**I12 · 文档与实现漂移**
- `docs/api.md:9`（写「重激活 POST 10 次/时、按 IP」）vs `web/core/routes/devices.ts:66`（30 次/时、按 MAC）
- 建议：改文档；顺手把 `docs/deployment.md` 的「固件骨架尚未编译」验收项更新为真机已通。成本=0。

---

## 可选

**O1 · LVGL 未用控件 ≈ 60 KB 可省**
- `firmware/sdkconfig`（`CONFIG_LV_USE_*` 几乎全 `=y`；UI 只用到 obj/label/flex/scroll）
- 实测：`lv_scale` 6.8 KB、`lv_chart` 5.5 KB、`lv_textarea` 5.2 KB、`lv_dropdown` 4.8 KB、`lv_buttonmatrix` 4.7 KB、`lv_arc` 4.1 KB、`lv_roller` 3.6 KB、`lv_calendar` 3.4 KB、`lv_table` 3.2 KB、`lv_theme_simple+mono`、`lv_math` 1.6 KB 等，合计 ≈60 KB。
- 建议：在 `sdkconfig.defaults` 显式 `CONFIG_LV_USE_X=n`（保持 `LV_USE_FLEX/LABEL/OBJ`）。收益 60 KB（约 app 的 4.7%），成本=一次全屏回归验证。

**O2 · 证书包 69 KB → 自定义 Cloudflare 根 CA 约省 65 KB**
- `firmware/sdkconfig.defaults:34`（`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`）
- 收益：65 KB（最大单项可砍）。代价：Cloudflare 换根/换链时需随固件更新——**若无 OTA 则不建议现在做**，两者有依赖关系。

**O3 · 字体与 Wi-Fi 细项**
- `lv_font_montserrat_20`（21.8 KB，仅用于标题与信息屏标题）→ 全用 14 可省 ≈21 KB；`CONFIG_ESP_WIFI_ENABLE_WPA3_SAE`（`sae.c` 10 KB）与 `ESP_WIFI_ENTERPRISE_SUPPORT`（`apply_sta_config` 用的是 `WIFI_AUTH_OPEN` 阈值，不需要）可关，省 ≈15 KB。
- 注意：关 SAE 后无法连 WPA3-only 热点；关 Enterprise 无影响。

**O4 · 配网页带来的新增体积（估算，未编译验证）**
- `libesp_http_server.a` 实测符号文本 19.4 KB + 页面 HTML 2.2 KB（`net_prov.c:22-86`）+ 处理函数与 cJSON 复用 ≈ **+25–30 KB**。
- 建议：合并后重跑 `idf.py size` 记录；这也说明 M1.5 后 app 仍 <1.35 MB（余 46%）。

**O5 · 可观测性缺位**
- 全仓无 `esp_task_wdt` 注册、无 `uxTaskGetStackHighWaterMark`、无 `heap_caps_get_free_size` 日志（grep 无命中）
- 建议：加 30 s 一次的堆/任务水位 ESP_LOGI（仅 DEBUG 级，发布关）。成本 ~20 行 + 1 KB flash；收益=长跑问题可定位。

**O6 · 设备端文案与小缺陷**
- `firmware/main/main.c:291`（`synced %u new announcement(s)` 在 `ORDER BY id DESC` 下，条目 0 才是最新，旧版 `main.c:264` 的「最新 #items[n-1]」标签已删除，注意别回退）
- `ui.c:472-473`（`s_sel` 夹紧逻辑正确，但列表从 6 条降到 0 条时不重置高亮）
- `net_prov.c:194` 扫描阻塞（`esp_wifi_scan_start(..., true)`）会让 AP 短暂抖动，建议 `scan_time.active.max = 300` 并限制结果数。
- `net_prov.c:167-179` 未校验 SSID 长度与空密码边界（`strlcpy` 静默截断）。

**O7 · 前端状态反馈**
- `web/public/assets/views/announcements.js:99`（发布成功后 toast 只写「在线设备将收到提示」，实际提示是尽力而为的，失败被 `announcements.ts:180` 吞掉）
- 建议：toast 文案改为「已发布（在线设备将收到提示）」，并在 `notifyAnnounce` 失败时后台记一条审计。成本 ~5 行。

---

## 不建议做的（过度工程）

- **BLE/NimBLE 配网**：C3 上 BT 与 Wi-Fi 共存的内存/射频代价大（>150 KB flash + 大量 RAM），现有 SoftAP + PIN 方案足够。
- **自研 OTA 框架/双写回滚**：`esp_https_ota` + `app_update` 已提供分区切换与回滚，别自己写 bootloader 逻辑。
- **关闭 TLS 证书校验或去掉 `crt_bundle`**（为省体积）：S2 的中间人风险正需要服务端身份校验兜底，安全不可让位于 65 KB。
- **公告富文本/Markdown/图片渲染**：240×320 单色卡片流承载不了，收益远低于把中文点阵字库做对（M4 目标）。
- **把 CJK 字库编译进 app**：应在 M4 放 `assets` SPIFFS 动态加载，编译进去会立刻吃掉 OTA 余量。
- **自建限速/去重服务（Redis/D1 计数表）**：M1 规模下 KV 尽力而为 + 审计日志足够，D9 的取舍成立。
- **重构 UI 为 LVGL 多屏 `lv_screen` 栈**：当前 `show_only()` 单层容器切换已满足 4 屏，重构只增风险。
