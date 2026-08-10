import { describe, it, expect } from 'vitest'
import { readFileSync } from 'node:fs'
import { buildEnv, spawnWithExitCode } from './e2e-run.mjs'

// 读取 package.json 的 scripts：脚本契约判别刻意不依赖启动器模块，
// 保证在启动器缺失/脚本仍为 POSIX 语法时断言能单独判别（RED 清晰可回放）。
const pkg = JSON.parse(
  readFileSync(new URL('../package.json', import.meta.url), 'utf8'),
)

// Q-WEB-B14-001：Windows cmd/PowerShell 不识别 POSIX 的 `VAR=x cmd` 内联环境变量前缀，
// 会把 `CYPRESS_COVERAGE=1` 当作可执行命令名而非零退出。以下契约测试判别跨平台修复。
describe('e2e:run:coverage 脚本契约（跨平台）', () => {
  const script = pkg.scripts['e2e:run:coverage']
  const firstToken = script.trim().split(/\s+/)[0]

  it('命令首 token 必须是可执行命令，而非 POSIX 环境变量前缀', () => {
    // cmd 把首 token 当命令名；`CYPRESS_COVERAGE=1` 前缀在 Windows 会报“不是可执行命令”。
    expect(firstToken).not.toMatch(/^[A-Za-z_][A-Za-z0-9_]*=/)
  })

  it('经 Node 启动器（node scripts/e2e-run.mjs）进入 Cypress', () => {
    expect(firstToken).toBe('node')
    expect(script).toContain('e2e-run.mjs')
  })
})

describe('buildEnv：跨平台环境变量注入', () => {
  it('默认注入 CYPRESS_COVERAGE=1（vite istanbul 插件的启用判据）', () => {
    expect(buildEnv().CYPRESS_COVERAGE).toBe('1')
  })

  it('调用方 extraEnv 可覆盖默认注入值', () => {
    expect(buildEnv({ CYPRESS_COVERAGE: '0' }).CYPRESS_COVERAGE).toBe('0')
  })
})

describe('spawnWithExitCode：非 POSIX 语义下运行 + 退出码透传', () => {
  it('子进程能读到注入的环境变量（无 `VAR=x` 前缀，Windows/Ubuntu 同语义）', () => {
    const { status, stdout } = spawnWithExitCode(
      process.execPath,
      ['-e', 'console.log(process.env.CYPRESS_COVERAGE)'],
      {},
      'pipe',
    )
    expect(status).toBe(0)
    expect(stdout.trim()).toBe('1')
  })

  it('原样透传子进程退出码（Cypress 用例失败时应非零返回给 pnpm/CI）', () => {
    const { status } = spawnWithExitCode(
      process.execPath,
      ['-e', 'process.exit(7)'],
    )
    expect(status).toBe(7)
  })
})
