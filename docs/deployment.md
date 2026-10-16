# 部署指南（Cloudflare Pages · pages.dev）

> 为什么是 Pages：`workers.dev` 域名在国内无法正常访问，`pages.dev` 可达。静态后台 + Functions API 同源部署于 `https://whisport.pages.dev`；Durable Objects 由伴生 Worker `whisport-realtime` 承载（Pages 不能定义 DO 类），经内部服务绑定访问，无公网路由——全部用户流量走 pages.dev。

## 架构一览

```
浏览器 / 设备 ──HTTPS──▶ whisport.pages.dev
                         ├─ public/      静态管理后台
                         ├─ functions/   REST API（web/core 路由器）
                         │    └─ bindings: DB(D1) / CACHE(KV) / PRESENCE(DO→whisport-realtime)
whisport-realtime ────────┘  仅承载 PresenceDO（M2 加 RoomDO），workers_dev=false
清理任务：惰性触发（whoami 时检查 KV 水位，1h 节流）—— Pages 无 Cron Triggers
```

## 首次部署（约 10 分钟）

```bash
cd web
npx wrangler login

# 1. 创建资源（D1 / KV；创建后回填 wrangler.toml 的 database_id 与 id）
npx wrangler d1 create ai-passport-board
npx wrangler kv namespace create CACHE

# 2. 伴生 Worker（DO 宿主）—— 必须先于 Pages 部署
npx wrangler deploy --config ../realtime/wrangler.toml

# 3. Pages 项目 + 部署
npx wrangler pages project create whisport --production-branch main
npx wrangler pages deploy                     # → https://whisport.pages.dev

# 4. 生产密钥（wrangler.toml 中不得放同名 vars）
openssl rand -hex 24                          # ADMIN_TOKEN（管理后台令牌，记入密码管理器）
npx wrangler pages secret put ADMIN_TOKEN --project-name whisport
openssl rand -hex 32                          # SECRET_ENC_KEY（64 hex，加密 device_secret 用）
npx wrangler pages secret put SECRET_ENC_KEY --project-name whisport

# 5. 建表 + 线上冒烟
npx wrangler d1 migrations apply ai-passport-board --remote
ADMIN_TOKEN=<你的令牌> node scripts/smoke.mjs https://whisport.pages.dev
```

> ⚠️ `SECRET_ENC_KEY` 一旦设置不可更换（所有 device_secret 将无法解密）；换钥需全体设备重新认领。密钥存于仓库外安全处（本次开发已生成一份，位置见交接消息）。

## 本地开发

```bash
cd web
npm install
npx wrangler d1 migrations apply ai-passport-board --local
npx wrangler pages dev                       # http://127.0.0.1:8788（静态+functions 同源）
node scripts/smoke.mjs http://127.0.0.1:8788

npm test                                     # 19 项集成测试
npm run typecheck
```

本地密钥放 `web/.dev.vars`（参考 `.dev.vars.example`；已 gitignore）。

## 验收清单（M1）

- [x] 管理后台登录、生成配对码（https://whisport.pages.dev）
- [x] 设备凭码认领 → 密钥写入 NVS → 刷机后免配置恢复（同 MAC 幂等）
- [x] 公告三级可见性 + 游标增量拉取 + 过期隐藏
- [x] 重新激活：设备请求 → 后台批准 → 轮询一次性取回新密钥、旧密钥失效
- [x] 审计全量可查；惰性清理生效
- [x] 线上冒烟 9/9（2026-10-16，pages.dev）
- [x] 真机（ESP32-C3）联测 —— 配网页/菜单/认领/公告/重激活全链路（2026-10-17）
- [x] 设备直连 pages 后端（无代理）验证通过

## 设备端配置台（M1.5）

设备自带 SoftAP 配网页，无需串口/App：首次开机（NVS 无 Wi-Fi 凭据）自动开热点
`whisport-XXXX`（密码=开机随机 8 位，屏幕显示），手机浏览器访问 `http://192.168.4.1`
可扫描 Wi-Fi、保存密码、改设备名、输入配对码认领；已联网后同一页面在 `http://<设备IP>` 可用。
所有写操作需输入屏幕展示的 4 位 PIN。串口 `prov` 命令可清凭据重回配网。

## 部署拓扑备注

- **删除旧部署**：重构前的 Worker `ai-passport-board`（workers.dev）已删除；D1/KV 资源沿用。
- **DO 迁移**：PresenceDO 无持久状态，换宿主 Worker 无数据迁移需求。
- **Bot 防护**：若账号开了 Bot Fight Mode，匿名设备 POST 可能被挑战页拦截（冒烟脚本曾中招）；建议关闭或为 API 路径加 WAF 例外。设备端已带 User-Agent。
- **可选 Cron Worker**：若希望无管理访问时也清理，可另建极小 Worker 挂 Cron Trigger，定时 POST 一个受保护端点；当前惰性方案对玩具规模足够。
