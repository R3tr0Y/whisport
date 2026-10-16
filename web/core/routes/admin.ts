import type { Ctx } from "../router";
import { ApiError, json, nowSec, readJson } from "../util";
import { audit } from "../audit";
import { requireAdmin } from "../auth";
import { maybeCleanup } from "../cron";
import { randomInt, sha256hex } from "../crypto";

const PAIRING_CODE_TTL = 600; // 10 分钟

/** GET /api/admin/whoami — 校验管理令牌（顺带惰性清理） */
export async function whoami(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  void maybeCleanup(ctx.env); // 尽力而为，不阻塞响应
  return json({ ok: true });
}

/** GET /api/admin/devices — 设备列表（含组归属与待审批数） */
export async function listDevices(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const { results } = await ctx.env.DB.prepare(
    `SELECT d.device_id, d.label, d.owner_name, d.status, d.created_at, d.activated_at, d.last_seen,
       (SELECT group_concat(g.name, ' · ') FROM group_members m
          JOIN groups g ON g.group_id = m.group_id WHERE m.device_id = d.device_id) AS groups,
       (SELECT COUNT(*) FROM reactivation_requests r
          WHERE r.device_id = d.device_id AND r.status = 'pending') AS pending_reactivation,
       (SELECT MAX(u.status) FROM reactivation_requests u
          WHERE u.device_id = d.device_id AND u.status = 'approved') AS approved_reactivation
     FROM devices d ORDER BY d.created_at DESC`,
  ).all();
  return json({ items: results });
}

/** PATCH /api/admin/devices/:id — {label?, owner_name?, status?} */
export async function patchDevice(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = ctx.params.id;
  const b = await readJson<{ label?: string; owner_name?: string; status?: string }>(ctx.req);
  if (b.status !== undefined && b.status !== "active" && b.status !== "suspended") {
    throw new ApiError(400, "bad_request", "status must be active|suspended");
  }
  const label = typeof b.label === "string" ? b.label.slice(0, 64) : null;
  const owner = typeof b.owner_name === "string" ? b.owner_name.slice(0, 64) : null;
  const r = await ctx.env.DB.prepare(
    "UPDATE devices SET label = COALESCE(?, label), owner_name = COALESCE(?, owner_name), " +
      "status = COALESCE(?, status) WHERE device_id = ?",
  )
    .bind(label, owner, b.status ?? null, id)
    .run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "no such device");
  await audit(ctx.env, "admin", "device.update", { device_id: id, ...b });
  return json({ ok: true });
}

/** POST /api/admin/devices/:id/unbind — 解绑：清凭证回待认领（记录保留） */
export async function unbindDevice(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = ctx.params.id;
  const r = await ctx.env.DB.prepare(
    "UPDATE devices SET status = 'pending', secret_enc = NULL, owner_name = NULL, activated_at = NULL WHERE device_id = ?",
  )
    .bind(id)
    .run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "no such device");
  await audit(ctx.env, "admin", "device.unbind", { device_id: id });
  return json({ ok: true });
}

/** POST /api/admin/devices/:id/reset — 出厂重置：删除记录（连带成员/请求，审计先行） */
export async function resetDevice(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = ctx.params.id;
  await audit(ctx.env, "admin", "device.reset", { device_id: id });
  const r = await ctx.env.DB.prepare("DELETE FROM devices WHERE device_id = ?").bind(id).run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "no such device");
  return json({ ok: true });
}

/** POST /api/admin/pairing-codes {note?} — 生成 6 位一次性配对码，明文仅此一次返回 */
export async function createPairingCode(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const b = await readJson<{ note?: string }>(ctx.req).catch(() => ({}) as { note?: string });
  const code = String(randomInt(1000000)).padStart(6, "0");
  const now = nowSec();
  await ctx.env.DB.prepare(
    "INSERT INTO pairing_codes (code_hash, note, created_at, expires_at) VALUES (?, ?, ?, ?)",
  )
    .bind(await sha256hex(code), typeof b.note === "string" ? b.note.slice(0, 128) : null, now, now + PAIRING_CODE_TTL)
    .run();
  await audit(ctx.env, "admin", "pairing_code.create", { note: b.note ?? null });
  return json({ code, expires_at: now + PAIRING_CODE_TTL }, 201);
}

/** GET /api/admin/pairing-codes — 近期配对码（哈希打码） */
export async function listPairingCodes(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const { results } = await ctx.env.DB.prepare(
    `SELECT substr(code_hash, 1, 8) AS code_hint, note, created_at, expires_at, used_at,
       (SELECT COALESCE(d.label, substr(d.device_id, 1, 8)) FROM devices d WHERE d.device_id = p.used_by) AS used_by_label
     FROM pairing_codes p ORDER BY created_at DESC LIMIT 20`,
  ).all();
  return json({ items: results });
}

/** GET /api/admin/reactivations?status=pending */
export async function listReactivations(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const status = ctx.url.searchParams.get("status");
  const { results } = await ctx.env.DB.prepare(
    `SELECT r.id, r.device_id, r.status, r.created_at, r.expires_at, r.decided_at, r.consumed_at,
       COALESCE(d.label, substr(d.device_id, 1, 8)) AS device_label
     FROM reactivation_requests r LEFT JOIN devices d ON d.device_id = r.device_id
     ${status ? "WHERE r.status = ?" : ""}
     ORDER BY r.id DESC LIMIT 100`,
  )
    .bind(...(status ? [status] : []))
    .all();
  return json({ items: results });
}

/** POST /api/admin/reactivations/:id/approve — 批准（secret 在设备轮询取回时签发） */
export async function approveReactivation(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = Number(ctx.params.id);
  if (!Number.isInteger(id)) throw new ApiError(400, "bad_request", "invalid id");
  const r = await ctx.env.DB.prepare(
    "UPDATE reactivation_requests SET status = 'approved', decided_at = ? " +
      "WHERE id = ? AND status = 'pending' AND expires_at > ?",
  )
    .bind(nowSec(), id, nowSec())
    .run();
  if (r.meta.changes === 0) throw new ApiError(409, "not_approvable", "request not pending or expired");
  await audit(ctx.env, "admin", "device.reactivate.approve", { request_id: id });
  return json({ ok: true });
}

/** POST /api/admin/reactivations/:id/deny */
export async function denyReactivation(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = Number(ctx.params.id);
  if (!Number.isInteger(id)) throw new ApiError(400, "bad_request", "invalid id");
  const r = await ctx.env.DB.prepare(
    "UPDATE reactivation_requests SET status = 'denied', decided_at = ? WHERE id = ? AND status = 'pending'",
  )
    .bind(nowSec(), id)
    .run();
  if (r.meta.changes === 0) throw new ApiError(409, "not_deniable", "request not pending");
  await audit(ctx.env, "admin", "device.reactivate.deny", { request_id: id });
  return json({ ok: true });
}

/** GET /api/admin/audit?action=&actor=&limit=&before_id= */
export async function listAudit(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const action = ctx.url.searchParams.get("action");
  const actor = ctx.url.searchParams.get("actor");
  const beforeId = Number(ctx.url.searchParams.get("before_id") ?? "0");
  const limit = Math.min(Number(ctx.url.searchParams.get("limit") ?? "50"), 200);
  const conds: string[] = [];
  const binds: unknown[] = [];
  if (action) {
    conds.push("action LIKE ?");
    binds.push(`%${action}%`);
  }
  if (actor) {
    conds.push("actor LIKE ?");
    binds.push(`%${actor}%`);
  }
  if (Number.isInteger(beforeId) && beforeId > 0) {
    conds.push("id < ?");
    binds.push(beforeId);
  }
  const where = conds.length ? `WHERE ${conds.join(" AND ")}` : "";
  const { results } = await ctx.env.DB.prepare(
    `SELECT * FROM audit_log ${where} ORDER BY id DESC LIMIT ?`,
  )
    .bind(...binds, Number.isInteger(limit) && limit > 0 ? limit : 50)
    .all();
  return json({ items: results });
}
