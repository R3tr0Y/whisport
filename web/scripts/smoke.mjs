/**
 * 本地冒烟测试：对运行中的 wrangler dev 走通 M1 全链路。
 * 用法：node scripts/smoke.mjs [base_url]
 */
const BASE = process.argv[2] ?? "https://whisport.pages.dev";
const ADMIN_TOKEN = process.env.ADMIN_TOKEN ?? "dev-admin-token";

let pass = 0;
let fail = 0;
function check(name, cond, extra = "") {
  if (cond) {
    pass++;
    console.log(`  ✓ ${name}`);
  } else {
    fail++;
    console.error(`  ✗ ${name} ${extra}`);
  }
}

const te = new TextEncoder();
const hex = (b) => [...b].map((x) => x.toString(16).padStart(2, "0")).join("");
const sha256 = async (s) => hex(new Uint8Array(await crypto.subtle.digest("SHA-256", te.encode(s))));
async function hmac(secretHex, msg) {
  const key = await crypto.subtle.importKey(
    "raw",
    new Uint8Array(secretHex.match(/../g).map((h) => parseInt(h, 16))),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  return hex(new Uint8Array(await crypto.subtle.sign("HMAC", key, te.encode(msg))));
}

async function admin(path, init = {}) {
  return fetch(BASE + path, {
    ...init,
    headers: { "content-type": "application/json", authorization: `Bearer ${ADMIN_TOKEN}`, ...(init.headers ?? {}) },
  });
}

const mac = "aabbccddeef0";
const deviceId = (await sha256(mac)).slice(0, 32);

console.log("M1 冒烟 →", BASE);

// 0. 静态站点
const page = await fetch(BASE + "/");
check("静态管理后台可访问", page.ok && (await page.text()).includes("Whisport"));

// 1. 管理鉴权
check("whoami", (await (await admin("/api/admin/whoami")).json()).ok === true);

// 2. 配对码 → 认领
const pc = await (
  await admin("/api/admin/pairing-codes", { method: "POST", body: JSON.stringify({ note: "smoke" }) })
).json();
check("生成配对码", /^\d{6}$/.test(pc.code), JSON.stringify(pc));

const claim = await fetch(BASE + "/api/devices/claim", {
  method: "POST",
  headers: { "content-type": "application/json", "user-agent": "whisport-smoke/1.0" },
  body: JSON.stringify({ pairing_code: pc.code, mac, label: "冒烟工牌" }),
});
const { device_id, device_secret } = await claim.json();
check("设备认领", claim.ok && device_id === deviceId && device_secret.length === 64);

// 3. HMAC 签名请求
async function signed(method, path) {
  const ts = Math.floor(Date.now() / 1000);
  const nonce = hex(crypto.getRandomValues(new Uint8Array(8)));
  const sig = await hmac(device_secret, `v1\n${deviceId}\n${ts}\n${nonce}\n${method}\n${path.split("?")[0]}`);
  return fetch(BASE + path, {
    method,
    headers: { "x-device-id": deviceId, "x-timestamp": String(ts), "x-nonce": nonce, "x-signature": sig },
  });
}

const me = await (await signed("GET", "/api/devices/me")).json();
check("设备 /me", me.device?.label === "冒烟工牌" && me.device?.status === "active");

// 4. 公告发布 → 设备可见
const ann = await (
  await admin("/api/admin/announcements", {
    method: "POST",
    body: JSON.stringify({ title: "冒烟公告", body: "smoke test", targets: [{ type: "all" }] }),
  })
).json();
check("发布公告", Number.isInteger(ann.id));

const list = await (await signed("GET", "/api/announcements?since=0")).json();
check("设备拉到公告", list.items.some((i) => i.id === ann.id && i.title === "冒烟公告"));
check("游标推进", list.cursor >= ann.id);

// 5. 审计可回溯
const auditAll = await (await admin("/api/admin/audit")).json();
check("审计含配对码/认领/发布", ["pairing_code.create", "device.claim", "announcement.create"].every((a) => auditAll.items.some((x) => x.action === a)));

console.log(`\n结果：${pass} 通过，${fail} 失败`);
process.exit(fail ? 1 : 0);
