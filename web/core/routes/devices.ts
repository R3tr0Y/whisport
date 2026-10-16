import type { Ctx } from "../router";
import { ApiError, json, nowSec, normalizeMac, readJson, requireString } from "../util";
import { audit } from "../audit";
import { deviceIdFromMac, deviceAuth, getDevice, rateLimit } from "../auth";
import { encryptString, randomHex, sha256hex } from "../crypto";

const REACT_REQUEST_TTL = 86400; // 重新激活请求有效期 24h

/**
 * POST /api/devices/claim — 设备凭配对码认领（幂等：MAC 主键，重复认领=轮换 secret）
 * body: { pairing_code, mac, label? }
 */
export async function claim(ctx: Ctx): Promise<Response> {
  await rateLimit(ctx.env, ctx.req, "claim", 10, 3600);
  const body = await readJson<{ pairing_code?: string; mac?: string; label?: string }>(ctx.req);
  const code = requireString(body.pairing_code, "pairing_code", 8);
  const mac = normalizeMac(requireString(body.mac, "mac", 20));
  const deviceId = await deviceIdFromMac(mac);

  const codeHash = await sha256hex(code);
  const pc = await ctx.env.DB.prepare(
    "SELECT expires_at, used_at FROM pairing_codes WHERE code_hash = ?",
  )
    .bind(codeHash)
    .first<{ expires_at: number; used_at: number | null } | null>();
  if (!pc || pc.used_at || pc.expires_at < nowSec()) {
    throw new ApiError(404, "invalid_code", "pairing code invalid, used or expired");
  }

  const dev = await getDevice(ctx.env, deviceId);
  if (dev?.status === "suspended") {
    throw new ApiError(403, "device_suspended", "device is suspended, contact admin");
  }

  const secret = randomHex(32);
  const secretEnc = await encryptString(secret, ctx.env.SECRET_ENC_KEY);
  const label = typeof body.label === "string" ? body.label.slice(0, 64) : "";
  const now = nowSec();
  await ctx.env.DB.batch([
    ctx.env.DB.prepare(
      `INSERT INTO devices (device_id, label, status, secret_enc, created_at, activated_at)
       VALUES (?, ?, 'active', ?, ?, ?)
       ON CONFLICT(device_id) DO UPDATE SET
         secret_enc = excluded.secret_enc,
         status = 'active',
         activated_at = excluded.activated_at,
         label = COALESCE(NULLIF(excluded.label, ''), label)`,
    ).bind(deviceId, label, secretEnc, now, now),
    ctx.env.DB.prepare("UPDATE pairing_codes SET used_at = ?, used_by = ? WHERE code_hash = ?")
      .bind(now, deviceId, codeHash),
    ctx.env.DB.prepare(
      "INSERT INTO audit_log (ts, actor, action, detail) VALUES (?, ?, 'device.claim', ?)",
    ).bind(now, deviceId, JSON.stringify({ mac_prefix: mac.slice(0, 6), rebind: !!dev })),
  ]);
  return json({ device_id: deviceId, device_secret: secret });
}

/**
 * POST /api/devices/reactivate — 匿名发起重新激活请求（设备丢失全部本地凭证时）
 * body: { mac }
 */
export async function reactivateRequest(ctx: Ctx): Promise<Response> {
  const body = await readJson<{ mac?: string }>(ctx.req);
  const mac = normalizeMac(requireString(body.mac, "mac", 20));
  // 按 MAC 限速而非 IP：家用 NAT 下多设备共享公网 IP，按 IP 会互相挤兑额度
  await rateLimit(ctx.env, ctx.req, `reactivate:${mac}`, 30, 3600);
  const deviceId = await deviceIdFromMac(mac);
  const now = nowSec();

  let dev = await getDevice(ctx.env, deviceId);
  if (!dev) {
    // I8：未知 MAC 额外按 IP 限速，防止随机 MAC 灌库（清理是惰性的）
    await rateLimit(ctx.env, ctx.req, "reactivate-unknown", 20, 3600);
    // 未知 MAC：幂等创建待认领记录（24h 后由清理逻辑删除），提示走配对码流程
    await ctx.env.DB.prepare(
      "INSERT INTO devices (device_id, status, created_at) VALUES (?, 'pending', ?) " +
        "ON CONFLICT(device_id) DO NOTHING",
    )
      .bind(deviceId, now)
      .run();
    return json({ status: "unclaimed", message: "device unknown, ask admin for a pairing code" });
  }
  if (dev.status === "pending") {
    return json({ status: "unclaimed", message: "device not claimed yet, pairing code required" });
  }
  if (dev.status === "suspended") {
    throw new ApiError(403, "device_suspended", "device is suspended");
  }

  const open = await ctx.env.DB.prepare(
    "SELECT id, status, expires_at FROM reactivation_requests " +
      "WHERE device_id = ? AND status IN ('pending','approved') ORDER BY id DESC LIMIT 1",
  )
    .bind(deviceId)
    .first<{ id: number; status: string; expires_at: number } | null>();
  if (open && open.expires_at > now) {
    return json({ status: open.status, request_id: open.id });
  }

  const r = await ctx.env.DB.prepare(
    "INSERT INTO reactivation_requests (device_id, created_at, expires_at) VALUES (?, ?, ?)",
  )
    .bind(deviceId, now, now + REACT_REQUEST_TTL)
    .run();
  await audit(ctx.env, deviceId, "device.reactivate.request", { mac_prefix: mac.slice(0, 6) });
  return json({ status: "pending", request_id: r.meta.last_row_id });
}

/**
 * GET /api/devices/reactivate?mac= — 匿名轮询；审批通过后在此一次性取回新 secret
 */
export async function reactivatePoll(ctx: Ctx): Promise<Response> {
  const mac = normalizeMac(requireString(ctx.url.searchParams.get("mac") ?? "", "mac", 20));
  await rateLimit(ctx.env, ctx.req, `reactivate-poll:${mac}`, 60, 3600);
  const deviceId = await deviceIdFromMac(mac);
  const now = nowSec();

  const dev = await getDevice(ctx.env, deviceId);
  if (!dev || dev.status === "pending") return json({ status: "unclaimed" });
  if (dev.status === "suspended") throw new ApiError(403, "device_suspended", "device is suspended");

  const r = await ctx.env.DB.prepare(
    "SELECT id, status, expires_at FROM reactivation_requests WHERE device_id = ? ORDER BY id DESC LIMIT 1",
  )
    .bind(deviceId)
    .first<{ id: number; status: string; expires_at: number } | null>();
  if (!r) return json({ status: "none", message: "post /api/devices/reactivate first" });

  const windowOpen = r.expires_at > now;
  if (r.status === "pending") return json({ status: windowOpen ? "pending" : "expired" });
  if (r.status === "denied") return json({ status: "denied" });
  if (r.status === "consumed") return json({ status: "consumed" });
  if (r.status === "approved") {
    if (!windowOpen) return json({ status: "expired" });
    // 签发即消费：secret 只出现在这一次响应里
    const secret = randomHex(32);
    const secretEnc = await encryptString(secret, ctx.env.SECRET_ENC_KEY);
    await ctx.env.DB.batch([
      ctx.env.DB.prepare("UPDATE devices SET secret_enc = ?, status = 'active' WHERE device_id = ?")
        .bind(secretEnc, deviceId),
      ctx.env.DB.prepare(
        "UPDATE reactivation_requests SET status = 'consumed', consumed_at = ? WHERE id = ?",
      ).bind(now, r.id),
      ctx.env.DB.prepare(
        "INSERT INTO audit_log (ts, actor, action, detail) VALUES (?, ?, 'device.reactivate.consume', NULL)",
      ).bind(now, deviceId),
    ]);
    return json({ status: "ok", device_id: deviceId, device_secret: secret });
  }
  return json({ status: r.status });
}

/** GET /api/devices/me — 设备自身档案（HMAC） */
export async function me(ctx: Ctx): Promise<Response> {
  const { deviceId } = await deviceAuth(ctx.req, ctx.env);
  const dev = await ctx.env.DB.prepare(
    "SELECT device_id, label, owner_name, status, created_at, activated_at, last_seen FROM devices WHERE device_id = ?",
  )
    .bind(deviceId)
    .first();
  const groups = await ctx.env.DB.prepare(
    `SELECT g.group_id, g.name FROM group_members m JOIN groups g ON g.group_id = m.group_id
     WHERE m.device_id = ? ORDER BY g.name`,
  )
    .bind(deviceId)
    .all();
  const maxId = await ctx.env.DB.prepare(
    `SELECT MAX(a.id) AS max_id FROM announcements a WHERE (a.expires_at IS NULL OR a.expires_at > ?)
     AND EXISTS (
       SELECT 1 FROM announcement_targets t WHERE t.announcement_id = a.id
         AND (t.target_type = 'all'
           OR (t.target_type = 'device' AND t.target_id = ?)
           OR (t.target_type = 'group' AND t.target_id IN (SELECT group_id FROM group_members WHERE device_id = ?))))`,
  )
    .bind(nowSec(), deviceId, deviceId)
    .first<{ max_id: number | null } | null>();
  return json({
    device: dev,
    groups: groups.results,
    max_announcement_id: maxId?.max_id ?? 0,
  });
}

/** PATCH /api/devices/me — 设备自助改显示名（HMAC） */
export async function patchMe(ctx: Ctx): Promise<Response> {
  const { deviceId } = await deviceAuth(ctx.req, ctx.env);
  const body = await readJson<{ label?: string; owner_name?: string }>(ctx.req);
  const label = typeof body.label === "string" ? body.label.slice(0, 64) : undefined;
  const owner = typeof body.owner_name === "string" ? body.owner_name.slice(0, 64) : undefined;
  await ctx.env.DB.prepare(
    "UPDATE devices SET label = COALESCE(?, label), owner_name = COALESCE(?, owner_name) WHERE device_id = ?",
  )
    .bind(label ?? null, owner ?? null, deviceId)
    .run();
  return json({ ok: true });
}
