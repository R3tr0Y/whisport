import schema from "../migrations/0001_init.sql?raw";
import { env as cfEnv } from "cloudflare:test";
import type { Env } from "../core/env";

/** 极简 SQL 语句切分：识别单引号字符串与 -- 注释，按 ; 切分（workerd 的 exec 对注释支持不佳） */
function splitSql(sql: string): string[] {
  const stmts: string[] = [];
  let cur = "";
  let inStr = false;
  for (let i = 0; i < sql.length; i++) {
    const c = sql[i]!;
    if (inStr) {
      cur += c;
      if (c === "'" && sql[i + 1] === "'") {
        cur += "'";
        i++;
      } else if (c === "'") {
        inStr = false;
      }
      continue;
    }
    if (c === "'") {
      inStr = true;
      cur += c;
      continue;
    }
    if (c === "-" && sql[i + 1] === "-") {
      while (i < sql.length && sql[i] !== "\n") i++;
      continue; // 丢弃注释（含换行外的内容；换行由下一轮保留语句内空白结构）
    }
    if (c === ";") {
      if (cur.trim()) stmts.push(cur.trim());
      cur = "";
      continue;
    }
    cur += c;
  }
  if (cur.trim()) stmts.push(cur.trim());
  return stmts;
}

// 每个 test file 独立隔离存储，setup 每文件执行一次
const e = cfEnv as unknown as Partial<Env>;
if (!e.DB) {
  console.warn("SETUP: env.DB missing; keys =", Object.keys(cfEnv ?? {}).join(","));
} else {
  const stmts = splitSql(schema);
  await e.DB.batch(stmts.map((s) => e.DB!.prepare(s)));
}
