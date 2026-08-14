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
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { dirname, join } from 'node:path'
import { pathToFileURL } from 'node:url'

const require = createRequire(import.meta.url)

/** cypress CLI 的 JS 入口：经其 package.json 的 bin 字段定位，兼容 pnpm 虚拟存储布局 */
const CYPRESS_CLI = (() => {
  const pkgPath = require.resolve('cypress/package.json')
  const binRel = require(pkgPath).bin.cypress
  return join(dirname(pkgPath), binRel)
})()

/** 注入 Cypress 进程的环境变量（单一来源；vite istanbul 插件以 CYPRESS_COVERAGE==='1' 为启用判据） */
export const COVERAGE_ENV = { CYPRESS_COVERAGE: '1' }

/** 组装子进程环境：基线 env + 注入变量 + 调用方额外 env（后者优先级最高） */
export function buildEnv(extraEnv = {}) {
  return { ...process.env, ...COVERAGE_ENV, ...extraEnv }
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

/** 运行 Cypress（node 直接加载其 JS 入口），原样透传退出码 */
export function runE2E(extraEnv = {}, stdio = 'inherit') {
  return spawnWithExitCode(process.execPath, [CYPRESS_CLI, 'run'], extraEnv, stdio)
}

// CLI 入口：`node scripts/e2e-run.mjs`；把 Cypress 退出码透传给 pnpm/CI，失败可判别
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exitCode = runE2E().status
}
