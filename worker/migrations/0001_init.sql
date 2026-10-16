-- AI Passport Board · D1 schema v1（M1 基线，含 M2/M3 预留表）
-- 约定：所有时间戳均为 unix epoch 秒。

CREATE TABLE devices (
  device_id     TEXT PRIMARY KEY,           -- sha256(规范化MAC) 前 16 字节 hex（32 字符）
  label         TEXT,                       -- 显示名，如「前台的工牌」
  owner_name    TEXT,
  status        TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','active','suspended')),
  secret_enc    TEXT,                       -- AES-256-GCM 加密后的 device_secret（见 docs/decisions.md），认领前为 NULL
  call_pin_hash TEXT,                       -- M3：「pbkdf2$iter$salt_b64$hash_b64」
  created_at    INTEGER NOT NULL,
  activated_at  INTEGER,
  last_seen     INTEGER
);

CREATE TABLE pairing_codes (
  code_hash  TEXT PRIMARY KEY,              -- sha256(6 位配对码)
  note       TEXT,                          -- 管理员备注
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,              -- 10 分钟有效
  used_at    INTEGER,
  used_by    TEXT REFERENCES devices(device_id) ON DELETE SET NULL
);
CREATE INDEX idx_pairing_codes_expires ON pairing_codes(expires_at);

CREATE TABLE reactivation_requests (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id   TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  status      TEXT NOT NULL DEFAULT 'pending'
              CHECK (status IN ('pending','approved','consumed','denied','expired')),
  created_at  INTEGER NOT NULL,
  expires_at  INTEGER NOT NULL,             -- 请求有效期 24h
  decided_at  INTEGER,
  consumed_at INTEGER
);
CREATE INDEX idx_react_req_device ON reactivation_requests(device_id, status);

CREATE TABLE groups (
  group_id   TEXT PRIMARY KEY,
  name       TEXT NOT NULL UNIQUE,
  created_at INTEGER NOT NULL
);

CREATE TABLE group_members (
  group_id  TEXT NOT NULL REFERENCES groups(group_id) ON DELETE CASCADE,
  device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  added_at  INTEGER NOT NULL,
  PRIMARY KEY (group_id, device_id)
);
CREATE INDEX idx_group_members_device ON group_members(device_id);

CREATE TABLE announcements (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  title      TEXT NOT NULL,
  body       TEXT NOT NULL,
  priority   INTEGER NOT NULL DEFAULT 0,
  created_by TEXT NOT NULL,                 -- 'admin'
  created_at INTEGER NOT NULL,
  expires_at INTEGER                        -- NULL = 永不过期
);
CREATE INDEX idx_announcements_created ON announcements(created_at);

CREATE TABLE announcement_targets (
  announcement_id INTEGER NOT NULL REFERENCES announcements(id) ON DELETE CASCADE,
  target_type     TEXT NOT NULL CHECK (target_type IN ('all','group','device')),
  target_id       TEXT NOT NULL DEFAULT '', -- group_id / device_id；'all' 时为 ''
  PRIMARY KEY (announcement_id, target_type, target_id)
);
CREATE INDEX idx_ann_targets_lookup ON announcement_targets(target_type, target_id);

CREATE TABLE channels (
  channel_id   TEXT PRIMARY KEY,
  name         TEXT NOT NULL,
  type         TEXT NOT NULL CHECK (type IN ('broadcast','private')),
  pin_hash     TEXT,
  created_by   TEXT NOT NULL,
  created_at   INTEGER NOT NULL,
  dissolved_at INTEGER
);

CREATE TABLE channel_members (
  channel_id TEXT NOT NULL REFERENCES channels(channel_id) ON DELETE CASCADE,
  device_id  TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
  joined_at  INTEGER NOT NULL,
  PRIMARY KEY (channel_id, device_id)
);

CREATE TABLE pin_failures (
  subject      TEXT PRIMARY KEY,            -- 如 'channel:<id>' / 'device:<id>'
  fail_count   INTEGER NOT NULL DEFAULT 0,
  locked_until INTEGER,
  updated_at   INTEGER NOT NULL
);

CREATE TABLE audit_log (
  id     INTEGER PRIMARY KEY AUTOINCREMENT,
  ts     INTEGER NOT NULL,
  actor  TEXT NOT NULL,                     -- 'admin' / device_id / 'ip:1.2.3.4'
  action TEXT NOT NULL,
  detail TEXT                               -- JSON 字符串
);
CREATE INDEX idx_audit_ts ON audit_log(ts);
CREATE INDEX idx_audit_action ON audit_log(action);
