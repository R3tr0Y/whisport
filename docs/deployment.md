# 部署指南（Cloudflare Free Plan）

## 首次部署（约 10 分钟）

```bash
cd worker
npx wrangler login

# 1. 创建资源
npx wrangler d1 create ai-passport-board          # 回填 database_id 到 wrangler.jsonc
npx wrangler kv namespace create CACHE            # 回填 id 到 wrangler.jsonc

# 2. 设置生产密钥（secret 优先于 vars，绝不入库）
openssl rand -hex 32                              # 生成 SECRET_ENC_KEY
npx wrangler secret put ADMIN_TOKEN               # 管理后台令牌，自拟强随机值
npx wrangler secret put SECRET_ENC_KEY            # 粘贴上面生成的 64 hex

# 3. 建表 + 部署
npx wrangler d1 migrations apply ai-passport-board --remote
npx wrangler deploy
```

部署完成后：

- 管理后台：`https://ai-passport-board.<你的子域>.workers.dev/`
- 设备 API 基址：同域名（固件 menuconfig → Whisport → WS_SERVER_URL）

> ⚠️ `SECRET_ENC_KEY` 一旦设置不可更换（所有 device_secret 将无法解密）；换钥需全体设备重新认领。

## 本地开发

```bash
cd worker
npm install
npx wrangler d1 migrations apply ai-passport-board --local
npx wrangler dev                                  # http://127.0.0.1:8787
node scripts/smoke.mjs                            # M1 全链路冒烟（9 项）

npm test                                          # 19 项集成测试（vitest + workers 运行时）
npm run typecheck
```

本地默认管理令牌为 `dev-admin-token`（wrangler.jsonc `vars`，仅开发用）。

## 静态后台独立部署到 Pages（可选）

后台是零构建静态站点，也可不经 Worker 直接发布：

```bash
npx wrangler pages deploy web/public --project-name whisport-admin
```

注意：独立部署后后台与 API 跨域，需在 Worker 补 CORS 头（当前按同源部署设计，未开 CORS）。

## 验收清单（M1）

- [x] 管理后台登录、生成配对码
- [x] 设备凭码认领 → 密钥写入 NVS → 刷机后免配置恢复（同 MAC 幂等）
- [x] 公告三级可见性 + 游标增量拉取 + 过期隐藏
- [x] 重新激活：设备请求 → 后台批准 → 轮询一次性取回新密钥、旧密钥失效
- [x] 审计全量可查；Cron 清理生效
- [ ] 真机（ESP32-C3）联测 —— 固件骨架尚未编译（无本地 ESP-IDF 环境）
