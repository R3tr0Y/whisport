import type { Ctx } from "../router";
import { ApiError, json, nowSec, readJson, requireString } from "../util";
import { audit } from "../audit";
import { deviceAuth, requireAdmin } from "../auth";

interface Target {
  type: "all" | "group" | "device";
  id: string; // 'all' 时为空串
}

/** 可见性过滤条件：?1 = device_id，?2 = now（服务端 SQL 裁剪，固件零信任） */
const VIS =
  "(a.expires_at IS NULL OR a.expires_at > ?2) AND EXISTS (" +
  " SELECT 1 FROM announcement_targets t WHERE t.announcement_id = a.id" +
  "  AND (t.target_type = 'all'" +
  "   OR (t.target_type = 'device' AND t.target_id = ?1)" +
  "   OR (t.target_type = 'group' AND t.target_id IN" +
  "       (SELECT group_id FROM group_members WHERE device_id = ?1))))";

/** GET /api/announcements?since=<id> — 设备增量拉取可见公告（HMAC） */
export async function listForDevice(ctx: Ctx): Promise<Response> {
  const { deviceId } = await deviceAuth(ctx.req, ctx.env);
  const since = Number(ctx.url.searchParams.get("since") ?? "0");
  if (!Number.isInteger(since) || since < 0) {
    throw new ApiError(400, "bad_request", "since must be a non-negative integer");
  }
  const { results } = await ctx.env.DB.prepare(
    `SELECT a.id, a.title, a.body, a.priority, a.created_at, a.expires_at
     FROM announcements a WHERE a.id > ?3 AND ${VIS}
     ORDER BY a.id DESC LIMIT 50`,
  )
    .bind(deviceId, nowSec(), since)
    .all();
  const cursor = results.length
    ? Math.max(...results.map((r) => Number((r as { id: number }).id)))
    : since;
  return json({ items: results, cursor });
}

/** POST /api/admin/announcements — 发布（管理员），body: {title, body, priority?, expires_at?, targets} */
export async function adminCreate(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const b = await readJson<{
    title?: string;
    body?: string;
    priority?: number;
    expires_at?: number | null;
    targets?: Array<{ type?: string; id?: string }>;
  }>(ctx.req);

  const title = requireString(b.title, "title", 120);
  const body = requireString(b.body, "body", 8000);
  const priority = clampInt(b.priority ?? 0, -128, 127, "priority");
  const now = nowSec();
  let expiresAt: number | null = null;
  if (b.expires_at !== undefined && b.expires_at !== null) {
    if (!Number.isInteger(b.expires_at) || b.expires_at <= now) {
      throw new ApiError(400, "bad_request", "expires_at must be a future unix timestamp (seconds)");
    }
    expiresAt = b.expires_at;
  }
  if (!Array.isArray(b.targets) || b.targets.length === 0) {
    throw new ApiError(400, "bad_request", "targets must be a non-empty array");
  }

  // 校验并规范化目标
  const seen = new Set<string>();
  const targets: Target[] = [];
  for (const t of b.targets) {
    const type = t.type;
    if (type === "all") {
      targets.push({ type: "all", id: "" });
      continue;
    }
    if (type === "group" || type === "device") {
      const id = requireString(t.id, "targets[].id", 64);
      const table = type === "group" ? "groups" : "devices";
      const exists = await ctx.env.DB.prepare(`SELECT 1 FROM ${table} WHERE ${type === "group" ? "group_id" : "device_id"} = ?`)
        .bind(id)
        .first();
      if (!exists) throw new ApiError(400, "bad_target", `${type} '${id}' does not exist`);
      const key = `${type}:${id}`;
      if (!seen.has(key)) {
        seen.add(key);
        targets.push({ type, id });
      }
      continue;
    }
    throw new ApiError(400, "bad_target", "target.type must be all|group|device");
  }

  const r = await ctx.env.DB.prepare(
    "INSERT INTO announcements (title, body, priority, created_by, created_at, expires_at) VALUES (?, ?, ?, 'admin', ?, ?)",
  )
    .bind(title, body, priority, now, expiresAt)
    .run();
  const annId = r.meta.last_row_id;
  await ctx.env.DB.batch(
    targets.map((t) =>
      ctx.env.DB.prepare(
        "INSERT INTO announcement_targets (announcement_id, target_type, target_id) VALUES (?, ?, ?)",
      ).bind(annId, t.type, t.id),
    ),
  );
  await audit(ctx.env, "admin", "announcement.create", { id: annId, title, targets });

  // 提示-拉取：向在线目标设备推轻量提示（尽力而为，失败不影响发布）
  void notifyAnnounce(ctx.env, annId, title, priority, targets);

  return json({ id: annId }, 201);
}

/** GET /api/admin/announcements — 管理员全量列表 */
export async function adminList(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const { results } = await ctx.env.DB.prepare(
    `SELECT a.*,
       (SELECT group_concat(
          CASE t.target_type
            WHEN 'all' THEN '全员'
            WHEN 'group' THEN '组:' || COALESCE(g.name, t.target_id)
            ELSE '设备:' || COALESCE(d.label, substr(t.target_id, 1, 8)) END, ' · ')
        FROM announcement_targets t
        LEFT JOIN groups g ON t.target_type = 'group' AND g.group_id = t.target_id
        LEFT JOIN devices d ON t.target_type = 'device' AND d.device_id = t.target_id
        WHERE t.announcement_id = a.id) AS targets_text
     FROM announcements a ORDER BY a.id DESC LIMIT 100`,
  ).all();
  return json({ items: results });
}

/** DELETE /api/admin/announcements/:id */
export async function adminDelete(ctx: Ctx): Promise<Response> {
  requireAdmin(ctx.req, ctx.env);
  const id = Number(ctx.params.id);
  if (!Number.isInteger(id)) throw new ApiError(400, "bad_request", "invalid id");
  const r = await ctx.env.DB.prepare("DELETE FROM announcements WHERE id = ?").bind(id).run();
  if (r.meta.changes === 0) throw new ApiError(404, "not_found", "no such announcement");
  await audit(ctx.env, "admin", "announcement.delete", { id });
  return json({ ok: true });
}

async function notifyAnnounce(
  env: Ctx["env"],
  annId: number,
  title: string,
  priority: number,
  targets: Target[],
): Promise<void> {
  try {
    const ids = new Set<string>();
    for (const t of targets) {
      if (t.type === "all") {
        const { results } = await env.DB.prepare(
          "SELECT device_id FROM devices WHERE status = 'active'",
        ).all<{ device_id: string }>();
        for (const r of results) ids.add(r.device_id);
      } else if (t.type === "device") {
        ids.add(t.id);
      } else {
        const { results } = await env.DB.prepare(
          "SELECT device_id FROM group_members WHERE group_id = ?",
        )
          .bind(t.id)
          .all<{ device_id: string }>();
        for (const r of results) ids.add(r.device_id);
      }
    }
    if (ids.size === 0) return;
    const stub = env.PRESENCE.get(env.PRESENCE.idFromName("global"));
    await stub.fetch("https://presence/notify", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        type: "announce",
        deviceIds: [...ids],
        payload: { id: annId, title, priority },
      }),
    });
  } catch (e) {
    console.warn("presence notify failed (non-fatal):", e);
  }
}

function clampInt(v: unknown, min: number, max: number, field: string): number {
  const n = Number(v);
  if (!Number.isInteger(n) || n < min || n > max) {
    throw new ApiError(400, "bad_request", `${field} must be an integer in [${min}, ${max}]`);
  }
  return n;
}
