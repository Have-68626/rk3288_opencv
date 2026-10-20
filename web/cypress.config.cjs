// 显式使用 CommonJS，避免 Cypress 13 的 TS 转译与项目 ESM 设置冲突。
const { defineConfig } = require('cypress')
const codeCoverageTask = require('@cypress/code-coverage/task')

module.exports = defineConfig({
  e2e: {
    // 优先取 CYPRESS_BASE_URL（CI 用 dev server 时显式对齐端口），本地默认 vite dev 5173
    baseUrl: process.env.CYPRESS_BASE_URL || 'http://127.0.0.1:5173',
    supportFile: 'cypress/support/e2e.ts',
    setupNodeEvents(on, config) {
      codeCoverageTask(on, config)
      on('before:run', () => console.log('[E2E] Cypress 测试运行开始'))
      on('before:spec', (spec) => console.log(`[E2E] 测试文件开始：${spec.relative}`))
      on('after:spec', (spec, results) => {
        const stats = results?.stats
        console.log(
          `[E2E] 测试文件结束：${spec.relative}（通过 ${stats?.passes ?? 0}，失败 ${stats?.failures ?? 0}，跳过 ${stats?.pending ?? 0}）`,
        )
      })
      on('task', {
        e2eProgress({ event, title }) {
          console.log(`[E2E] ${event}：${title}`)
          return null
        },
      })
      return config
    },
  },
})

