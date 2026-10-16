import { api, esc, fmtTime, toast, confirmAction } from "../api.js";

export async function renderAnnouncements(root) {
  const [{ items: anns }, { items: groups }, { items: devices }] = await Promise.all([
    api("/api/admin/announcements"),
    api("/api/admin/groups"),
    api("/api/admin/devices"),
  ]);

  root.innerHTML = `
    <div class="section-title"><h2>公告</h2></div>
    <div class="card">
      <h3>发布公告</h3>
      <form id="ann-form">
        <div class="row" style="margin-bottom:10px">
          <input id="ann-title" placeholder="标题" required style="flex:1" maxlength="120">
          <select id="ann-priority" title="优先级">
            <option value="0">普通</option>
            <option value="1">重要</option>
            <option value="2">紧急</option>
          </select>
          <input id="ann-expires" type="datetime-local" title="过期时间（可选）">
        </div>
        <textarea id="ann-body" placeholder="正文" required maxlength="8000"></textarea>
        <div style="margin:10px 0">
          <strong>可见范围：</strong>
          <label class="check"><input type="checkbox" id="tgt-all"> 全员</label>
          <span id="tgt-groups" style="margin-left:8px">
            ${groups.map((g) => `<label class="check"><input type="checkbox" class="tgt-group" value="${esc(g.group_id)}"> ${esc(g.name)}</label>`).join("") || '<span class="muted">（无分组）</span>'}
          </span>
        </div>
        <div>
          <strong>点名设备：</strong><span class="muted">（可选）</span><br>
          <div id="tgt-devices" style="max-height:120px;overflow:auto;margin-top:4px">
            ${devices
              .map(
                (d) =>
                  `<label class="check"><input type="checkbox" class="tgt-device" value="${esc(d.device_id)}"> ${esc(d.label ?? d.device_id.slice(0, 8))}<span class="muted">(${esc(d.status)})</span></label>`,
              )
              .join("") || '<span class="muted">（无设备）</span>'}
          </div>
        </div>
        <div style="margin-top:12px"><button class="primary" type="submit">发布</button></div>
      </form>
    </div>
    <div class="card">
      <h3>已发布（${anns.length}）</h3>
      <table>
        <thead><tr><th>ID</th><th>标题</th><th>级别</th><th>可见目标</th><th>发布时间</th><th>过期</th><th></th></tr></thead>
        <tbody>
          ${
            anns.length === 0
              ? '<tr><td colspan="7" class="muted">暂无公告</td></tr>'
              : anns
                  .map(
                    (a) => `
            <tr>
              <td class="mono">${a.id}</td>
              <td><strong>${esc(a.title)}</strong><div class="muted" style="max-width:360px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(a.body)}</div></td>
              <td>${a.priority > 0 ? `<span class="badge ${a.priority >= 2 ? "suspended" : "pending"}">${a.priority >= 2 ? "紧急" : "重要"}</span>` : "普通"}</td>
              <td>${esc(a.targets_text ?? "—")}</td>
              <td>${fmtTime(a.created_at)}</td>
              <td>${a.expires_at ? fmtTime(a.expires_at) : "—"}</td>
              <td><button class="mini danger" data-del="${a.id}">删除</button></td>
            </tr>`,
                  )
                  .join("")
          }
        </tbody>
      </table>
    </div>`;

  root.querySelector("#ann-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const targets = [];
    if (root.querySelector("#tgt-all").checked) targets.push({ type: "all" });
    for (const cb of root.querySelectorAll(".tgt-group:checked")) targets.push({ type: "group", id: cb.value });
    for (const cb of root.querySelectorAll(".tgt-device:checked")) targets.push({ type: "device", id: cb.value });
    if (!targets.length) return toast("至少选择一个可见目标", true);

    let expires_at;
    const exp = root.querySelector("#ann-expires").value;
    if (exp) {
      expires_at = Math.floor(new Date(exp).getTime() / 1000);
      if (expires_at <= Date.now() / 1000) return toast("过期时间必须在未来", true);
    }

    try {
      await api("/api/admin/announcements", {
        method: "POST",
        body: JSON.stringify({
          title: root.querySelector("#ann-title").value.trim(),
          body: root.querySelector("#ann-body").value,
          priority: Number(root.querySelector("#ann-priority").value),
          expires_at: expires_at ?? null,
          targets,
        }),
      });
      toast("公告已发布，在线设备将收到提示");
      await renderAnnouncements(root);
    } catch (err) {
      toast(err.message, true);
    }
  });

  root.addEventListener("click", async (e) => {
    const del = e.target.closest("button[data-del]");
    if (!del) return;
    if (!(await confirmAction(`删除公告 #${del.dataset.del}？`))) return;
    try {
      await api(`/api/admin/announcements/${del.dataset.del}`, { method: "DELETE" });
      toast("已删除");
      await renderAnnouncements(root);
    } catch (err) {
      toast(err.message, true);
    }
  });
}
