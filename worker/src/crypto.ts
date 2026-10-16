/** WebCrypto 封装：SHA-256 / HMAC / AES-GCM / 常量时间比较 */

const te = new TextEncoder();
const td = new TextDecoder();

export function hex(bytes: Uint8Array): string {
  return [...bytes].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export function hexBytes(h: string): Uint8Array {
  if (h.length % 2 !== 0 || !/^[0-9a-f]+$/i.test(h)) {
    throw new Error("invalid hex string");
  }
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(h.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function randomHex(nBytes: number): string {
  return hex(crypto.getRandomValues(new Uint8Array(nBytes)));
}

/** [0, max) 均匀随机整数（拒绝采样避免模偏差） */
export function randomInt(max: number): number {
  const limit = Math.floor(0x100000000 / max) * max;
  const buf = new Uint32Array(1);
  for (;;) {
    crypto.getRandomValues(buf);
    if (buf[0]! < limit) return buf[0]! % max;
  }
}

export async function sha256hex(s: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", te.encode(s));
  return hex(new Uint8Array(d));
}

/** HMAC-SHA256，secret 为 hex 字符串，输出小写 hex */
export async function hmacHex(secretHex: string, msg: string): Promise<string> {
  const key = await crypto.subtle.importKey(
    "raw",
    hexBytes(secretHex) as BufferSource,
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const sig = await crypto.subtle.sign("HMAC", key, te.encode(msg));
  return hex(new Uint8Array(sig));
}

/** 字符串常量时间比较（等长假设下逐字符 XOR 累积） */
export function timingSafeEqualStr(a: string, b: string): boolean {
  const max = Math.max(a.length, b.length);
  let diff = a.length ^ b.length;
  for (let i = 0; i < max; i++) {
    diff |= (a.charCodeAt(i) ^ b.charCodeAt(i)) | 0;
  }
  return diff === 0;
}

// ---- AES-256-GCM：device_secret 加密存储（v1.<iv_b64url>.<ct_b64url>） ----

function b64url(bytes: Uint8Array): string {
  let bin = "";
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function b64urlBytes(s: string): Uint8Array {
  const b64 = s.replace(/-/g, "+").replace(/_/g, "/");
  const bin = atob(b64 + "=".repeat((4 - (b64.length % 4)) % 4));
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

async function aesKey(keyHex: string): Promise<CryptoKey> {
  const raw = hexBytes(keyHex);
  if (raw.length !== 32) throw new Error("SECRET_ENC_KEY must be 64 hex chars (32 bytes)");
  return crypto.subtle.importKey("raw", raw as BufferSource, "AES-GCM", false, ["encrypt", "decrypt"]);
}

export async function encryptString(plain: string, keyHex: string): Promise<string> {
  const key = await aesKey(keyHex);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const ct = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, te.encode(plain));
  return `v1.${b64url(iv)}.${b64url(new Uint8Array(ct))}`;
}

export async function decryptString(enc: string, keyHex: string): Promise<string> {
  const [v, ivS, ctS] = enc.split(".");
  if (v !== "v1" || !ivS || !ctS) throw new Error("malformed secret_enc");
  const key = await aesKey(keyHex);
  const plain = await crypto.subtle.decrypt(
    { name: "AES-GCM", iv: b64urlBytes(ivS) as BufferSource },
    key,
    b64urlBytes(ctS) as BufferSource,
  );
  return td.decode(plain);
}
