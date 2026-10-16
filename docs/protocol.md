# WSS 协议规范（M1 已实现子集 + M2 规划）

端点：`wss://<worker域名>/ws/device`（由 PresenceDO 承载，全局单实例）

控制消息 = JSON 文本帧；音频 = 二进制帧（M2）。

## 握手（M1 已实现）

```jsonc
// 设备 → 服务端（HMAC：v1\n{device_id}\n{ts}\n{nonce}\nws）
{"type":"hello","device_id":"…","ts":1760000000,"nonce":"…","sig":"…","label":"前台的工牌"}

// 成功
{"type":"hello_ok","device_id":"…"}
// 失败（随后服务端 close 1008）
{"type":"hello_fail","reason":"bad signature"}
```

要求：ts ±120s；设备 status=active 且持有有效密钥。

## M1 已实现消息

| 方向 | 消息 | 说明 |
|---|---|---|
| 设备→服务端 | `{"type":"ping"}` | 应用层心跳（响应 `pong`） |
| 设备→服务端 | `{"type":"presence_list"}` | 请求在线通讯录 |
| 服务端→设备 | `{"type":"presence_list","devices":[{"id":"…","label":"…"}]}` | 在线设备（去重） |
| 服务端→设备 | `{"type":"announce","id":42,"title":"…","priority":1}` | 公告提示 → 设备转 HTTPS 拉取 |

未认证即发非 hello 消息 → `{"type":"error","reason":"unauthenticated"}` + close 1008。

## M2 规划（设计定稿，见根目录规划 §5/§6.4）

**信令**

```jsonc
{"type":"call_invite","to":"device_id"}
{"type":"call_incoming","from":"…","from_label":"…","pin_required":true}
{"type":"call_accept"} / {"type":"call_reject","reason":"busy|decline|timeout"}
{"type":"room_join","room":"…","role":"caller|callee"}
{"type":"talk_request"}  → {"type":"talk_grant"} | {"type":"talk_deny","holder":"…"}
{"type":"talk_release"}  → {"type":"floor_free"}
```

**音频二进制帧（强制攒包，200ms/条，规划 §5.5）**

```
┌──────────┬───────────┬───────────┬──────────────────┐
│ magic 1B │ seq 2B BE │ ts_ms 4B  │ opus_frames[N]   │
│ 0xA1     │           │ 首帧时间戳 │ 每帧 len(1B)+data │
└──────────┴───────────┴───────────┴──────────────────┘
```

- Opus 16kHz 单声道 20ms/帧，16–24kbps；每条消息 10 帧
- 接收端 jitter buffer 3–5 条深度，按 seq 排序 + PLC
- **攒包是额度生存前提**：不攒包 DO 请求量放大 10 倍击穿免费额度

**RoomDO**：每个活跃通话/频道一个实例；话语权仲裁（同时仅一人持麦）；空闲 Hibernation。
