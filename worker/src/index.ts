import type { Env } from "./env";
import { Router, type Ctx } from "./router";
import { errorResponse, json } from "./util";
import { runCleanup } from "./cron";
import * as admin from "./routes/admin";
import * as groups from "./routes/groups";
import * as devices from "./routes/devices";
import * as announcements from "./routes/announcements";

export { PresenceDO } from "./presence";

const router = new Router()
  // ---- 管理端（Bearer ADMIN_TOKEN）----
  .get("/api/admin/whoami", admin.whoami)
  .get("/api/admin/devices", admin.listDevices)
  .patch("/api/admin/devices/:id", admin.patchDevice)
  .post("/api/admin/devices/:id/unbind", admin.unbindDevice)
  .post("/api/admin/devices/:id/reset", admin.resetDevice)
  .post("/api/admin/pairing-codes", admin.createPairingCode)
  .get("/api/admin/pairing-codes", admin.listPairingCodes)
  .get("/api/admin/reactivations", admin.listReactivations)
  .post("/api/admin/reactivations/:id/approve", admin.approveReactivation)
  .post("/api/admin/reactivations/:id/deny", admin.denyReactivation)
  .get("/api/admin/announcements", announcements.adminList)
  .post("/api/admin/announcements", announcements.adminCreate)
  .delete("/api/admin/announcements/:id", announcements.adminDelete)
  .get("/api/admin/audit", admin.listAudit)
  // ---- 组管理（管理员）----
  .post("/api/admin/groups", groups.create)
  .get("/api/admin/groups", groups.list)
  .delete("/api/admin/groups/:id", groups.remove)
  .post("/api/admin/groups/:id/members", groups.addMember)
  .delete("/api/admin/groups/:id/members/:deviceId", groups.removeMember)
  // ---- 设备端 ----
  .post("/api/devices/claim", devices.claim)
  .post("/api/devices/reactivate", devices.reactivateRequest)
  .get("/api/devices/reactivate", devices.reactivatePoll)
  .get("/api/devices/me", devices.me)
  .patch("/api/devices/me", devices.patchMe)
  .get("/api/announcements", announcements.listForDevice)
  // ---- 实时通道（转交 PresenceDO）----
  .get("/ws/device", wsDevice);

/** GET /ws/device — 透传至 PresenceDO（WSS 升级） */
async function wsDevice(ctx: Ctx): Promise<Response> {
  const stub = ctx.env.PRESENCE.get(ctx.env.PRESENCE.idFromName("global"));
  return stub.fetch(new Request("https://presence/ws", ctx.req));
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    try {
      return await router.handle(req, env);
    } catch (e) {
      return errorResponse(e);
    }
  },

  async scheduled(_event: ScheduledController, env: Env, ctx: ExecutionContext): Promise<void> {
    ctx.waitUntil(
      runCleanup(env).then(
        (s) => console.log("cleanup:", JSON.stringify(s)),
        (e) => console.error("cleanup failed:", e),
      ),
    );
  },
};
