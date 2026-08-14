import { defineConfig } from 'cypress'
import codeCoverageTask from '@cypress/code-coverage/task'

export default defineConfig({
  e2e: {
    // 优先取 CYPRESS_BASE_URL（CI 用 dev server 时显式对齐端口），本地默认 vite dev 5173
    baseUrl: process.env.CYPRESS_BASE_URL || 'http://127.0.0.1:5173',
    supportFile: 'cypress/support/e2e.ts',
    setupNodeEvents(on, config) {
      codeCoverageTask(on, config)
      return config
    },
  },
})

