# Whisport 固件（M1 骨架）

AI Passport（ESP32-C3）公告板 + PTT 对讲固件。当前为 **M1 里程碑骨架**：身份链路 + 公告拉取，面向 bring-up；音频/WSS 对讲属 M2（见根目录规划 §7）。

> ⚠️ 本骨架在无 ESP-IDF 环境下编写，尚未经 `idf.py build` 编译验证。首次编译若有小错（多为头文件路径），按报错微调即可，模块划分与 API 语义已与云端服务端（`worker/`）对齐并通过其 19 项集成测试。

## 模块（components/）

| 组件 | 职责 | 规划对应 |
|---|---|---|
| `app_store` | NVS 凭证/游标/已读标记（独立 `storage` 分区，L2 落点） | §7.1 store |
| `app_identity` | eFuse MAC → device_id 派生、HMAC 签名、secret 管理 | §7.1 identity |
| `app_net` | Wi-Fi STA 连接与事件 | §7.1 net_mgr |
| `app_api` | HTTPS 客户端 + 设备签名头 + cJSON 解析 | §7.1 proto(REST 部分) |
| `app_ui` | 控制台 UI（`pair`/`status`/`sync` 命令），LVGL 屏幕界面待接入 BSP 后实现 | §7.1 ui |

## 构建

```bash
# 依赖 ESP-IDF v5.1+，target esp32c3
idf.py set-target esp32c3
idf.py menuconfig   # Whisport → 服务器 URL / Wi-Fi SSID/密码
idf.py build flash monitor
```

若组件仓库无内置 cJSON：`idf.py add-dependency "espressif/cjson"`。

## 控制台命令（USB 串口）

```
pair <6位配对码>   # 认领/重新绑定设备（写入 device_secret 到 NVS）
status             # 查看 device_id / 在线状态 / 凭证状态
sync               # 立即拉取一次公告
```

无凭证时固件会每小时自动向服务器发起一次重新激活请求，管理员在后台批准后，下次轮询自动取回新密钥（规划 §3.4）。

## 凭证层级（规划 §3.3）

- L2：`storage` NVS 分区（当前实现，默认落点）
- L1：eFuse 用户 KEY 块（TODO：配网烧录工具，README 后续补充）
- L3：无凭证 → 重新激活流程（已实现）

## 与官方固件集成

组件均为纯 ESP-IDF 实现、不依赖板级 BSP。接入 FoloToy AI-Passport 官方工程时，把 `components/app_*` 复制进其 `components/` 目录，并在其 UI 任务中调用：

```c
app_api_sync_announcements();          // 拉取
app_ui_show_announcements(&list);      // 换成 LVGL 渲染
```
