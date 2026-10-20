import { describe, it, expect, vi } from 'vitest'
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
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

  it('子进程不继承宿主自动切换交互 shell 的标记，宿主环境保持不变', () => {
    vi.stubEnv('CLAUDE_DESKTOP_USE_PWSH', '1')
    try {
      expect(buildEnv().CLAUDE_DESKTOP_USE_PWSH).toBeUndefined()
      expect(buildEnv({ CLAUDE_DESKTOP_USE_PWSH: '1' }).CLAUDE_DESKTOP_USE_PWSH).toBeUndefined()
      expect(process.env.CLAUDE_DESKTOP_USE_PWSH).toBe('1')
    } finally {
      vi.unstubAllEnvs()
    }
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

describe('spawnWithHeartbeat：长时间运行时定期报告状态', () => {
  it('子进程存活期间持续心跳，并透传退出码', async () => {
    const { spawnWithHeartbeat } = await import('./e2e-run.mjs')
    expect(typeof spawnWithHeartbeat).toBe('function')
    if (typeof spawnWithHeartbeat !== 'function') return

    const heartbeats = []
    const status = await spawnWithHeartbeat(
      process.execPath,
      ['-e', 'setTimeout(() => process.exit(7), 140)'],
      {},
      {
        heartbeatIntervalMs: 20,
        onHeartbeat: (message) => heartbeats.push(message),
      },
    )

    expect(status).toBe(7)
    expect(heartbeats.length).toBeGreaterThan(1)
    expect(heartbeats.every((message) => message.includes('仍在运行'))).toBe(true)
  })
})

describe('覆盖率运行隔离', () => {
  it('归档上轮计数，保留 Cypress 缓存；首次运行不需要旧数据', async () => {
    const { archivePreviousCoverage } = await import('./e2e-run.mjs')
    expect(typeof archivePreviousCoverage).toBe('function')
    if (typeof archivePreviousCoverage !== 'function') return
    const scratch = mkdtempSync(join(tmpdir(), 'rk-cypress-coverage-'))
    try {
      const project = join(scratch, 'web')
      const archive = join(scratch, 'archive')
      mkdirSync(join(project, '.nyc_output'), { recursive: true })
      mkdirSync(join(project, 'node_modules', '.cache', 'cypress-e2e-cache'), { recursive: true })
      const cache = join(project, 'node_modules', '.cache', 'cypress-e2e-cache', 'keep.txt')
      writeFileSync(cache, '保留缓存')
      expect(archivePreviousCoverage(project, archive)).toBeNull()
      const previous = join(project, '.nyc_output', 'out.json')
      writeFileSync(previous, '{"previous":1}')
      const saved = archivePreviousCoverage(project, archive)
      expect(existsSync(previous)).toBe(false)
      expect(readFileSync(saved, 'utf8')).toBe('{"previous":1}')
      expect(readFileSync(cache, 'utf8')).toBe('保留缓存')
      expect(archivePreviousCoverage(project, archive)).toBeNull()
      expect(readFileSync(saved, 'utf8')).toBe('{"previous":1}')
    } finally {
      rmSync(scratch, { recursive: true, force: true })
    }
  })
})
