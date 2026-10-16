import { getToken, setToken, api, toast, esc } from "./api.js";
import { renderDevices } from "./views/devices.js";
import { renderPairing } from "./views/pairing.js";
import { renderAnnouncements } from "./views/announcements.js";
import { renderGroups } from "./views/groups.js";
import { renderAudit } from "./views/audit.js";

const views = {
  devices: renderDevices,
  pairing: renderPairing,
  announcements: renderAnnouncements,
  groups: renderGroups,
  audit: renderAudit,
};

const viewEl = document.getElementById("view");
const navEl = document.getElementById("nav");
const overlay = document.getElementById("login-overlay");
const logoutBtn = document.getElementById("btn-logout");
const loginForm = document.getElementById("login-form");
const loginToken = document.getElementById("login-token");
const loginError = document.getElementById("login-error");

let currentView = "devices";

function showLogin() {
  overlay.hidden = false;
  loginToken.value = getToken();
  loginToken.focus();
}

function hideLogin() {
  overlay.hidden = true;
  logoutBtn.hidden = false;
}

async function boot() {
  if (!getToken()) return showLogin();
  try {
    await api("/api/admin/whoami");
    hideLogin();
    switchView(currentView);
  } catch {
    /* 401 已触发 auth-required */
  }
}

loginForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  loginError.hidden = true;
  setToken(loginToken.value.trim());
  try {
    await api("/api/admin/whoami");
    hideLogin();
    toast("登录成功");
    switchView(currentView);
  } catch {
    loginError.hidden = false;
    setToken("");
  }
});

logoutBtn.addEventListener("click", () => {
  setToken("");
  location.reload();
});

window.addEventListener("auth-required", () => {
  showLogin();
  toast("登录已失效，请重新输入令牌", true);
});

async function switchView(name) {
  currentView = name;
  for (const b of navEl.querySelectorAll("button")) {
    b.classList.toggle("active", b.dataset.view === name);
  }
  viewEl.replaceChildren();
  viewEl.innerHTML = `<p class="muted">加载中…</p>`;
  try {
    await views[name](viewEl);
  } catch (e) {
    if (e?.status !== 401) {
      viewEl.innerHTML = `<p class="error">加载失败：${esc(String(e.message ?? e))}</p>`;
    }
  }
}

navEl.addEventListener("click", (e) => {
  const btn = e.target.closest("button[data-view]");
  if (btn) switchView(btn.dataset.view);
});

boot();
