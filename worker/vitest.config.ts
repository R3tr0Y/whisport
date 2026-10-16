import { defineWorkersConfig } from "@cloudflare/vitest-pool-workers/config";

export default defineWorkersConfig({
  test: {
    include: ["tests/**/*.test.ts"],
    setupFiles: ["tests/setup.ts"],
    poolOptions: {
      workers: {
        wrangler: { configPath: "./wrangler.jsonc" },
        // PresenceDO（SQLite 后端）与 isolatedStorage 存在已知兼容问题（.sqlite-shm 断言），
        // 关闭按用例隔离；测试文件内自行管理状态，文件间仍相互隔离。
        isolatedStorage: false,
        miniflare: {
          compatibilityDate: "2025-09-01",
        },
      },
    },
  },
});
