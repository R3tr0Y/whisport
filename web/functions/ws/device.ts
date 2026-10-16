/**
 * /ws/device → PresenceDO（WSS 升级透传）
 * 注意：路径含 [[ ]] 的 catch-all 只挂 /api；WS 端点单列避免歧义。
 */
import type { Env } from "../../core/env";

export const onRequest: PagesFunction<Env> = async ({ request, env }) => {
  const stub = env.PRESENCE.get(env.PRESENCE.idFromName("global"));
  return stub.fetch(new Request("https://presence/ws", request));
};
