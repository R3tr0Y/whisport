import { api, esc, fmtTime, toast, confirmAction } from "../api.js";

export async function renderGroups(root) {
  const [{ items: groups }, { items: devices }] = await Promise.all([
    api("/api/admin/groups"),
    api("/api/admin/devices"),
  ]);

  root.innerHTML = `
    <div class="section-title"><h2>分组</h2></div>
    <div class="card">
      <div class="row">
        <input id="g-name" placeholder="新分组名称" maxlength="64" style="flex:1">
        <button class="primary" id="g-create">创建分组</button>
      </div>
    </div>
    ${
      groups.length === 0
        ? '<div class="card"><p class="muted">暂无分组。分组用于公告可见性与广播频道成员（M2/M3）。</p></div>'
        : `<div id="groups-wrap">${groups
            .map(
              (g) => `
      <div class="card" data-group="${esc(g.group_id)}">
        <div class="section-title">
          <h3>${esc(g.name)} <span class="muted mono">(${esc(g.group_id)})</span></h3>
          <div class="actions">
            <button class="mini danger" data-gdel="${esc(g.group_id)}">删除分组</button>
          </div>
        </div>
        <p class="muted">创建于 ${fmtTime(g.created_at)} · 成员 ${g.member_count} 个</p>
        <div>
          ${(g.members ?? [])
            .map(
              (m) => `
          <span class="member-chip">${esc(m.label)}${m.status !== "active" ? `<span class="muted">(${esc(m.status)})</span>` : ""}
            <button data-gmrm="${esc(g.group_id)}|${esc(m.device_id)}" title="移除">✕</button>
          </span>`,
            )
            .join("") || '<span class="muted">暂无成员</span>'}
        </div>
        <div class="row" style="margin-top:10px">
          <select class="gm-add">
            <option value="">选择设备加入…</option>
            ${devices
              .filter((d) => !(g.members ?? []).some((m) => m.device_id === d.device_id))
              .map((d) => `<option value="${esc(d.device_id)}">${esc(d.label ?? d.device_id.slice(0, 8))} (${esc(d.status)})</option>`)
              .join("")}
          </select>
          <button class="mini" data-gmadd="${esc(g.group_id)}">加入</button>
        </div>
      </div>`,
            )
            .join("")}</div>`
    }`;

  root.querySelector("#g-create").addEventListener("click", async () => {
    const name = root.querySelector("#g-name").value.trim();
    if (!name) return toast("请输入分组名称", true);
    try {
      await api("/api/admin/groups", { method: "POST", body: JSON.stringify({ name }) });
      toast("分组已创建");
      await renderGroups(root);
    } catch (e) {
      toast(e.message, true);
    }
  });

  // 监听挂在本次渲染新建的容器上（旧元素销毁即监听器销毁，避免叠加）
  root.querySelector("#groups-wrap")?.addEventListener("click", async (e) => {
    const btn = e.target.closest("button");
    if (!btn) return;

    if (btn.dataset.gdel) {
      if (!(await confirmAction("删除该分组？成员关系将一并移除。"))) return;
      try {
        await api(`/api/admin/groups/${btn.dataset.gdel}`, { method: "DELETE" });
        toast("已删除");
        await renderGroups(root);
      } catch (err) {
        toast(err.message, true);
      }
      return;
    }

    if (btn.dataset.gmrm) {
      const [gid, did] = btn.dataset.gmrm.split("|");
      try {
        await api(`/api/admin/groups/${gid}/members/${did}`, { method: "DELETE" });
        toast("已移除");
        await renderGroups(root);
      } catch (err) {
        toast(err.message, true);
      }
      return;
    }

    if (btn.dataset.gmadd) {
      const card = btn.closest("[data-group]");
      const did = card.querySelector(".gm-add").value;
      if (!did) return toast("请先选择设备", true);
      try {
        await api(`/api/admin/groups/${btn.dataset.gmadd}/members`, {
          method: "POST",
          body: JSON.stringify({ device_id: did }),
        });
        toast("已加入");
        await renderGroups(root);
      } catch (err) {
        toast(err.message, true);
      }
    }
  });
}
