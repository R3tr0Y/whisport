import { defineWorkersConfig } from "@cloudflare/vitest-pool-workers/config";

export default defineWorkersConfig({
  test: {
    include: ["tests/**/*.test.ts"],
    setupFiles: ["tests/setup.ts"],
    poolOptions: {
      workers: {
        // 测试直接以 core 为入口（Pages functions 只是薄适配层），
        // 绑定全部内联声明，不依赖 wrangler 配置形态。
        main: "./core/index.ts",
        // PresenceDO（SQLite DO）与 isolatedStorage 有已知兼容问题；
        // 关闭按用例隔离，文件内自行管理状态，文件间仍相互隔离。
        isolatedStorage: false,
        miniflare: {
          compatibilityDate: "2025-09-01",
          bindings: {
            ADMIN_TOKEN: "dev-admin-token",
            SECRET_ENC_KEY:
              "11223344556677889900aabbccddeeff11223344556677889900aabbccddeeff",
          },
          d1Databases: { DB: "test-db" },
          kvNamespaces: { CACHE: "test-cache" },
          durableObjects: { PRESENCE: "PresenceDO" },
        },
      },
    },
  },
});
