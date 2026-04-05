import { defineConfig } from 'cypress'
import codeCoverageTask from '@cypress/code-coverage/task'

export default defineConfig({
  e2e: {
    baseUrl: 'http://127.0.0.1:5173',
    supportFile: 'cypress/support/e2e.ts',
    setupNodeEvents(on, config) {
      codeCoverageTask(on, config)
      return config
    },
  },
})

