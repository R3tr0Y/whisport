const TOKEN_KEY = "whisport_token";

export function getToken() {
  return localStorage.getItem(TOKEN_KEY) ?? "";
}

export function setToken(t) {
  if (t) localStorage.setItem(TOKEN_KEY, t);
  else localStorage.removeItem(TOKEN_KEY);
}

export class ApiError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

export async function api(path, opts = {}) {
  const headers = {
    "content-type": "application/json",
    authorization: `Bearer ${getToken()}`,
    ...(opts.headers ?? {}),
  };
  const r = await fetch(path, { ...opts, headers });
  if (r.status === 401) {
    window.dispatchEvent(new Event("auth-required"));
    throw new ApiError(401, "未授权");
  }
  const body = await r.json().catch(() => ({}));
  if (!r.ok) throw new ApiError(r.status, body?.error?.message ?? `HTTP ${r.status}`);
  return body;
}

export function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c],
  );
}

export function fmtTime(ts) {
  if (!ts) return "—";
  return new Date(ts * 1000).toLocaleString("zh-CN", { hour12: false });
}

export function fmtAgo(ts) {
  if (!ts) return "从未";
  const s = Math.floor(Date.now() / 1000) - ts;
  if (s < 60) return `${s} 秒前`;
  if (s < 3600) return `${Math.floor(s / 60)} 分钟前`;
  if (s < 86400) return `${Math.floor(s / 3600)} 小时前`;
  return `${Math.floor(s / 86400)} 天前`;
}

export function short(id, n = 8) {
  return id ? `${String(id).slice(0, n)}…` : "—";
}

export function toast(msg, isErr = false) {
  const el = document.getElementById("toast");
  el.textContent = msg;
  el.className = `toast${isErr ? " err" : ""}`;
  el.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (el.hidden = true), 2600);
}

export async function confirmAction(msg) {
  return window.confirm(msg);
}
