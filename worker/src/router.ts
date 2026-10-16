import type { Env } from "./env";
import { ApiError } from "./util";

export interface Ctx {
  req: Request;
  env: Env;
  url: URL;
  params: Record<string, string>;
}

type Handler = (ctx: Ctx) => Promise<Response> | Response;

interface Route {
  method: string;
  segs: string[];
  handler: Handler;
}

/** 极简路径路由：支持 :param 段 */
export class Router {
  private routes: Route[] = [];

  add(method: string, path: string, handler: Handler): this {
    this.routes.push({ method, segs: path.split("/").filter(Boolean), handler });
    return this;
  }

  get(path: string, h: Handler) { return this.add("GET", path, h); }
  post(path: string, h: Handler) { return this.add("POST", path, h); }
  patch(path: string, h: Handler) { return this.add("PATCH", path, h); }
  put(path: string, h: Handler) { return this.add("PUT", path, h); }
  delete(path: string, h: Handler) { return this.add("DELETE", path, h); }

  async handle(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const usegs = url.pathname.split("/").filter(Boolean);
    for (const r of this.routes) {
      if (r.method !== req.method) continue;
      if (r.segs.length !== usegs.length) continue;
      const params: Record<string, string> = {};
      let ok = true;
      for (let i = 0; i < r.segs.length; i++) {
        const seg = r.segs[i]!;
        if (seg.startsWith(":")) {
          params[seg.slice(1)] = decodeURIComponent(usegs[i]!);
        } else if (seg !== usegs[i]) {
          ok = false;
          break;
        }
      }
      if (ok) return r.handler({ req, env, url, params });
    }
    throw new ApiError(404, "not_found", "no such route");
  }
}
