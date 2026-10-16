import type { Env } from "./env";
import { decryptString, hmacHex, timingSafeEqualStr, sha256hex } from "./crypto";
import { ApiError, clientIp, nowSec } from "./util";

/** REST 设备签名规范化串（与固件 app_api.c 严格一致，勿改动） */
export function restCanon(
  deviceId: string,
  ts: number,
  nonce: string,
  method: string,
  pathname: string,
): string {
  return `v1\n${deviceId}\n${ts}\n${nonce}\n${method}\n${pathname}`;
}

/** WSS hello 签名规范化串 */
export function wsCanon(deviceId: string, ts: number, nonce: string): string {
  return `v1\n${deviceId}\n${ts}\n${nonce}\nws`;
}

const TS_WINDOW_SEC = 120;

/** 管理员鉴权：Authorization: Bearer <ADMIN_TOKEN> */
export function requireAdmin(req: Request, env: Env): void {
  const auth = req.headers.get("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";
  if (!token || !timingSafeEqualStr(token, env.ADMIN_TOKEN)) {
    throw new ApiError(401, "unauthorized", "admin token required");
  }
}

export interface DeviceRow {
  device_id: string;
  status: string;
  secret_enc: string | null;
  last_seen: number | null;
}

export async function getDevice(env: Env, deviceId: string): Promise<DeviceRow | null> {
  return env.DB.prepare(
    "SELECT device_id, status, secret_enc, last_seen FROM devices WHERE device_id = ?",
  )
    .bind(deviceId)
    .first<DeviceRow | null>();
}

/**
 * 设备 HMAC 鉴权。
 * 头部：X-Device-Id / X-Timestamp(秒) / X-Nonce(≥8B hex) / X-Signature
 * 签名串见 restCanon()；时间窗 ±120s；nonce 经 KV 防重放。
 */
export async function deviceAuth(req: Request, env: Env): Promise<{ deviceId: string }> {
  const deviceId = req.headers.get("x-device-id") ?? "";
  const tsStr = req.headers.get("x-timestamp") ?? "";
  const nonce = req.headers.get("x-nonce") ?? "";
  const sig = req.headers.get("x-signature") ?? "";

  if (!deviceId || !tsStr || !nonce || !sig) {
    throw new ApiError(401, "missing_auth_headers", "X-Device-Id/X-Timestamp/X-Nonce/X-Signature required");
  }
  if (!/^[0-9a-f]{32}$/.test(deviceId)) throw new ApiError(401, "bad_device_id", "malformed device id");
  const ts = Number(tsStr);
  if (!Number.isInteger(ts) || Math.abs(nowSec() - ts) > TS_WINDOW_SEC) {
    throw new ApiError(401, "stale_timestamp", "timestamp outside allowed window");
  }
  if (!/^[0-9a-f]{16,64}$/.test(nonce)) throw new ApiError(401, "bad_nonce", "malformed nonce");

  const row = await getDevice(env, deviceId);
  if (!row || !row.secret_enc) throw new ApiError(401, "unknown_device", "device not claimed");
  if (row.status === "suspended") throw new ApiError(403, "suspended", "device is suspended");
  if (row.status !== "active") throw new ApiError(403, "not_active", "device is not active");

  const secret = await decryptString(row.secret_enc, env.SECRET_ENC_KEY);
  const expected = await hmacHex(secret, restCanon(deviceId, ts, nonce, req.method, new URL(req.url).pathname));
  if (!timingSafeEqualStr(sig, expected)) {
    throw new ApiError(401, "bad_signature", "signature mismatch");
  }

  // nonce 防重放：5 分钟 TTL 覆盖时间窗
  const nonceKey = `nonce:${deviceId}:${nonce}`;
  try {
    const seen = await env.CACHE.get(nonceKey);
    if (seen) throw new ApiError(401, "replayed_nonce", "nonce already used");
    await env.CACHE.put(nonceKey, "1", { expirationTtl: 300 });
  } catch (e) {
    if (e instanceof ApiError) throw e;
    // KV 不可用时降级放行（尽力而为防重放）
    console.warn("nonce kv unavailable:", e);
  }

  // last_seen 节流写：60 秒内不重复更新
  if (!row.last_seen || nowSec() - row.last_seen > 60) {
    await env.DB.prepare("UPDATE devices SET last_seen = ? WHERE device_id = ?")
      .bind(nowSec(), deviceId)
      .run();
  }
  return { deviceId };
}

/** 按 IP 限速（尽力而为，KV 最终一致，足够玩具规模） */
export async function rateLimit(
  env: Env,
  req: Request,
  bucket: string,
  max: number,
  windowSec: number,
): Promise<void> {
  const key = `rl:${bucket}:${clientIp(req)}`;
  try {
    const cur = Number((await env.CACHE.get(key)) ?? "0");
    if (cur >= max) {
      throw new ApiError(429, "rate_limited", `too many requests, retry in ${windowSec}s`);
    }
    await env.CACHE.put(key, String(cur + 1), { expirationTtl: windowSec });
  } catch (e) {
    if (e instanceof ApiError) throw e;
    console.warn("rate limit kv unavailable:", e);
  }
}

/** 由规范化 MAC 派生 device_id：sha256 前 16 字节 hex */
export async function deviceIdFromMac(mac: string): Promise<string> {
  return (await sha256hex(mac)).slice(0, 32);
}
