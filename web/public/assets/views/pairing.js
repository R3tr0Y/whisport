import { api, esc, fmtTime, toast } from "../api.js";

export async function renderPairing(root) {
  const { items: codes } = await api("/api/admin/pairing-codes");

  root.innerHTML = `
    <div class="section-title"><h2>配对码</h2></div>
    <div class="card">
      <h3>生成一次性配对码</h3>
      <div class="row">
        <input id="pc-note" placeholder="备注（可选，如「新工牌 #3」）" style="flex:1">
        <button class="primary" id="pc-gen">生成</button>
      </div>
      <div id="pc-result"></div>
      <p class="muted">6 位数字 · 10 分钟有效 · 单次使用。设备端输入配对码完成认领（首次或重新绑定）。</p>
    </div>
    <div class="card">
      <h3>近期记录</h3>
      <table>
        <thead><tr><th>码(哈希前缀)</th><th>备注</th><th>创建</th><th>状态</th></tr></thead>
        <tbody>
          ${
            codes.length === 0
              ? '<tr><td colspan="4" class="muted">暂无记录</td></tr>'
              : codes
                  .map(
                    (c) => `
            <tr>
              <td class="mono">${esc(c.code_hint)}</td>
              <td>${esc(c.note ?? "—")}</td>
              <td>${fmtTime(c.created_at)}</td>
              <td>${
                c.used_at
                  ? `<span class="badge consumed">已用 · ${esc(c.used_by_label ?? "")}</span>`
                  : c.expires_at * 1000 < Date.now()
                    ? '<span class="badge expired">已过期</span>'
                    : '<span class="badge active">有效</span>'
              }</td>
            </tr>`,
                  )
                  .join("")
          }
        </tbody>
      </table>
    </div>`;

  root.querySelector("#pc-gen").addEventListener("click", async () => {
    try {
      const body = await api("/api/admin/pairing-codes", {
        method: "POST",
        body: JSON.stringify({ note: root.querySelector("#pc-note").value.trim() || undefined }),
      });
      root.querySelector("#pc-result").innerHTML = `
        <div class="code-display">${esc(body.code)}</div>
        <p class="muted" style="text-align:center">有效期至 ${fmtTime(body.expires_at)}（仅显示一次）</p>`;
      toast("配对码已生成");
      // 刷新下方列表
      const refresh = document.createElement("button");
      refresh.className = "ghost mini";
      refresh.textContent = "刷新记录";
      refresh.addEventListener("click", () => renderPairing(root));
      root.querySelector("#pc-result").appendChild(refresh);
    } catch (e) {
      toast(e.message, true);
    }
  });
}
