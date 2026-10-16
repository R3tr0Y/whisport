/**
 * /api/* catch-all → core 路由器
 * Pages 静态资源优先匹配；未命中静态的 /api 路径进入本函数。
 */
import type { Env } from "../../core/env";
import worker from "../../core/index";

export const onRequest: PagesFunction<Env> = (context) => {
  return worker.fetch(context.request, context.env);
};
