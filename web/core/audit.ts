import type { Env } from "./env";
import { nowSec } from "./util";

/** 审计日志：actor = 'admin' | device_id | 'ip:x.x.x.x'，detail 为 JSON 字符串 */
export async function audit(
  env: Env,
  actor: string,
  action: string,
  detail?: Record<string, unknown>,
): Promise<void> {
  await env.DB.prepare("INSERT INTO audit_log (ts, actor, action, detail) VALUES (?, ?, ?, ?)")
    .bind(nowSec(), actor, action, detail ? JSON.stringify(detail) : null)
    .run();
}
