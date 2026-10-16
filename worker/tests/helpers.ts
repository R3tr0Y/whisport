import { SELF, env as cfEnv } from "cloudflare:test";
import type { Env } from "../src/env";
import { hmacHex, randomHex, randomInt } from "../src/crypto";
import { restCanon } from "../src/auth";

export const env = cfEnv as unknown as Env;
export const BASE = "https://test.local";

let macCounter = 0x100000;

/** 唯一假 MAC（aabb… 前缀，仅测试） */
export function nextMac(): string {
  macCounter += 1;
  return (macCounter.toString(16).padStart(12, "0") + "000000000000").slice(0, 12);
}

export async function adminFetch(
  path: string,
  init: RequestInit = {},
): Promise<Response> {
  const headers = new Headers(init.headers);
  headers.set("authorization", `Bearer ${env.ADMIN_TOKEN}`);
  headers.set("content-type", "application/json");
  return SELF.fetch(BASE + path, { ...init, headers });
}

export async function anonFetch(path: string, init: RequestInit = {}): Promise<Response> {
  const headers = new Headers(init.headers);
  if (init.body) headers.set("content-type", "application/json");
  // 每次匿名请求带随机模拟 IP，避免测试触发真实的按 IP 限速
  if (!headers.has("cf-connecting-ip")) {
    headers.set("cf-connecting-ip", `10.${randomInt(255)}.${randomInt(255)}.${randomInt(255)}`);
  }
  return SELF.fetch(BASE + path, { ...init, headers });
}

/** 管理员生成配对码（明文） */
export async function newPairingCode(): Promise<string> {
  const r = await adminFetch("/api/admin/pairing-codes", {
    method: "POST",
    body: JSON.stringify({ note: "test" }),
  });
  if (r.status !== 201) throw new Error(`pairing code failed: ${await r.text()}`);
  return ((await r.json()) as { code: string }).code;
}

export interface Claimed {
  deviceId: string;
  secret: string;
  mac: string;
}

/** 全流程认领一台测试设备 */
export async function claimDevice(label?: string): Promise<Claimed> {
  const mac = nextMac();
  const r = await anonFetch("/api/devices/claim", {
    method: "POST",
    body: JSON.stringify({ pairing_code: await newPairingCode(), mac, label }),
  });
  if (r.status !== 200) throw new Error(`claim failed: ${await r.text()}`);
  const body = (await r.json()) as { device_id: string; device_secret: string };
  return { deviceId: body.device_id, secret: body.device_secret, mac };
}

/** 设备 HMAC 签名请求 */
export async function signedFetch(
  secret: string,
  deviceId: string,
  method: string,
  path: string,
  body?: unknown,
  overrides?: { ts?: number; nonce?: string; sig?: string },
): Promise<Response> {
  const ts = overrides?.ts ?? Math.floor(Date.now() / 1000);
  const nonce = overrides?.nonce ?? randomHex(8);
  const sig =
    overrides?.sig ??
    (await hmacHex(secret, restCanon(deviceId, ts, nonce, method, path.split("?")[0]!)));
  const headers: Record<string, string> = {
    "x-device-id": deviceId,
    "x-timestamp": String(ts),
    "x-nonce": nonce,
    "x-signature": sig,
  };
  if (body !== undefined) headers["content-type"] = "application/json";
  return SELF.fetch(BASE + path, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
}
