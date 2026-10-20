#!/usr/bin/env node
/**
 * 跨平台 Cypress E2E 启动器（Q-WEB-B14-001）
 *
 * 背景：
 * - 旧脚本 `CYPRESS_COVERAGE=1 cypress run` 是 POSIX 内联环境变量语法；
 *   Windows cmd/PowerShell 不识别该前缀，会把 `CYPRESS_COVERAGE=1` 当作可执行命令名，
 *   报“不是可执行命令”并以非零码退出，Windows 上从未真正进入 Cypress。
 * - 本启动器改为：在 Node 中先通过 process.env 注入环境变量，再以非 shell 方式
 *   （shell:false）运行 cypress 的 JS 入口，并原样透传退出码。
 *   shell:false 保证不经过 shell（POSIX 前缀语法不再参与解析）；
 *   直接加载 cypress 的 JS 入口，绕开 Windows 下 `.CMD`/`.exe` 的命令名解析差异，
 *   因此 Windows 与 Ubuntu 行为一致。
 */
import { spawn, spawnSync } from 'node:child_process'
import { existsSync, mkdirSync, renameSync } from 'node:fs'
import { createRequire } from 'node:module'
import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

const require = createRequire(import.meta.url)

/** cypress CLI 的 JS 入口：经其 package.json 的 bin 字段定位，兼容 pnpm 虚拟存储布局 */
const CYPRESS_CLI = (() => {
  const pkgPath = require.resolve('cypress/package.json')
  const binRel = require(pkgPath).bin.cypress
  return join(dirname(pkgPath), binRel)
})()

/** 注入 Cypress 进程的环境变量（单一来源；vite istanbul 插件以 CYPRESS_COVERAGE==='1' 为启用判据） */
export const COVERAGE_ENV = { CYPRESS_COVERAGE: '1' }

/** 组装子进程环境；宿主的交互 shell 标记不属于 Cypress 运行环境 */
export function buildEnv(extraEnv = {}) {
  const env = { ...process.env, ...COVERAGE_ENV, ...extraEnv }
  // Windows profile 会据此转入交互 pwsh，卡住 Cypress 的系统信息查询；仅清理子进程副本。
  delete env.CLAUDE_DESKTOP_USE_PWSH
  return env
}

/** headless 插件会合并旧计数；归档后再运行，保留证据并避免跨轮假通过。 */
export function archivePreviousCoverage(projectDir, archiveDir) {
  const previous = join(projectDir, '.nyc_output', 'out.json')
  if (!existsSync(previous)) return null
  mkdirSync(archiveDir, { recursive: true })
  const archived = join(archiveDir, 'out.json')
  renameSync(previous, archived)
  return archived
}

/**
 * 以非 shell 方式运行 bin+args，注入环境变量并透传退出码。
 * shell:false 是关键：POSIX 的 `VAR=1` 前缀语法在 Windows 上不会被当作命令名执行。
 */
export function spawnWithExitCode(bin, args, extraEnv = {}, stdio = 'inherit') {
  const child = spawnSync(bin, args, {
    env: buildEnv(extraEnv),
    stdio,
    shell: false,
  })
  // status 为 null 表示子进程被信号终止/未能启动，统一归一为非零（1）
  return { status: child.status ?? 1, stdout: child.stdout?.toString() ?? '' }
}

/** 异步运行长任务并定期报告存活状态，保留终端实时输出与退出码 */
export function spawnWithHeartbeat(bin, args, extraEnv = {}, {
  heartbeatIntervalMs = 30_000,
  onHeartbeat = (message) => process.stdout.write(`${message}\n`),
} = {}) {
  return new Promise((resolve) => {
    const startedAt = Date.now()
    let settled = false
    let heartbeat

    const finish = (status) => {
      if (settled) return
      settled = true
      clearInterval(heartbeat)
      resolve(status ?? 1)
    }

    const child = spawn(bin, args, {
      env: buildEnv(extraEnv),
      stdio: 'inherit',
      shell: false,
    })

    heartbeat = setInterval(() => {
      const elapsedSeconds = Math.floor((Date.now() - startedAt) / 1000)
      onHeartbeat(`[E2E 心跳] 子进程仍在运行，已用时 ${elapsedSeconds} 秒`)
    }, heartbeatIntervalMs)

    child.once('error', (error) => {
      process.stderr.write(`[E2E] 无法启动 Cypress：${error.message}\n`)
      finish(1)
    })
    child.once('close', (status) => finish(status))
  })
}

/** 异步运行 Cypress（node 直接加载其 JS 入口），原样透传退出码 */
export function runE2E(extraEnv = {}, options = {}) {
  const projectDir = join(dirname(fileURLToPath(import.meta.url)), '..')
  const archived = archivePreviousCoverage(projectDir, join(projectDir, '..', 'cache', 'web-e2e', `${Date.now()}-${process.pid}`))
  if (archived) process.stdout.write(`[E2E] 上轮覆盖率已归档：${archived}\n`)
  return spawnWithHeartbeat(process.execPath, [CYPRESS_CLI, 'run'], extraEnv, options)
}

// CLI 入口：`node scripts/e2e-run.mjs`；把 Cypress 退出码透传给 pnpm/CI，失败可判别
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exitCode = await runE2E()
}
