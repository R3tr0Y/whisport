export interface Env {
  /** D1 数据库（全部持久化数据） */
  DB: D1Database;
  /** KV：nonce 防重放、限速计数、会话票据（M3） */
  CACHE: KVNamespace;
  /** PresenceDO 命名空间（在线状态 + 控制信令） */
  PRESENCE: DurableObjectNamespace;
  /** 管理员令牌（生产环境用 wrangler secret 覆盖） */
  ADMIN_TOKEN: string;
  /** device_secret 加密密钥，64 hex 字符 = AES-256（生产环境用 wrangler secret 覆盖） */
  SECRET_ENC_KEY: string;
}
