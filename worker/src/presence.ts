import type { Env } from "./env";
import { decryptString, hmacHex, timingSafeEqualStr } from "./crypto";
import { getDevice, wsCanon } from "./auth";
import { nowSec } from "./util";

interface WsAttachment {
  authed: boolean;
  deviceId: string;
  label: string;
}

/**
 * PresenceDO（M1 最小实现）
 * - /ws    设备 WSS 通道：hello/auth（HMAC）→ presence_list / ping；公告提示由此下发
 * - /notify  Worker 内部调用：向在线目标设备推送提示（announce 等）
 * - /online  Worker 内部调用：在线设备列表
 * M2 扩展：call_* 信令路由、RoomDO 引导（见 docs/protocol.md）。
 */
export class PresenceDO implements DurableObject {
  constructor(private state: DurableObjectState, private env: Env) {}

  async fetch(req: Request): Promise<Response> {
    const url = new URL(req.url);

    if (url.pathname === "/ws") {
      if (req.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
        return new Response("expected websocket upgrade", { status: 400 });
      }
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1]);
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    if (url.pathname === "/notify" && req.method === "POST") {
      const body = (await req.json()) as {
        type: string;
        deviceIds?: string[];
        payload?: Record<string, unknown>;
      };
      if (body.type !== "announce") return new Response("unknown notify type", { status: 400 });
      const idSet = body.deviceIds ? new Set(body.deviceIds) : null;
      let sent = 0;
      for (const ws of this.state.getWebSockets()) {
        const att = ws.deserializeAttachment() as WsAttachment | null;
        if (att?.authed && (!idSet || idSet.has(att.deviceId))) {
          ws.send(JSON.stringify({ type: "announce", ...(body.payload ?? {}) }));
          sent++;
        }
      }
      return Response.json({ sent });
    }

    if (url.pathname === "/online" && req.method === "GET") {
      const devices: Array<{ id: string; label: string }> = [];
      const seen = new Set<string>();
      for (const ws of this.state.getWebSockets()) {
        const att = ws.deserializeAttachment() as WsAttachment | null;
        if (att?.authed && !seen.has(att.deviceId)) {
          seen.add(att.deviceId);
          devices.push({ id: att.deviceId, label: att.label || att.deviceId.slice(0, 8) });
        }
      }
      return Response.json({ devices });
    }

    return new Response("not found", { status: 404 });
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message !== "string") {
      ws.close(1003, "binary not accepted on control channel");
      return;
    }
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(message) as Record<string, unknown>;
    } catch {
      ws.send(JSON.stringify({ type: "error", reason: "bad_json" }));
      return;
    }

    if (msg.type === "hello") {
      await this.handleHello(ws, msg);
      return;
    }

    const att = ws.deserializeAttachment() as WsAttachment | null;
    if (!att?.authed) {
      ws.send(JSON.stringify({ type: "error", reason: "unauthenticated" }));
      ws.close(1008, "unauthenticated");
      return;
    }
    switch (msg.type) {
      case "ping":
        ws.send(JSON.stringify({ type: "pong", ts: nowSec() }));
        return;
      case "presence_list": {
        const devices: Array<{ id: string; label: string }> = [];
        const seen = new Set<string>();
        for (const w of this.state.getWebSockets()) {
          const a = w.deserializeAttachment() as WsAttachment | null;
          if (a?.authed && !seen.has(a.deviceId)) {
            seen.add(a.deviceId);
            devices.push({ id: a.deviceId, label: a.label || a.deviceId.slice(0, 8) });
          }
        }
        ws.send(JSON.stringify({ type: "presence_list", devices }));
        return;
      }
      default:
        ws.send(JSON.stringify({ type: "error", reason: "unknown_type" }));
    }
  }

  private async handleHello(ws: WebSocket, msg: Record<string, unknown>): Promise<void> {
    try {
      const deviceId = String(msg.device_id ?? "");
      const ts = Number(msg.ts);
      const nonce = String(msg.nonce ?? "");
      const sig = String(msg.sig ?? "");
      const label = String(msg.label ?? "").slice(0, 64);
      if (!/^[0-9a-f]{32}$/.test(deviceId)) throw new Error("bad device_id");
      if (!Number.isInteger(ts) || Math.abs(nowSec() - ts) > 120) throw new Error("stale ts");
      if (!/^[0-9a-f]{16,64}$/.test(nonce)) throw new Error("bad nonce");

      const row = await getDevice(this.env, deviceId);
      if (!row?.secret_enc || row.status !== "active") throw new Error("device not active");

      const secret = await decryptString(row.secret_enc, this.env.SECRET_ENC_KEY);
      const expected = await hmacHex(secret, wsCanon(deviceId, ts, nonce));
      if (!timingSafeEqualStr(sig, expected)) throw new Error("bad signature");

      ws.serializeAttachment({ authed: true, deviceId, label } satisfies WsAttachment);
      ws.send(JSON.stringify({ type: "hello_ok", device_id: deviceId }));
    } catch (e) {
      ws.send(
        JSON.stringify({ type: "hello_fail", reason: (e as Error).message ?? "auth failed" }),
      );
      ws.close(1008, "auth failed");
    }
  }

  async webSocketClose(): Promise<void> {}
  async webSocketError(): Promise<void> {}
}
