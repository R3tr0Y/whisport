# Whisport — AI Passport 公告板 + PTT 对讲系统

[FoloToy AI Passport](https://folotoy.com)（ESP32-C3 智能工牌）的公告板与个人语音对讲应用。后端 100% Cloudflare Free Plan（Worker + D1 + KV + Durable Objects + Cron），无 VPS。

设计总纲：[AI-Passport公告板对讲项目规划.md](./AI-Passport公告板对讲项目规划.md)（基准文档）
实现偏差与工程决策：[docs/decisions.md](./docs/decisions.md)

## 仓库结构

```
worker/     Cloudflare Worker：REST API + PresenceDO + Cron + 管理后台托管（19 项集成测试）
web/        管理后台 SPA（零构建原生 ES 模块，随 Worker 同源部署）
firmware/   ESP-IDF 固件（M1 骨架：身份链路 + 公告拉取；音频对讲属 M2）
docs/       API 规范 · WSS 协议 · 部署指南 · 决策记录
```

## 当前状态（2026-10）

| 里程碑 | 内容 | 状态 |
|---|---|---|
| **M1** | 身份链路（MAC 幂等注册/配对码认领/刷机恢复/重新激活审批）+ 公告板（三级可见性/提示-拉取/游标）+ 管理后台 | ✅ 后端与后台完成并测试；固件骨架完成（待真机编译联调） |
| M2 | Presence 信令 + RoomDO + 1:1 呼叫 + PTT 语音 | 规划定稿（docs/protocol.md），未开工 |
| M3 | 频道 / PIN 准入 / 防爆破（表结构已就位） | 未开工 |
| M4 | 后台打磨 + 全链路压测 | 未开工 |

## 快速开始

```bash
# 后端（本地）
cd worker && npm install
npx wrangler d1 migrations apply ai-passport-board --local
npx wrangler dev                       # 管理后台 + API → http://127.0.0.1:8787
node scripts/smoke.mjs                 # 全链路冒烟
npm test                               # 19 项集成测试

# 生产部署
# 见 docs/deployment.md（D1/KV 创建、secrets、migrations、deploy）

# 固件
cd firmware && idf.py set-target esp32c3 && idf.py menuconfig && idf.py build flash monitor
# 控制台：pair <配对码> / sync / status
```

## 核心机制速览

- **设备身份**：`device_id = SHA-256(MAC)[:16]`，eFuse MAC 刷机不变 → 零配置恢复；密钥 AES-GCM 加密存库，HMAC 签名握手（±120s + nonce 防重放）
- **公告可见性**：`all`/`group`/`device` 三级，服务端 SQL 裁剪，固件零信任；在线设备走 WSS 轻提示 + HTTPS 拉正文
- **重新激活**：丢密钥设备匿名报 MAC → 主人后台一键批准 → 设备轮询一次性取回新密钥（防拾获冒用）
- **额度生存**（M2 关键）：PTT 音频强制 200ms 攒包，DO 消息量控制在免费额度内（规划 §5.5）
