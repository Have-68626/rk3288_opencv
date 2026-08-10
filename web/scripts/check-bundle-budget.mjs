// bundle-budget 测试（Q-CQ-B08-011）
// 目的：构建后独立步骤，断言 Vite 入口主 chunk（webroot/assets/index-*.js）
// 的 raw 字节（minified 未压缩）不超过批准预算。
// 预算已由用户批准：600KB raw（614400 字节）。
// 用法：pnpm -C web build && node scripts/check-bundle-budget.mjs（或 pnpm -C web bundle:check）
import { readdirSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const BUDGET_KB = 600
const BUDGET_BYTES = BUDGET_KB * 1024

// 构建产物输出到仓库根 src/win/app/webroot（见 vite.config.ts build.outDir），
// 本脚本位于 web/scripts/，需上溯两级到仓库根再进入 src/win/app/webroot/assets。
const assetsDir = fileURLToPath(new URL('../../src/win/app/webroot/assets', import.meta.url))

let entries
try {
  entries = readdirSync(assetsDir).filter((f) => /^index-.*\.js$/.test(f))
} catch {
  console.error(`[bundle-budget] FAIL: 找不到构建产物目录 ${assetsDir}（请先执行 pnpm build）`)
  process.exit(1)
}

if (entries.length !== 1) {
  console.error(`[bundle-budget] FAIL: 期望恰好 1 个入口主 chunk（index-*.js），实际 ${entries.length} 个: ${entries.join(', ')}`)
  process.exit(1)
}

const entry = entries[0]
const size = statSync(`${assetsDir}/${entry}`).size
const sizeKB = (size / 1024).toFixed(1)

if (size > BUDGET_BYTES) {
  console.error(
    `[bundle-budget] FAIL: 主 chunk ${entry} = ${size} 字节（${sizeKB} KB），超出批准预算 ${BUDGET_KB} KB raw。` +
      `需要进一步拆包（路由懒加载 / vendor 分离）。`,
  )
  process.exit(1)
}

console.log(`[bundle-budget] PASS: 主 chunk ${entry} = ${size} 字节（${sizeKB} KB）≤ 批准预算 ${BUDGET_KB} KB raw`)
