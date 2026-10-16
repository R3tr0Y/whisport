/** API 错误：携带 HTTP 状态码与机器可读 code */
export class ApiError extends Error {
  constructor(
    public status: number,
    public code: string,
    message: string,
  ) {
    super(message);
  }
}

export function json(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });
}

export function errorResponse(e: unknown): Response {
  if (e instanceof ApiError) {
    return json({ error: { code: e.code, message: e.message } }, e.status);
  }
  console.error("unhandled error:", e);
  return json({ error: { code: "internal", message: "internal server error" } }, 500);
}

export function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

export async function readJson<T>(req: Request): Promise<T> {
  try {
    return (await req.json()) as T;
  } catch {
    throw new ApiError(400, "bad_json", "request body is not valid JSON");
  }
}

/** 规范化 MAC：去分隔符、转小写，校验 12 位 hex */
export function normalizeMac(mac: string): string {
  const m = mac.replace(/[:-\s]/g, "").toLowerCase();
  if (!/^[0-9a-f]{12}$/.test(m)) {
    throw new ApiError(400, "bad_mac", "invalid MAC address");
  }
  return m;
}

export function requireString(v: unknown, field: string, maxLen = 256): string {
  if (typeof v !== "string" || v.length === 0) {
    throw new ApiError(400, "bad_request", `field '${field}' is required`);
  }
  if (v.length > maxLen) {
    throw new ApiError(400, "bad_request", `field '${field}' too long (max ${maxLen})`);
  }
  return v;
}

export function clientIp(req: Request): string {
  return req.headers.get("cf-connecting-ip") ?? "local";
}
