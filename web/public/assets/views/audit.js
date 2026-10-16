import { api, esc, fmtTime, toast } from "../api.js";

export async function renderAudit(root) {
  let filter = "";
  let beforeId = 0;

  root.innerHTML = `
    <div class="section-title"><h2>审计日志</h2></div>
    <div class="card">
      <div class="row">
        <input id="au-filter" placeholder="按动作过滤（如 device. / announcement.）" style="flex:1" value="${esc(filter)}">
        <button class="primary" id="au-search">查询</button>
      </div>
    </div>
    <div class="card">
      <table>
        <thead><tr><th>时间</th><th>操作者</th><th>动作</th><th>详情</th></tr></thead>
        <tbody id="au-body"></tbody>
      </table>
      <div style="margin-top:10px;text-align:center">
        <button class="ghost" id="au-more" hidden>加载更多</button>
      </div>
    </div>`;

  const body = root.querySelector("#au-body");
  const moreBtn = root.querySelector("#au-more");

  async function load(reset) {
    if (reset) {
      beforeId = 0;
      body.innerHTML = "";
    }
    const q = new URLSearchParams({ limit: "50" });
    if (filter) q.set("action", filter);
    if (beforeId) q.set("before_id", String(beforeId));
    try {
      const { items } = await api(`/api/admin/audit?${q}`);
      if (!items.length && reset) {
        body.innerHTML = '<tr><td colspan="4" class="muted">无记录</td></tr>';
      } else {
        body.insertAdjacentHTML(
          "beforeend",
          items
            .map(
              (a) => `
          <tr>
            <td>${fmtTime(a.ts)}</td>
            <td class="mono">${esc(a.actor)}</td>
            <td>${esc(a.action)}</td>
            <td><pre class="detail">${esc(a.detail)}</pre></td>
          </tr>`,
            )
            .join(""),
        );
      }
      beforeId = items.length ? items[items.length - 1].id : 0;
      moreBtn.hidden = items.length < 50;
    } catch (e) {
      toast(e.message, true);
    }
  }

  root.querySelector("#au-search").addEventListener("click", () => {
    filter = root.querySelector("#au-filter").value.trim();
    load(true);
  });
  root.querySelector("#au-filter").addEventListener("keydown", (e) => {
    if (e.key === "Enter") root.querySelector("#au-search").click();
  });
  moreBtn.addEventListener("click", () => load(false));

  await load(true);
}
