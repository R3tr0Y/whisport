import type { Env } from "./env";
import { nowSec } from "./util";

export interface CleanupStats {
  expiredPendingDevices: number;
  oldPairingCodes: number;
  expiredReactivations: number;
  trimmedAuditRows: number;
}

const DAY = 86400;

/**
 * Cron 清理（每小时）：
 * - 待认领设备 24h 过期清除（防孤儿记录堆积）
 * - 已用/过期配对码 1h 后清除
 * - 重新激活请求超窗标记 expired（保留审计轨迹）
 * - 审计滚动裁剪：90 天 或 最近 1 万条
 */
export async function runCleanup(env: Env, now = nowSec()): Promise<CleanupStats> {
  const stats: CleanupStats = {
    expiredPendingDevices: 0,
    oldPairingCodes: 0,
    expiredReactivations: 0,
    trimmedAuditRows: 0,
  };

  let r = await env.DB.prepare(
    "DELETE FROM devices WHERE status = 'pending' AND created_at < ?",
  )
    .bind(now - DAY)
    .run();
  stats.expiredPendingDevices = r.meta.changes;

  r = await env.DB.prepare(
    "DELETE FROM pairing_codes WHERE expires_at < ? OR (used_at IS NOT NULL AND used_at < ?)",
  )
    .bind(now - 3600, now - 3600)
    .run();
  stats.oldPairingCodes = r.meta.changes;

  r = await env.DB.prepare(
    "UPDATE reactivation_requests SET status = 'expired' WHERE status IN ('pending','approved') AND expires_at < ?",
  )
    .bind(now)
    .run();
  stats.expiredReactivations = r.meta.changes;

  r = await env.DB.prepare(
    "DELETE FROM audit_log WHERE ts < ? OR id <= (SELECT COALESCE(MAX(id), 0) - 10000 FROM audit_log)",
  )
    .bind(now - 90 * DAY)
    .run();
  stats.trimmedAuditRows = r.meta.changes;

  return stats;
}
