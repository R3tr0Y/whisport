# Whisport 固件（M1）

AI Passport（ESP32-C3）公告板 + PTT 对讲固件。**板级底座直接复用官方工程 BSP**，应用层全新设计。

> 基线：[FoloToy/ai-passport](https://gitee.com/FoloToy/ai-passport)（ESP-IDF **5.5.3**，main @ a06235e）
> 本骨架未经 `idf.py build` 编译验证（开发机无 ESP-IDF）；API 均按官方 BSP 头文件签名对接。

## 架构

```
firmware/
├── main/                 应用层（官方规范：页面/状态机/任务放 main，UI 独立重设计）
│   ├── main.c            启动序（对齐官方）· 认领/重新激活/公告同步 · 按键队列 · console
│   └── ui.c / ui.h       全新 LVGL UI：顶栏状态/公告卡片流（不碰 ui_pixel/demo 外壳）
└── components/           业务组件（非板级，不进官方 bsp）
    ├── app_store         NVS：L2 凭证/游标/已读（独立 storage 分区）
    ├── app_identity      MAC→device_id 派生、HMAC 签名（与云端逐字节一致）
    ├── app_net           Wi-Fi STA（官方 main 分支只有扫描 demo，连接自实现）
    └── app_api           HTTPS 客户端 + 设备签名头 + cJSON

板级（经 EXTRA_COMPONENT_DIRS 引用 vendor/ai-passport/components/bsp）：
  bsp_audio（ES8311 全双工裸 PCM 16kHz）· bsp_button（UP/DOWN/OK，5 类事件）
  bsp_display + LVGL 9.5（ST7789 240×320）· bsp_battery（CW2017）· bsp_i2c
```

官方仓库克隆位置（gitignore，不入本仓库）：

```bash
git clone --depth 1 https://gitee.com/FoloToy/ai-passport vendor/ai-passport
```

## 构建

```bash
# 依赖 ESP-IDF 5.5.3（官方基线严格版本，勿用其它）
source ~/esp/esp-idf/export.sh && idf.py --version   # 须输出 v5.5.3

idf.py set-target esp32c3
idf.py menuconfig    # Whisport → Wi-Fi SSID/密码（服务地址默认 https://whisport.pages.dev）
idf.py build flash monitor
```

官方仓库自带门禁可作对照参考（在 vendor/ai-passport 下 `./tools/validate.sh`）；本工程尚未移植其 host-tests 门禁。

## 控制台与按键

```
pair <6位配对码>   # 认领/重新绑定（密钥写入 NVS，刷机后免配置恢复）
sync               # 立即拉取公告
status             # device_id / 凭证 / 网络
```

| 按键 | M1 | M2（规划 §7.2） |
|---|---|---|
| 长按 | 立即同步 | PTT 说话 |
| 短按 | 预留（列表滚动） | 确认/接听 |
| 双击 | 预留 | 返回/拒接 |

无凭证时每小时自动发起重新激活请求，管理员批准后下次轮询取回新密钥。

## 8MB flash 预算（决策 D11）

| 分区 | 大小 | 用途 |
|---|---|---|
| nvs + otadata | 28KB | 系统 |
| ota_0 / ota_1 | 各 2.5MB | 双 OTA（官方默认单 factory 7.9MB；whisport 需 OTA 场景故自定义，官方门禁允许） |
| storage | 256KB | L2 凭证/游标（NVS，实际 <10KB） |
| assets | 2.625MB | M4 预留：LVGL 中文字库/图标（SPIFFS） |

体积手段：`-Os`、newlib-nano、TLS 仅客户端、TLS 缓冲 4KB、HTTP 响应 64KB 上限。
app 超 2.5MB 时降级链：裁字库 → 降 Opus 码率 → 弃双 OTA 换单 app。

## 与官方规范的对齐点（fork 约束）

- **UI 强制重设计**：本应用 UI 为独立设计（深色卡片流 + 自有配色），未使用官方 demo 菜单/页面/`ui_pixel` 外壳
- 按键回调只入队（禁阻塞）；LVGL 操作全部经 `bsp_lvgl_lock/unlock`
- 中文 UI（M4）须过官方中文字体检查清单（Montserrat 无中文字形，现界面为英文/数字）
- 内存预算：无 PSRAM——LVGL 24KB 池 + Wi-Fi + TLS +（M2）Opus 需持续评估连续堆块

## M2 预告（对讲）

官方无网络音频链路，需自建：`bsp_audio_read/write`（裸 PCM 16kHz）→ Opus 编码 → 200ms 攒包 → WSS。
官方参考文档 `docs/reference/shinku-chen/` 的实测结论：Opus 6kbps ≈ 1KB/s，解码器 60–80KB flash。
