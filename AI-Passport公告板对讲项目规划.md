# AI Passport 公告板 + PTT 对讲系统 · 项目规划

> 版本：v1.0（设计定稿）
> 目标平台：FoloToy AI Passport（ESP32-C3 智能工牌） + Cloudflare Pages/Workers（无 VPS）
> 文档性质：架构与实施总纲，作为后续各模块详细设计（API 规范、协议规范、固件设计）的基准

---

## 1. 项目背景与目标

### 1.1 背景

AI Passport 是一款玩具属性的开源智能工牌（ESP32-C3 单核 160MHz，8MB Flash，无 PSRAM，ES8311 音频编解码，I2S 全双工，240×320 彩屏，Wi-Fi/BLE）。官方固件已验证「麦克风采集 → Wi-Fi 上传音频流 → 接收音频流 → 扬声器播放」的完整语音链路，按键原生支持 PTT 按住说话。

本项目为其开发一套**公告板 + 个人语音对讲**应用，后端完全托管于 Cloudflare，不使用任何 VPS。

### 1.2 功能目标

| 编号 | 功能 | 描述 |
|---|---|---|
| F1 | 公告板 | 管理端发布公告，设备端列表查看；支持按设备/按组/全员三级可见性 |
| F2 | 1:1 私密呼叫 | 从在线通讯录选择设备发起呼叫，被叫确认后建立 PTT 半双工通话 |
| F3 | 广播频道 | 持久频道（如全员频道、值班频道），组成员随时加入，共享话语权仲裁 |
| F4 | 私密频道 | 任意设备创建，4–6 位 PIN 准入 |
| F5 | 呼叫保护 PIN | 被叫可设置呼叫保护 PIN，主叫需输对才能建立通话 |
| F6 | 管理后台 | 网页端：发公告、管组、审批设备认领/重新激活、查看审计日志 |

### 1.3 非功能目标

- **无 VPS**：全部后端运行于 Cloudflare（Pages + Functions + D1 + KV + Durable Objects + Cron Triggers）。
- **零配置重刷**：玩具场景下用户高频刷机，普通刷机/OTA 后设备开机即用，权限与通讯录自动恢复。
- **免费额度内运行**：规模定位个人/小团队（≤ 50 设备），控制在 Cloudflare Free Plan 内。
- **半双工对讲体验**：端到端延迟目标 < 800ms，话语权冲突有明确仲裁反馈。

### 1.4 明确排除（Out of Scope）

- 拨打真实电话号码（PSTN）：Cloudflare 无 SIP/RTP 出口，如需应外接 Twilio 类服务，本期不做。
- 全双工同时通话：ESP32-C3 算力与协议复杂度不支持完整 WebRTC，采用 PTT 半双工模型。
- 浏览器/手机网页加入通话（Cloudflare Calls SFU 桥接）：预留架构位，列为二期可选。

---

## 2. 总体架构

```
┌──────────────┐        ┌─────────────────────────────────────────┐
│  AI Passport │ ←WSS→  │ PresenceDO（在线状态 + 控制信道路由）      │
│   （固件）    │        │ RoomDO × N（每个活跃通话/频道一个实例）     │
│              │ ←HTTPS→│ Pages Functions API（REST）              │
└──────────────┘        │ D1（公告/权限/频道/设备/审计）             │
┌──────────────┐        │ KV（会话/票据缓存，可选）                  │
│ 管理后台网页  │ ←HTTPS→│ Cron Trigger（过期清理任务）              │
└──────────────┘        └─────────────────────────────────────────┘
```

**核心设计原则**

1. **一条 WSS 四种职责**：设备开机即建立持久 WSS 连接，同时承担在线状态上报、来电通知、公告更新提示、对讲音频传输。
2. **控制与内容分离**：WSS 只走信令和音频帧；公告正文等内容走 HTTPS API。WSS 上不过大 payload，节省 DO 请求额度。
3. **权限只在服务端裁决**：设备端固件视为不可信终端，所有可见性过滤、PIN 校验、话语权仲裁均在服务端完成。
4. **硬件身份锚定**：设备身份以 eFuse MAC 为主键，刷机不产生新记录（详见 §3）。

---

## 3. 设备身份体系（抗高频刷机设计）

### 3.1 问题定义

玩具属性决定用户会频繁刷写各种固件，NVS 中的凭证会反复丢失。若身份依赖随机分配 ID + NVS 存储，将导致：每次刷机重新配对、服务端堆积孤儿设备记录、权限配置反复重设。

### 3.2 身份锚点

- **主键 = 芯片 MAC**（eFuse 出厂唯一，任何刷机方式不变）。
- `devices` 表以 MAC 派生值（如 SHA-256(MAC) 前 16 字节）为主键。
- 注册幂等：设备上线报 MAC，已知 → 凭证校验；未知 → 创建「待认领」记录。刷机 N 次仍是同一条记录。

### 3.3 凭证三层持久化

| 层级 | 存储位置 | 抵御场景 | 丢失后 |
|---|---|---|---|
| L1 | eFuse 用户 KEY 块（首次配对时配网工具烧入，设读保护） | 一切，含 `erase_flash` 整片擦除 | 不会丢失 |
| L2 | 加密 NVS（默认落点） | 普通刷写、OTA 升级（不触碰 NVS 分区） | 降级 L3 |
| L3 | 无本地凭证 | 分区表布局不同的第三方固件来回刷 | 主人后台一键确认重新激活 |

### 3.4 生命周期流程

**首次认领（仅一次）**
1. 管理后台生成一次性配对码（6 位，10 分钟有效）。
2. 设备输入配对码 → 上报 MAC → 服务端创建/激活设备记录，签发 `device_secret`。
3. 设备写入 eFuse（支持时）+ NVS。

**刷机后自动恢复**
1. 设备读取 L1/L2 凭证 → HMAC 签名握手 → 上线。
2. 权限、组关系、频道成员资格随主键自动恢复，零配置。

**重新激活（极端情况）**
1. 设备无有效凭证，匿名连 WSS 报 MAC → 进入「待激活」。
2. 服务端发现 MAC 已注册但缺凭证 → 管理后台向主人推送「设备请求重新激活」。
3. 主人点击确认 → 重签 secret 下发 → 设备落盘恢复。

> 安全说明：MAC 非秘密（Wi-Fi 扫描可见），禁止「裸 MAC 换凭证」，重新激活必须经主人确认，防止拾获冒用。

**转让与重置**
- 解绑：清除主人关系，设备回到待认领，新主人走首次认领。
- 出厂重置：连记录带审计一并删除。

### 3.5 服务端防浪费机制

| 机制 | 说明 |
|---|---|
| 幂等注册 | MAC 主键，刷机不产生新记录 |
| 待认领过期 | 未认领 MAC 记录 24h 自动清除（Cron Trigger） |
| 注册限速 | 按 IP 限流，防随机 MAC 灌库 |
| 审计滚动裁剪 | 呼叫/PIN 日志保留最近 90 天或 1 万条 |
| Presence 零存储 | 在线状态存于 DO 内存，掉线即消失 |

---

## 4. 公告板模块（F1）

### 4.1 可见性模型

- 三级可见性：`all`（全员）/ `group`（设备组）/ `device`（临时勾选任意设备）。
- 组为主、临时勾选为辅，两者可叠加（同一公告可同时挂组目标和设备目标）。
- **过滤只在服务端执行**：设备请求公告列表时 SQL 直接裁剪，固件拿不到无权数据。

### 4.2 推送：提示-拉取模式

1. 管理员发布公告 → API 计算目标设备列表。
2. 经 PresenceDO 向在线目标设备发轻量提示 `{type:"announce", id:123}`。
3. 设备收到后走 HTTPS 拉取正文；离线设备下次上线轮询补拉（带 `since` 游标）。

### 4.3 设备端 UI

- 公告列表按 priority / 时间排序；未读红点，已读标记存 NVS。
- 支持过期公告自动隐藏（`expires_at`）。

---

## 5. PTT 对讲系统（F2–F5）

### 5.1 统一抽象：房间

三种模式统一为「房间」，仅创建方式与准入控制不同：

| 模式 | 创建 | 准入 | 生命周期 |
|---|---|---|---|
| 1:1 私密呼叫 | 主叫选在线设备 → 邀请 → 被叫按键接听 | 双方确认（可选被叫保护 PIN） | 挂断即销毁 |
| 广播频道 | 管理员预建 | 组成员自动可见，可叠加 PIN | 持久 |
| 私密频道 | 任意设备创建并设 PIN | PIN 校验通过 | 持久或定时解散 |

### 5.2 RoomDO 职责

- 每个活跃房间一个 Durable Object 实例：WebSocket 汇聚、状态机、音频扇出。
- **话语权仲裁（Floor Control）**：PTT 按下 → `talk_request` → DO 授予/拒绝（同时仅一人持麦）→ 音频上行 → 扇出给其他成员 → 松开 `talk_release`。
- 空闲时启用 Hibernation：连接保持、停止计费。

### 5.3 1:1 呼叫时序

```
主叫设备              PresenceDO             被叫设备           RoomDO
   │  call_invite(B)     │                     │                │
   │────────────────────>│  call_invite        │                │
   │                     │────────────────────>│ 响铃提示        │
   │                     │  call_accept        │                │
   │                     │<────────────────────│ 按键接听        │
   │  room_join(roomId)  │   创建房间           │                │
   │<────────────────────│────────────────────>│───────────────>│
   │  talk_request/grant ──────────────────────────────────────>│
   │  音频帧上行 ──────────────────────────────────────────────>│──扇出──>│
```

拒接、忙线、超时（30s 未接听）均有对应信令与 UI 反馈。

### 5.4 PIN 安全设计（F4/F5）

| 项 | 设计 |
|---|---|
| 存储 | 仅存哈希：PBKDF2（WebCrypto，Workers 原生支持），每 PIN 独立 salt |
| 校验位置 | 全部服务端；设备只做输入 UI |
| 频道准入 | 校验通过后签发**该频道一次性会话票据**，设备不缓存 PIN |
| 防爆破 | 错 5 次锁 10 分钟（D1 记录），指数退避 |
| 审计 | 每次校验与呼叫入审计表（who/when/room） |

### 5.5 音频协议与成本控制（关键设计）

**问题**：DO 按 WebSocket 消息数计请求。20ms Opus 帧独立发送 → 双向扇出后约 6000 条/分钟通话 → 免费额度 10 万条/天仅支撑约 30 分钟。

**方案：设备端攒包**
- 每 10 个 20ms Opus 帧打包为一条 200ms WSS 二进制消息 → 5 条/秒。
- 200ms 缓冲延迟在 PTT 场景无感知。
- 额度估算：约 300 条/分钟/方向，免费额度支撑 5 小时以上/天通话。

**帧格式**

```
二进制音频消息：
┌──────────┬───────────┬───────────┬──────────────────┐
│ magic(1B)│ seq(2B)   │ ts_ms(4B) │ opus_frames[N]   │
│ 0xA1     │ 大端       │ 首帧时间戳  │ 每帧: len(1B)+data│
└──────────┴───────────┴───────────┴──────────────────┘

控制消息：JSON 文本帧（hello/auth, presence, call_*, talk_*, announce 提示等）
```

- 编码：Opus @ 16kHz 单声道，20ms/帧，目标码率 16–24kbps。
- 接收端：jitter buffer（建议 3–5 个消息深度），按 seq 排序、丢包 PLC。

---

## 6. 服务端设计

### 6.1 组件清单

| 组件 | 技术 | 职责 |
|---|---|---|
| 静态站点 | Cloudflare Pages | 管理后台 SPA |
| API | Pages Functions（Workers） | 公告 CRUD、组管理、设备认领/激活审批、PIN 设置、审计查询 |
| 实时层 | Durable Objects | PresenceDO（在线/信令路由）、RoomDO（房间/仲裁/扇出） |
| 数据库 | D1（SQLite） | 全部持久化数据 |
| 缓存 | KV（可选） | 会话票据、热点配置 |
| 定时任务 | Cron Triggers | 待认领过期清理、审计裁剪、过期公告处理 |

### 6.2 D1 数据模型（草案）

```sql
-- 设备（MAC 派生主键，幂等注册）
CREATE TABLE devices (
  device_id     TEXT PRIMARY KEY,      -- SHA-256(MAC)[:16]
  mac_hash      TEXT UNIQUE NOT NULL,
  owner_name    TEXT,
  label         TEXT,                  -- 显示名，如「前台的工牌」
  status        TEXT NOT NULL,         -- pending|active|suspended
  secret_hash   TEXT,                  -- device_secret 哈希
  call_pin_hash TEXT,                  -- 呼叫保护 PIN（可空）
  created_at    INTEGER, activated_at INTEGER, last_seen INTEGER
);

-- 设备组
CREATE TABLE groups (
  group_id TEXT PRIMARY KEY, name TEXT NOT NULL, created_at INTEGER
);
CREATE TABLE group_members (
  group_id TEXT, device_id TEXT, PRIMARY KEY (group_id, device_id)
);

-- 公告与可见性
CREATE TABLE announcements (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  title TEXT, body TEXT, priority INTEGER DEFAULT 0,
  created_by TEXT, created_at INTEGER, expires_at INTEGER
);
CREATE TABLE announcement_targets (
  announcement_id INTEGER,
  target_type TEXT,                    -- 'all'|'group'|'device'
  target_id   TEXT                     -- group_id 或 device_id，all 时为 NULL
);

-- 频道
CREATE TABLE channels (
  channel_id TEXT PRIMARY KEY, name TEXT,
  type TEXT,                           -- 'broadcast'|'private'
  pin_hash TEXT, created_by TEXT, created_at INTEGER, dissolved_at INTEGER
);
CREATE TABLE channel_members (
  channel_id TEXT, device_id TEXT, PRIMARY KEY (channel_id, device_id)
);

-- PIN 防爆破与审计
CREATE TABLE pin_failures (
  subject TEXT,                        -- device_id + 上下文
  fail_count INTEGER, locked_until INTEGER, PRIMARY KEY (subject)
);
CREATE TABLE audit_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts INTEGER, actor TEXT, action TEXT, detail TEXT
);
```

### 6.3 API 端点（草案）

| 方法 | 路径 | 说明 | 鉴权 |
|---|---|---|---|
| POST | /api/pairing-codes | 生成配对码 | 管理员 |
| POST | /api/devices/claim | 设备凭配对码认领 | 配对码 |
| POST | /api/devices/reactivate/approve | 审批重新激活 | 管理员 |
| GET | /api/announcements?since= | 设备拉取可见公告（游标增量） | 设备 |
| POST | /api/announcements | 发布公告（含 targets） | 管理员 |
| GET/POST/DELETE | /api/groups[...] | 组与成员管理 | 管理员 |
| POST | /api/channels | 建频道（可带 PIN） | 设备/管理员 |
| POST | /api/channels/:id/join | PIN 校验入频道，发票据 | 设备 |
| PUT | /api/devices/me/call-pin | 设置呼叫保护 PIN | 设备主人 |
| GET | /api/audit | 审计查询 | 管理员 |
| WSS | /ws/device | 设备实时通道（信令+音频） | 设备 |

### 6.4 WSS 控制信令（草案）

```jsonc
// 握手（HMAC: device_id + ts + nonce 签名）
{"type":"hello","device_id":"…","ts":…,"nonce":"…","sig":"…"}

// 在线通讯录
{"type":"presence_list","devices":[{"id":"…","label":"…"}]}

// 呼叫
{"type":"call_invite","to":"device_id"}
{"type":"call_incoming","from":"device_id","from_label":"…","pin_required":true}
{"type":"call_accept"} / {"type":"call_reject","reason":"busy"}
{"type":"room_join","room":"…","role":"caller"}

// 话语权
{"type":"talk_request"}  →  {"type":"talk_grant"} | {"type":"talk_deny","holder":"…"}
{"type":"talk_release"}  →  {"type":"floor_free"}

// 公告提示
{"type":"announce","id":123,"title":"…","priority":1}
```

---

## 7. 固件设计

### 7.1 模块划分

```
net_mgr     Wi-Fi 连接管理 + TLS + 断线重连（指数退避，上限 60s）
identity    eFuse/NVS 凭证读取、HMAC 签名、配对码/激活流程
proto       WSS 客户端：JSON 控制帧（cJSON）+ 二进制音频帧收发
audio       ES8311 → Opus 编码(16kHz/20ms) → 攒包(200ms) → 发送
            收包 → jitter buffer → Opus 解码 → 播放
ui          状态机：公告列表 / 在线通讯录 / 频道列表 / PIN 输入
            / 来电响铃 / 通话中（PTT 状态、持麦提示）
store       NVS：device_secret（L2）、未读标记、常用频道、游标
```

### 7.2 按键语义

| 操作 | 语义 |
|---|---|
| 长按 | PTT 说话（通话/频道内） |
| 短按 | 确认 / 接听 / 进入 |
| 双击 | 返回 / 拒接 |
| 组合键 | 进入配网/配对模式 |

### 7.3 资源预算注意

- 无 PSRAM：Opus 编码器 RAM 约 40–60KB，jitter buffer 与 TLS 缓冲需统一预算，建议 TLS 接收缓冲 ≤ 8KB。
- 音频任务与网络任务分核优先级绑定，避免 UI 刷新阻塞音频路径。

---

## 8. 安全设计汇总

| 域 | 措施 |
|---|---|
| 传输 | 全链路 TLS（HTTPS/WSS） |
| 设备认证 | HMAC 签名握手（device_id + ts + nonce），secret 不落服务端明文 |
| 公告可见性 | 服务端 SQL 过滤，固件零信任 |
| PIN | PBKDF2 哈希 + salt；错 5 锁 10 分钟；票据制，不缓存 |
| 身份冒用 | 重新激活需主人审批；MAC 不作为认证依据 |
| 滥用防护 | 注册/认领/PIN 接口 IP 限速；待认领 24h 过期 |
| 审计 | 呼叫、PIN 校验、管理操作全量入 audit_log，90 天滚动 |

---

## 9. 成本与额度估算（Cloudflare Free Plan）

| 资源 | 免费额度 | 本项目估算 | 结论 |
|---|---|---|---|
| Durable Objects 请求 | 100K 条/天 | 攒包后通话 5h+ ≈ 90K；日常信令 < 5K | 够用，攒包是前提 |
| DO 时长 | 13K GB-s/天 | 小内存实例 + Hibernation | 充裕 |
| D1 | 5GB / 500 万读/天 | 50 设备量级 < 1MB 级 | 极充裕 |
| Pages/Functions 请求 | 100K 次/天 | 轮询+管理 < 10K | 充裕 |
| Workers CPU | 10ms/请求 | 转发为主，无重计算 | 充裕 |

**风险红线**：若取消攒包设计，DO 请求量会放大 10 倍以上并击穿免费额度——攒包是必须项而非优化项。

---

## 10. 实施路线图

### 阶段一：管道打通 + 公告板 MVP（里程碑 M1）

**目标**：身份链路 + 公告全链路跑通。
- Pages 站点 + Functions + D1 schema 落地
- MAC 幂等注册、配对码认领、eFuse/NVS 凭证、重新激活审批
- 公告 CRUD + 三级可见性 + 提示-拉取
- 固件：net_mgr + identity + 公告拉取与列表 UI

**验收**：新设备配对 → 刷机 → 免配置恢复 → 按权限收到公告。重新激活走后台一键审批。

### 阶段二：Presence + 1:1 呼叫（里程碑 M2）

**目标**：设备间私密通话闭环。
- PresenceDO（在线状态、信令路由、通讯录）
- RoomDO（floor control、攒包音频扇出）
- 呼叫邀请/接听/拒接/超时流程、响铃 UI、PTT 状态机

**验收**：两台设备互呼，PTT 交替发言，端到端延迟 < 800ms；30 分钟连续通话无断链，DO 消息量符合攒包预算。

### 阶段三：频道与 PIN（里程碑 M3）

**目标**：广播频道、私密频道、呼叫保护 PIN、审计。
- 频道 CRUD、PIN 准入票据、防爆破锁定、audit_log

**验收**：PIN 频道错 5 次锁定；广播频道 5 设备同时在线抢麦仲裁正确；审计可回溯。

### 阶段四：管理后台完善 + 打磨（里程碑 M4）

- 后台：发公告（组+临时勾选）、组管理、激活审批、审计视图、设备解绑/重置
- 固件：断线恢复体验、UI 细节、未读红点
- 全链路压测与额度复盘

### 里程碑依赖

```
M1（身份+公告）→ M2（对讲核心）→ M3（频道/PIN）→ M4（后台+打磨）
```

M1 是地基：身份体系与公告权限同时是后续所有模块的前置。

---

## 11. 风险登记册

| 风险 | 等级 | 对策 |
|---|---|---|
| DO 免费额度被音频消息击穿 | 高 | 攒包 200ms 为强制设计；阶段二实测复盘消息量 |
| ESP32-C3 无 PSRAM 下 Opus+TLS 内存紧张 | 高 | 早期做内存预算验证；TLS 缓冲压缩；必要时降码率至 12kbps |
| eFuse 烧写提升配对门槛 | 中 | 提供一键配网烧录脚本；L2 NVS 为默认落点，eFuse 可选增强 |
| MAC 伪造冒用 | 中 | MAC 仅作身份锚点不作认证；重新激活强制主人审批 |
| WSS 长连接在弱网下频繁掉线 | 中 | 指数退避重连 + 会话快速恢复（免重握手）；通话状态 30s 可恢复 |
| Cloudflare 平台策略变化 | 低 | 架构抽象层（proto）隔离传输，可迁移至任意 WS 后端 |

---

## 12. 二期可选方向（本期不做）

- 网页/手机端通过 Cloudflare Calls（SFU/TURN）加入通话，云端桥接 WebSocket 音频 ↔ WebRTC。
- 公告附件（小图标/二维码）。
- 消息文字对讲（公告信道的轻量复用）。
- 多租户 SaaS 化（每主人独立命名空间）。

---

## 13. 仓库结构建议（Monorepo）

```
ai-passport-board/
├── web/            # 管理后台 SPA（Pages 静态站点）
├── worker/         # Pages Functions API + PresenceDO + RoomDO + Cron
│   └── migrations/ # D1 schema migrations
├── firmware/       # ESP-IDF 固件（fork 自 ai-passport，feature 分支开发）
│   └── main/       # net_mgr / identity / proto / audio / ui / store
└── docs/           # API 规范、WSS 协议规范、配对流程说明
```

---

*本文档为设计基准，进入实现阶段后，API 规范（§6.3）与 WSS 协议规范（§6.4、§5.5）应独立成文并版本化管理。*
