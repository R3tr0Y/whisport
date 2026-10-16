import { api, esc, fmtTime, fmtAgo, short, toast, confirmAction } from "../api.js";

export async function renderDevices(root) {
  const [{ items: devices }, { items: pendings }] = await Promise.all([
    api("/api/admin/devices"),
    api("/api/admin/reactivations?status=pending"),
  ]);

  const html = [];

  if (pendings.length) {
    html.push(`
      <div class="card">
        <h3>重新激活审批 <span class="badge pending">${pendings.length} 待处理</span></h3>
        <table>
          <thead><tr><th>设备</th><th>请求时间</th><th>状态</th><th>操作</th></tr></thead>
          <tbody>
            ${pendings
              .map(
                (r) => `
              <tr>
                <td>${esc(r.device_label ?? short(r.device_id))}<div class="muted mono">${short(r.device_id, 12)}</div></td>
                <td>${fmtTime(r.created_at)}</td>
                <td><span class="badge ${esc(r.status)}">${esc(r.status)}</span></td>
                <td class="actions">
                  <button class="primary mini" data-approve="${r.id}">批准</button>
                  <button class="danger mini" data-deny="${r.id}">拒绝</button>
                </td>
              </tr>`,
              )
              .join("")}
          </tbody>
        </table>
        <p class="muted">批准后设备下次轮询时取回新密钥（签发一次，旧密钥立即失效）。</p>
      </div>`);
  }

  html.push(`
    <div class="section-title"><h2>设备（${devices.length}）</h2></div>
    <div class="card">
      <table>
        <thead>
          <tr><th>显示名 / 主人</th><th>状态</th><th>分组</th><th>最近在线</th><th>认领时间</th><th>操作</th></tr>
        </thead>
        <tbody>
          ${
            devices.length === 0
              ? '<tr><td colspan="6" class="muted">暂无设备。生成配对码后让设备完成认领。</td></tr>'
              : devices
                  .map(
                    (d) => `
            <tr data-device="${esc(d.device_id)}">
              <td>
                <strong>${esc(d.label ?? "(未命名)")}</strong>
                <div class="muted">${esc(d.owner_name ?? "")}</div>
                <div class="muted mono" title="${esc(d.device_id)}">${short(d.device_id, 10)}</div>
              </td>
              <td><span class="badge ${esc(d.status)}">${esc(d.status)}</span>
                ${d.approved_reactivation ? '<div class="muted">有已批准待取回</div>' : ""}</td>
              <td>${esc(d.groups ?? "—")}</td>
              <td>${fmtAgo(d.last_seen)}</td>
              <td>${fmtTime(d.activated_at)}</td>
              <td class="actions">
                <button class="mini" data-act="rename">改名</button>
                <button class="mini" data-act="suspend">${d.status === "suspended" ? "恢复" : "暂停"}</button>
                <button class="mini" data-act="unbind">解绑</button>
                <button class="mini danger" data-act="reset">重置</button>
              </td>
            </tr>`,
                  )
                  .join("")
          }
        </tbody>
      </table>
    </div>`);

  root.innerHTML = html.join("");

  root.addEventListener("click", onAction);

  async function onAction(e) {
    const btn = e.target.closest("button");
    if (!btn) return;

    const approveId = btn.dataset.approve;
    const denyId = btn.dataset.deny;
    if (approveId || denyId) {
      const id = approveId ?? denyId;
      try {
        await api(`/api/admin/reactivations/${id}/${approveId ? "approve" : "deny"}`, { method: "POST" });
        toast(approveId ? "已批准，等待设备取回新密钥" : "已拒绝");
        root.removeEventListener("click", onAction);
        await renderDevices(root);
      } catch (err) {
        toast(err.message, true);
      }
      return;
    }

    const act = btn.dataset.act;
    if (!act) return;
    const tr = btn.closest("tr[data-device]");
    const deviceId = tr.dataset.device;
    const dev = devices.find((d) => d.device_id === deviceId);

    try {
      if (act === "rename") {
        const label = prompt("显示名：", dev.label ?? "");
        if (label === null) return;
        const owner = prompt("主人名（可留空）：", dev.owner_name ?? "") ?? "";
        await api(`/api/admin/devices/${deviceId}`, {
          method: "PATCH",
          body: JSON.stringify({ label, owner_name: owner }),
        });
        toast("已更新");
      } else if (act === "suspend") {
        const status = dev.status === "suspended" ? "active" : "suspended";
        await api(`/api/admin/devices/${deviceId}`, {
          method: "PATCH",
          body: JSON.stringify({ status }),
        });
        toast(status === "suspended" ? "已暂停" : "已恢复");
      } else if (act === "unbind") {
        if (!(await confirmAction(`解绑「${dev.label ?? short(deviceId)}」？设备将回到待认领，需重新配对码认领。`))) return;
        await api(`/api/admin/devices/${deviceId}/unbind`, { method: "POST" });
        toast("已解绑");
      } else if (act === "reset") {
        if (!(await confirmAction(`出厂重置「${dev.label ?? short(deviceId)}」？将删除该设备全部记录（含权限/分组），不可恢复。`))) return;
        if (!(await confirmAction("再次确认：真的要彻底删除？"))) return;
        await api(`/api/admin/devices/${deviceId}/reset`, { method: "POST" });
        toast("已删除");
      }
      root.removeEventListener("click", onAction);
      await renderDevices(root);
    } catch (err) {
      toast(err.message, true);
    }
  }
}
