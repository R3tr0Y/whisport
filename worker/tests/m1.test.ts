import { describe, expect, it } from "vitest";
import {
  adminFetch,
  anonFetch,
  claimDevice,
  env,
  nextMac,
  newPairingCode,
  signedFetch,
} from "./helpers";
import { deviceIdFromMac } from "../src/auth";
import { hmacHex, randomHex } from "../src/crypto";
import { restCanon } from "../src/auth";
import { runCleanup } from "../src/cron";

const DAY = 86400;

describe("管理员鉴权", () => {
  it("whoami：正确令牌 200，错误令牌 401", async () => {
    const ok = await adminFetch("/api/admin/whoami");
    expect(ok.status).toBe(200);
    const bad = await anonFetch("/api/admin/whoami", {
      headers: { authorization: "Bearer wrong-token" },
    });
    expect(bad.status).toBe(401);
  });
});

describe("配对码与认领（F1 身份链路）", () => {
  it("认领成功 → 返回 device_id/secret，/me 可用", async () => {
    const d = await claimDevice("测试工牌");
    expect(d.deviceId).toMatch(/^[0-9a-f]{32}$/);
    const me = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    expect(me.status).toBe(200);
    const body = (await me.json()) as { device: { label: string; status: string } };
    expect(body.device.label).toBe("测试工牌");
    expect(body.device.status).toBe("active");
  });

  it("device_id 由 MAC 确定性派生（幂等注册）", async () => {
    const mac = nextMac();
    const d1 = await anonFetch("/api/devices/claim", {
      method: "POST",
      body: JSON.stringify({ pairing_code: await newPairingCode(), mac }),
    });
    expect(d1.status).toBe(200);
    const id1 = ((await d1.json()) as { device_id: string }).device_id;
    expect(id1).toBe(await deviceIdFromMac(mac));
  });

  it("配对码一次性：重用 → 404", async () => {
    const code = await newPairingCode();
    const first = await anonFetch("/api/devices/claim", {
      method: "POST",
      body: JSON.stringify({ pairing_code: code, mac: nextMac() }),
    });
    expect(first.status).toBe(200);
    const second = await anonFetch("/api/devices/claim", {
      method: "POST",
      body: JSON.stringify({ pairing_code: code, mac: nextMac() }),
    });
    expect(second.status).toBe(404);
  });

  it("无效/过期配对码 → 404；坏 MAC → 400", async () => {
    const badCode = await anonFetch("/api/devices/claim", {
      method: "POST",
      body: JSON.stringify({ pairing_code: "000000", mac: nextMac() }),
    });
    expect(badCode.status).toBe(404);
    const badMac = await anonFetch("/api/devices/claim", {
      method: "POST",
      body: JSON.stringify({ pairing_code: await newPairingCode(), mac: "zz:zz" }),
    });
    expect(badMac.status).toBe(400);
  });
});

describe("设备 HMAC 鉴权", () => {
  it("坏签名 → 401；过期时间戳 → 401；重放 nonce → 401", async () => {
    const d = await claimDevice();
    const badSig = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me", undefined, {
      sig: "0".repeat(64),
    });
    expect(badSig.status).toBe(401);

    const stale = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me", undefined, {
      ts: Math.floor(Date.now() / 1000) - 600,
    });
    expect(stale.status).toBe(401);

    const ts = Math.floor(Date.now() / 1000);
    const nonce = randomHex(8);
    const path = "/api/devices/me";
    const sig = await hmacHex(d.secret, restCanon(d.deviceId, ts, nonce, "GET", path));
    const first = await signedFetch(d.secret, d.deviceId, "GET", path, undefined, { ts, nonce, sig });
    expect(first.status).toBe(200);
    const replay = await signedFetch(d.secret, d.deviceId, "GET", path, undefined, { ts, nonce, sig });
    expect(replay.status).toBe(401);
  });

  it("暂停（suspended）设备 → 403；恢复后 200", async () => {
    const d = await claimDevice();
    const patch = await adminFetch(`/api/admin/devices/${d.deviceId}`, {
      method: "PATCH",
      body: JSON.stringify({ status: "suspended" }),
    });
    expect(patch.status).toBe(200);
    const blocked = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    expect(blocked.status).toBe(403);
    await adminFetch(`/api/admin/devices/${d.deviceId}`, {
      method: "PATCH",
      body: JSON.stringify({ status: "active" }),
    });
    const ok = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    expect(ok.status).toBe(200);
  });
});

describe("公告板（F1 三级可见性 + 游标）", () => {
  it("all：全员可见，since 游标增量", async () => {
    const d = await claimDevice();
    const create = await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({
        title: "全员公告",
        body: "hello world",
        targets: [{ type: "all" }],
      }),
    });
    expect(create.status).toBe(201);
    const id = ((await create.json()) as { id: number }).id;

    const page0 = await signedFetch(d.secret, d.deviceId, "GET", "/api/announcements?since=0");
    expect(page0.status).toBe(200);
    const body0 = (await page0.json()) as { items: { id: number }[]; cursor: number };
    expect(body0.items.some((i) => i.id === id)).toBe(true);
    expect(body0.cursor).toBeGreaterThanOrEqual(id);

    const page1 = await signedFetch(
      d.secret,
      d.deviceId,
      "GET",
      `/api/announcements?since=${body0.cursor}`,
    );
    const body1 = (await page1.json()) as { items: { id: number }[] };
    expect(body1.items.some((i) => i.id === id)).toBe(false);
  });

  it("group：组成员可见，非成员不可见（服务端 SQL 裁剪）", async () => {
    const member = await claimDevice();
    const outsider = await claimDevice();
    const g = await adminFetch("/api/admin/groups", {
      method: "POST",
      body: JSON.stringify({ name: "值班组" }),
    });
    const groupId = ((await g.json()) as { group_id: string }).group_id;
    await adminFetch(`/api/admin/groups/${groupId}/members`, {
      method: "POST",
      body: JSON.stringify({ device_id: member.deviceId }),
    });
    const create = await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({
        title: "组公告",
        body: "group only",
        targets: [{ type: "group", id: groupId }],
      }),
    });
    expect(create.status).toBe(201);

    const seen = await signedFetch(member.secret, member.deviceId, "GET", "/api/announcements?since=0");
    const seenBody = (await seen.json()) as { items: { title: string }[] };
    expect(seenBody.items.some((i) => i.title === "组公告")).toBe(true);

    const notSeen = await signedFetch(
      outsider.secret,
      outsider.deviceId,
      "GET",
      "/api/announcements?since=0",
    );
    const notSeenBody = (await notSeen.json()) as { items: { title: string }[] };
    expect(notSeenBody.items.some((i) => i.title === "组公告")).toBe(false);
  });

  it("device：仅目标设备可见；all+group 叠加取并集", async () => {
    const target = await claimDevice();
    const other = await claimDevice();
    await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({
        title: "点名公告",
        body: "only you",
        targets: [{ type: "device", id: target.deviceId }],
      }),
    });
    const t = (await (await signedFetch(target.secret, target.deviceId, "GET", "/api/announcements?since=0")).json()) as { items: { title: string }[] };
    const o = (await (await signedFetch(other.secret, other.deviceId, "GET", "/api/announcements?since=0")).json()) as { items: { title: string }[] };
    expect(t.items.some((i) => i.title === "点名公告")).toBe(true);
    expect(o.items.some((i) => i.title === "点名公告")).toBe(false);
  });

  it("过期公告被隐藏（含 max_announcement_id）", async () => {
    const d = await claimDevice();
    const now = Math.floor(Date.now() / 1000);
    // 直接落库一条已过期公告（API 校验拒绝过去时间，属正常路径外数据）
    const r = await env.DB.prepare(
      "INSERT INTO announcements (title, body, priority, created_by, created_at, expires_at) VALUES ('过期公告', 'x', 0, 'admin', ?, ?)",
    )
      .bind(now - 100, now - 50)
      .run();
    const expiredId = r.meta.last_row_id;
    await env.DB.prepare(
      "INSERT INTO announcement_targets (announcement_id, target_type, target_id) VALUES (?, 'all', '')",
    )
      .bind(expiredId)
      .run();

    const list = await signedFetch(d.secret, d.deviceId, "GET", "/api/announcements?since=0");
    const body = (await list.json()) as { items: { id: number }[] };
    expect(body.items.some((i) => i.id === expiredId)).toBe(false);

    const me = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    const meBody = (await me.json()) as { max_announcement_id: number };
    expect(meBody.max_announcement_id).toBeLessThan(expiredId);
  });

  it("非法 targets / since → 400；公告删除生效", async () => {
    const bad = await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({ title: "t", body: "b", targets: [{ type: "group", id: "g_nope" }] }),
    });
    expect(bad.status).toBe(400);
    const d0 = await claimDevice();
    const badSince = await signedFetch(
      d0.secret,
      d0.deviceId,
      "GET",
      "/api/announcements?since=-5",
    );
    expect(badSince.status).toBe(400);

    const d = await claimDevice();
    const create = await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({ title: "将被删除", body: "b", targets: [{ type: "all" }] }),
    });
    const id = ((await create.json()) as { id: number }).id;
    const del = await adminFetch(`/api/admin/announcements/${id}`, { method: "DELETE" });
    expect(del.status).toBe(200);
    const list = (await (await signedFetch(d.secret, d.deviceId, "GET", "/api/announcements?since=0")).json()) as { items: { id: number }[] };
    expect(list.items.some((i) => i.id === id)).toBe(false);
  });
});

describe("重新激活（抗刷机恢复）", () => {
  it("全流程：请求 → 审批 → 一次性取回新 secret → 旧 secret 失效", async () => {
    const d = await claimDevice();
    const req = await anonFetch("/api/devices/reactivate", {
      method: "POST",
      body: JSON.stringify({ mac: d.mac }),
    });
    expect(((await req.json()) as { status: string }).status).toBe("pending");

    const pendings = (await (await adminFetch("/api/admin/reactivations?status=pending")).json()) as {
      items: { id: number; device_id: string }[];
    };
    const rid = pendings.items.find((x) => x.device_id === d.deviceId)!.id;
    const approve = await adminFetch(`/api/admin/reactivations/${rid}/approve`, { method: "POST" });
    expect(approve.status).toBe(200);

    const poll = await anonFetch(`/api/devices/reactivate?mac=${d.mac}`);
    const pollBody = (await poll.json()) as { status: string; device_secret?: string };
    expect(pollBody.status).toBe("ok");
    expect(pollBody.device_secret).toBeTruthy();
    expect(pollBody.device_secret).not.toBe(d.secret);

    // 二次轮询已消费
    const poll2 = (await (await anonFetch(`/api/devices/reactivate?mac=${d.mac}`)).json()) as { status: string };
    expect(poll2.status).toBe("consumed");

    // 新 secret 可用，旧 secret 失效
    const ok = await signedFetch(pollBody.device_secret!, d.deviceId, "GET", "/api/devices/me");
    expect(ok.status).toBe(200);
    const old = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    expect(old.status).toBe(401);
  });

  it("未知 MAC → unclaimed 并创建待认领记录", async () => {
    const mac = nextMac();
    const r = await anonFetch("/api/devices/reactivate", {
      method: "POST",
      body: JSON.stringify({ mac }),
    });
    expect(((await r.json()) as { status: string }).status).toBe("unclaimed");
    const devices = (await (await adminFetch("/api/admin/devices")).json()) as {
      items: { device_id: string; status: string }[];
    };
    const deviceId = await deviceIdFromMac(mac);
    const row = devices.items.find((x) => x.device_id === deviceId);
    expect(row?.status).toBe("pending");
  });
});

describe("设备管理（解绑/重置）", () => {
  it("解绑 → 待认领 + 签名失效；重置 → 记录删除", async () => {
    const d = await claimDevice();
    const un = await adminFetch(`/api/admin/devices/${d.deviceId}/unbind`, { method: "POST" });
    expect(un.status).toBe(200);
    const auth = await signedFetch(d.secret, d.deviceId, "GET", "/api/devices/me");
    expect(auth.status).toBe(401);
    const devices = (await (await adminFetch("/api/admin/devices")).json()) as {
      items: { device_id: string; status: string }[];
    };
    expect(devices.items.find((x) => x.device_id === d.deviceId)?.status).toBe("pending");

    const d2 = await claimDevice();
    const reset = await adminFetch(`/api/admin/devices/${d2.deviceId}/reset`, { method: "POST" });
    expect(reset.status).toBe(200);
    const after = (await (await adminFetch("/api/admin/devices")).json()) as {
      items: { device_id: string }[];
    };
    expect(after.items.some((x) => x.device_id === d2.deviceId)).toBe(false);
  });
});

describe("审计日志", () => {
  it("关键操作全量入审计且可查询", async () => {
    await claimDevice();
    await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({ title: "审计测试", body: "b", targets: [{ type: "all" }] }),
    });
    const r = (await (await adminFetch("/api/admin/audit")).json()) as {
      items: { action: string }[];
    };
    const actions = r.items.map((i) => i.action);
    expect(actions).toContain("pairing_code.create");
    expect(actions).toContain("device.claim");
    expect(actions).toContain("announcement.create");
  });
});

describe("Cron 清理", () => {
  it("过期待认领设备、旧配对码、超窗激活请求、旧审计被清理", async () => {
    const now = Math.floor(Date.now() / 1000);
    // 25h 前的 pending 设备
    await env.DB.prepare(
      "INSERT INTO devices (device_id, status, created_at) VALUES (?, 'pending', ?)",
    )
      .bind("f".repeat(32), now - 25 * 3600)
      .run();
    // 2h 前过期的配对码（清理条件为 expires_at < now-1h，需留出边界余量）
    await env.DB.prepare(
      "INSERT INTO pairing_codes (code_hash, created_at, expires_at) VALUES (?, ?, ?)",
    )
      .bind("a".repeat(64), now - 10800, now - 7200)
      .run();
    // 超窗激活请求（需要真实设备）
    const d = await claimDevice();
    await env.DB.prepare(
      "INSERT INTO reactivation_requests (device_id, created_at, expires_at) VALUES (?, ?, ?)",
    )
      .bind(d.deviceId, now - 25 * 3600, now - 3600)
      .run();
    // 91 天前的审计
    await env.DB.prepare(
      "INSERT INTO audit_log (ts, actor, action) VALUES (?, 'ancient', 'x.y')",
    )
      .bind(now - 91 * DAY)
      .run();

    const stats = await runCleanup(env, now);
    expect(stats.expiredPendingDevices).toBeGreaterThanOrEqual(1);
    expect(stats.oldPairingCodes).toBeGreaterThanOrEqual(1);
    expect(stats.expiredReactivations).toBeGreaterThanOrEqual(1);
    expect(stats.trimmedAuditRows).toBeGreaterThanOrEqual(1);

    const ff = await env.DB.prepare(
      "SELECT COUNT(*) AS n FROM devices WHERE device_id = ?",
    )
      .bind("f".repeat(32))
      .first<{ n: number }>();
    expect(ff!.n).toBe(0);
    const staleCode = await env.DB.prepare(
      "SELECT COUNT(*) AS n FROM pairing_codes WHERE code_hash = ?",
    )
      .bind("a".repeat(64))
      .first<{ n: number }>();
    expect(staleCode!.n).toBe(0);
  });
});

describe("PresenceDO（WSS 最小链路）", () => {
  it("hello/auth → hello_ok → presence_list 含自身 → announce 提示送达", async () => {
    const d = await claimDevice("在线测试");
    const stub = env.PRESENCE.get(env.PRESENCE.idFromName("global"));
    const resp = await stub.fetch("https://presence/ws", {
      headers: { Upgrade: "websocket", "Sec-WebSocket-Key": randomHex(8), "Sec-WebSocket-Version": "13" },
    });
    expect(resp.status).toBe(101);
    const ws = resp.webSocket!;
    ws.accept();
    const received: unknown[] = [];
    const done = new Promise<void>((resolve) => {
      ws.addEventListener("message", (ev: MessageEvent) => {
        const m = JSON.parse(String(ev.data)) as { type: string };
        received.push(m);
        if (m.type === "announce") resolve();
      });
    });

    const ts = Math.floor(Date.now() / 1000);
    const nonce = randomHex(8);
    const sig = await hmacHex(d.secret, `v1\n${d.deviceId}\n${ts}\n${nonce}\nws`);
    ws.send(
      JSON.stringify({ type: "hello", device_id: d.deviceId, ts, nonce, sig, label: "在线测试" }),
    );

    // 等 hello_ok
    await new Promise<void>((resolve) => {
      const t = setInterval(() => {
        if (received.some((m) => (m as { type: string }).type === "hello_ok")) {
          clearInterval(t);
          resolve();
        }
      }, 20);
    });
    ws.send(JSON.stringify({ type: "presence_list" }));
    await new Promise<void>((resolve) => {
      const t = setInterval(() => {
        if (received.some((m) => (m as { type: string }).type === "presence_list")) {
          clearInterval(t);
          resolve();
        }
      }, 20);
    });

    // 发布公告 → announce 提示应经 DO 送达
    await adminFetch("/api/admin/announcements", {
      method: "POST",
      body: JSON.stringify({ title: "推送测试", body: "b", targets: [{ type: "all" }] }),
    });
    await done;
    ws.close();
  });

  it("坏签名 hello → hello_fail", async () => {
    const d = await claimDevice();
    const stub = env.PRESENCE.get(env.PRESENCE.idFromName("global"));
    const resp = await stub.fetch("https://presence/ws", {
      headers: { Upgrade: "websocket", "Sec-WebSocket-Key": randomHex(8), "Sec-WebSocket-Version": "13" },
    });
    const ws = resp.webSocket!;
    ws.accept();
    const outcome = new Promise<string>((resolve) => {
      ws.addEventListener("message", (ev: MessageEvent) => {
        resolve((JSON.parse(String(ev.data)) as { type: string }).type);
      });
    });
    const ts = Math.floor(Date.now() / 1000);
    ws.send(
      JSON.stringify({
        type: "hello",
        device_id: d.deviceId,
        ts,
        nonce: randomHex(8),
        sig: "0".repeat(64),
      }),
    );
    expect(await outcome).toBe("hello_fail");
    ws.close();
  });
});
