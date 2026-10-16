/**
 * whisport-realtime — 伴生 Worker（不对外暴露）
 *
 * Pages Functions 不能定义 Durable Object 类（绑定必须指向独立 Worker），
 * 故 PresenceDO（M2 将加 RoomDO）由本 Worker 承载，Pages 经服务绑定访问。
 * 全部用户流量仍走 whisport.pages.dev，本 Worker 无公网路由。
 *
 * 实现代码在 web/core/presence.ts（单一事实源，两端共享）；
 * 普通 Worker 打包无目录限制，可跨包引用。
 */
export { PresenceDO } from "../../web/core/presence";

export default {
  async fetch(): Promise<Response> {
    // 本 Worker 不对公网服务；此入口仅为满足模块格式与内部探活
    return new Response("whisport-realtime (internal)", { status: 404 });
  },
};
