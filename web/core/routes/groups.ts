import type { Ctx } from "../router";
import { ApiError, json, nowSec, readJson, requireString } from "../util";
import { audit } from "../audit";
import { requireAdmin } from "../auth";
import { randomHex } from "../crypto";

/** POST /api/admin/groups {name} */
export async function create(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const b = await readJson<{ name?: string }>(ctx.req);
  const name = requireString(b.name, "name", 64);
  const id = `g_${randomHex(6)}`;
  try {
    await ctx.env.DB.prepare("INSERT INTO groups (group_id, name, created_at) VALUES (?, ?, ?)")
      .bind(id, name, nowSec())
      .run();
  } catch (e) {
    if ((e as Error).message.includes("UNIQUE")) {
      throw new ApiError(409, "duplicate", "group name already exists");
    }
    throw e;
  }
  await audit(ctx.env, "admin", "group.create", { group_id: id, name });
  return json({ group_id: id, name }, 201);
}

/** GET /api/admin/groups — 列表（含成员数与成员明细） */
export async function list(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const { results } = await ctx.env.DB.prepare(
    `SELECT g.group_id, g.name, g.created_at,
       (SELECT COUNT(*) FROM group_members m WHERE m.group_id = g.group_id) AS member_count
     FROM groups g ORDER BY g.created_at DESC`,
  ).all();
  const members = await ctx.env.DB.prepare(
    `SELECT m.group_id, m.device_id, COALESCE(d.label, substr(m.device_id, 1, 8)) AS label, d.status
     FROM group_members m LEFT JOIN devices d ON d.device_id = m.device_id ORDER BY m.added_at`,
  ).all();
  const byGroup = new Map<string, unknown[]>();
  for (const m of members.results) {
    const row = m as { group_id: string };
    const arr = byGroup.get(row.group_id) ?? [];
    arr.push(m);
    byGroup.set(row.group_id, arr);
  }
  return json({
    items: results.map((g) => ({ ...(g as object), members: byGroup.get((g as { group_id: string }).group_id) ?? [] })),
  });
}

/** DELETE /api/admin/groups/:id */
export async function remove(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = requireString(ctx.params.id, "id", 64);
  const r = await ctx.env.DB.prepare("DELETE FROM groups WHERE group_id = ?").bind(id).run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "no such group");
  await audit(ctx.env, "admin", "group.delete", { group_id: id });
  return json({ ok: true });
}

/** POST /api/admin/groups/:id/members {device_id} */
export async function addMember(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const groupId = requireString(ctx.params.id, "id", 64);
  const b = await readJson<{ device_id?: string }>(ctx.req);
  const deviceId = requireString(b.device_id, "device_id", 64);
  const dev = await ctx.env.DB.prepare("SELECT device_id FROM devices WHERE device_id = ?")
    .bind(deviceId)
    .first();
  if (!dev) throw new ApiError(400, "bad_target", "device does not exist");
  try {
    await ctx.env.DB.prepare(
      "INSERT INTO group_members (group_id, device_id, added_at) VALUES (?, ?, ?)",
    )
      .bind(groupId, deviceId, nowSec())
      .run();
  } catch (e) {
    if ((e as Error).message.includes("FOREIGN KEY")) {
      throw new ApiError(404, "not_found", "no such group");
    }
    if ((e as Error).message.includes("UNIQUE")) return json({ ok: true });
    throw e;
  }
  await audit(ctx.env, "admin", "group.member.add", { group_id: groupId, device_id: deviceId });
  return json({ ok: true }, 201);
}

/** DELETE /api/admin/groups/:id/members/:deviceId */
export async function removeMember(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const groupId = requireString(ctx.params.id, "id", 64);
  const deviceId = requireString(ctx.params.deviceId, "deviceId", 64);
  const r = await ctx.env.DB.prepare(
    "DELETE FROM group_members WHERE group_id = ? AND device_id = ?",
  )
    .bind(groupId, deviceId)
    .run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "not a member");
  await audit(ctx.env, "admin", "group.member.remove", { group_id: groupId, device_id: deviceId });
  return json({ ok: true });
}
