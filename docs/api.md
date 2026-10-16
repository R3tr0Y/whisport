# REST API 规范（M1 实现 · v1）

基址：`https://<worker域名>`（本地 dev 为 `http://127.0.0.1:8787`）

**通用约定**

- 时间戳一律 unix epoch **秒**
- 响应为 JSON；错误统一为 `{"error":{"code":"…","message":"…"}}`
- 限速：匿名端点按 IP 限速（认领 10 次/时、重激活 POST 10 次/时 / GET 60 次/时）

## 鉴权

### 管理员

```
Authorization: Bearer <ADMIN_TOKEN>
```

`ADMIN_TOKEN` 为 Worker secret（见 docs/deployment.md）。

### 设备 HMAC

每个需要设备身份的请求携带四个头：

| 头 | 值 |
|---|---|
| `X-Device-Id` | 32 hex（SHA-256(规范化MAC) 前 16 字节） |
| `X-Timestamp` | 当前秒级时间戳（服务端允许 ±120s） |
| `X-Nonce` | ≥8 字节随机 hex，5 分钟内不可重放 |
| `X-Signature` | HMAC-SHA256 签名，小写 hex |

**规范化串**（与固件 `app_identity.c` 逐字节一致）：

```
v1\n{device_id}\n{ts}\n{nonce}\n{METHOD}\n{path}
```

`path` 为不含 query 的 URL 路径（如 `/api/announcements`）。签名密钥为认领时下发的 `device_secret`（64 hex）。

错误码：`missing_auth_headers` / `stale_timestamp` / `unknown_device` / `bad_signature` / `replayed_nonce` / `suspended`。

## 设备端点（匿名）

### POST /api/devices/claim

凭配对码认领（首次或重绑定，MAC 主键幂等，重认领=轮换密钥）。

```json
// 请求
{"pairing_code": "482913", "mac": "aabbccddeeff", "label": "前台的工牌"}
// 200 —— device_secret 仅此一次返回
{"device_id": "9f26…（32 hex）", "device_secret": "…（64 hex）"}
```

错误：`invalid_code`(404) / `device_suspended`(403) / `bad_mac`(400) / `rate_limited`(429)。

### POST /api/devices/reactivate

设备丢失全部本地凭证时匿名发起（body `{"mac":"…"}`）。响应 `status`：

- `unclaimed`：MAC 未认领过，需走配对码
- `pending`：已建请求，待管理员批准（含 `request_id`）
- `approved`：已批准，去 GET 取回

### GET /api/devices/reactivate?mac=…

轮询。`approved` 状态下首次轮询**签发并一次性返回**新密钥：

```json
{"status":"ok","device_id":"…","device_secret":"…（新，仅此一次；旧密钥同时失效）"}
```

其余：`pending` / `consumed` / `denied` / `expired` / `none` / `unclaimed`。

## 设备端点（HMAC）

### GET /api/devices/me

```json
{"device":{"device_id":"…","label":"…","owner_name":"…","status":"active","last_seen":…},
 "groups":[{"group_id":"g_…","name":"值班组"}],
 "max_announcement_id": 42}
```

`max_announcement_id`：当前可见公告的最大 id（首刷游标用）。

### PATCH /api/devices/me — `{"label?":"…","owner_name?":"…"}`

### GET /api/announcements?since=\<id\>

服务端按可见性 SQL 裁剪（全员/组/点名，过期自动隐藏），增量返回：

```json
{"items":[{"id":42,"title":"…","body":"…","priority":1,"created_at":…,"expires_at":null}],
 "cursor": 42}
```

游标 = 已见最大 announcement id；设备持久化于 NVS。

## 管理端点（Bearer）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | /api/admin/whoami | 令牌校验 |
| GET | /api/admin/devices | 设备列表（含组、待审批数） |
| PATCH | /api/admin/devices/:id | `{label?, owner_name?, status?}`（active/suspended） |
| POST | /api/admin/devices/:id/unbind | 解绑 → 待认领（记录保留） |
| POST | /api/admin/devices/:id/reset | 出厂重置（删除记录，级联清理） |
| POST | /api/admin/pairing-codes | `{note?}` → `{code:"482913", expires_at}` 明文仅一次 |
| GET | /api/admin/pairing-codes | 近期记录（哈希前缀） |
| GET | /api/admin/reactivations?status=pending | 审批队列 |
| POST | /api/admin/reactivations/:id/approve | 批准（密钥在设备轮询时签发） |
| POST | /api/admin/reactivations/:id/deny | 拒绝 |
| GET/POST | /api/admin/announcements | 列表 / 发布 |
| DELETE | /api/admin/announcements/:id | 删除 |
| GET/POST/DELETE | /api/admin/groups[/:id][/members/:deviceId] | 分组管理 |
| GET | /api/admin/audit?action=&actor=&limit=&before_id= | 审计查询 |

### 公告发布

```json
POST /api/admin/announcements
{"title":"…","body":"…","priority":0,"expires_at":1760000000,
 "targets":[{"type":"all"} | {"type":"group","id":"g_…"} | {"type":"device","id":"9f26…"}]}
```

`targets` 可混合 group+device（取并集）；发布后经 PresenceDO 向在线目标推送轻量提示。

## 审计动作清单（M1）

`pairing_code.create` · `device.claim` · `device.update` · `device.unbind` · `device.reset` · `device.reactivate.request/approve/deny/consume` · `announcement.create/delete` · `group.create/delete/member.add/member.remove`
