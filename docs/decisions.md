# 实现决策记录（相对规划文档的偏差与补充）

规划文档（`AI-Passport公告板对讲项目规划.md`）是设计基准；以下为落地时的工程决策及理由。改规划前先读这里。

| # | 决策 | 理由 | 影响 |
|---|---|---|---|
| D1 | **统一 Worker 部署**（静态资源走 Worker assets），而非规划 §6.1 的 Pages Functions | Pages 不支持 Cron Triggers；DO + Cron + API + 静态站合一个部署单元，本地测试（vitest-pool-workers）也顺理成章。web/public 仍可独立发布 Pages（零构建） | 部署形态；功能等价 |
| D2 | `devices.secret_enc`（AES-256-GCM 加密存储）替代草案的 `secret_hash` | HMAC 签名握手要求服务端能验证 HMAC——哈希无法做到。加密存储满足规划 §8「secret 不落服务端**明文**」的原文约束 | 若 DB 泄漏且密钥未泄漏，secret 仍安全；换 SECRET_ENC_KEY 需全体重认领 |
| D3 | 去掉草案中的 `mac_hash` 列 | 规划 §3.2 规定主键即 MAC 派生值，`mac_hash` 与 `device_id` 完全重复 | 无 |
| D4 | `announcement_targets.target_id` 用 `''` 哨兵而非 NULL | SQLite rowid 表主键列含 NULL 不去重，可能插入重复 'all' 行 | 无 |
| D5 | 重新激活的密钥在**设备轮询取回时**生成，批准动作只改状态 | 避免明文 secret 落表；取回即消费、一次性 | 批准后设备 24h 内不轮询则请求过期，需重新发起 |
| D6 | 认领（claim）允许对已 active 设备重复执行（凭新配对码） | 这是「用户手里有设备但丢了密钥」最顺滑的恢复路径，比重新激活审批更轻 | 配对码即授权，与规划安全模型一致 |
| D7 | 公告游标 = announcement 自增 id（非时间戳） | 单调、无时钟偏移问题；`max_announcement_id` 由 /me 提供 | 公告 M1 不支持编辑（删除靠重新拉全量或过期） |
| D8 | `last_seen` 写入按 60s 节流 | 避免每次轮询都写 D1 | 在线时间精度 1 分钟 |
| D9 | 设备限速/防重放用 KV 尽力而为（最终一致），不可用时放行 | 玩具规模下可用性优先；KV 故障不应打断正常设备 | 极小概率重复计数或漏放行 |
| D10 | 固件 `storage` 独立 NVS 分区存 L2 凭证，缺失时自动回退默认 nvs 分区 | 与第三方分区表兼容；正式分区表见 partitions.csv | 无 |

## 测试基建备注

- vitest-pool-workers 需 `poolOptions.workers.wrangler.configPath`（顶层 `wrangler` 键会被静默剥离）
- workerd 的 D1 `exec()` 对注释/多语句支持差 → setup 自带小型 SQL 切分器按语句 batch
- PresenceDO（SQLite DO）与 isolatedStorage 有已知 `.sqlite-shm` 冲突 → 测试关闭按用例隔离（文件间仍隔离）
