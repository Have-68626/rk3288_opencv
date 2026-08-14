// bundle-budget 测试（Q-CQ-B08-011）
// 目的：构建后独立步骤，断言"首屏 eager 下载的 JS 总量"（index.html 静态引用的
// module script + modulepreload 共享 chunk）的 raw 字节（minified 未压缩）不超过预算。
//
// 口径演进：初版仅断言入口 index-*.js ≤ 600KB（用户批准口径）；路由拆包后入口降至
// ~144KB，但 AppStore 共享 chunk（antd/react 公共依赖，位于所有路由公共边界，无法
// 懒加载）使首屏 eager 总量约 620KB，略超原 600KB。用户决策（2026-08-10）：
// 不进一步压缩，放宽标准至 700KB——口径升级为"首屏 eager 总量"（更贴近真实负载），
// 预算含 ~80KB 余量，门禁继续兜底防回退。
//
// 用法：pnpm -C web build && node scripts/check-bundle-budget.mjs（或 pnpm -C web bundle:check）
import { readFileSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const BUDGET_KB = 700
const BUDGET_BYTES = BUDGET_KB * 1024

// 构建产物输出到仓库根 src/win/app/webroot（见 vite.config.ts build.outDir），
// 本脚本位于 web/scripts/，需上溯两级到仓库根再进入 src/win/app/webroot。
const webroot = fileURLToPath(new URL('../../src/win/app/webroot', import.meta.url))
const assetsDir = path.join(webroot, 'assets')

let indexHtml
try {
  indexHtml = readFileSync(path.join(webroot, 'index.html'), 'utf8')
} catch {
  console.error(`[bundle-budget] FAIL: 找不到 ${path.join(webroot, 'index.html')}（请先执行 pnpm build）`)
  process.exit(1)
}

// 入口：index.html 中 <script type=module> 引用的主 chunk，必须恰好 1 个。
const entryMatch = indexHtml.match(/<script type="module"[^>]*src="\.\/assets\/([^".]+\.js)"/)
if (!entryMatch) {
  console.error(`[bundle-budget] FAIL: index.html 缺少 <script type=module> 入口（构建产物异常）`)
  process.exit(1)
}

// 首屏 eager 口径：入口 script src + modulepreload link href 引用的全部 .js。
// 这两类是浏览器解析 HTML 后立即下载执行的 chunk；懒加载路由 chunk 不在 index.html
// 内，不计入首屏。
const refs = [...indexHtml.matchAll(/(?:src|href)="\.\/assets\/([^".]+\.js)"/g)].map((m) => m[1])
if (refs.length === 0) {
  console.error(`[bundle-budget] FAIL: index.html 未引用任何 assets/*.js`)
  process.exit(1)
}

let total = 0
const parts = []
for (const ref of refs) {
  const abs = path.join(assetsDir, ref)
  let size
  try {
    size = statSync(abs).size
  } catch {
    console.error(`[bundle-budget] FAIL: index.html 引用的 ${ref} 不在 assets/ 下（构建产物与 html 不同步）`)
    process.exit(1)
  }
  total += size
  parts.push(`${ref} = ${size} 字节`)
}

const totalKB = (total / 1024).toFixed(1)
if (total > BUDGET_BYTES) {
  console.error(
    `[bundle-budget] FAIL: 首屏 eager JS 总量 = ${total} 字节（${totalKB} KB），超出预算 ${BUDGET_KB} KB raw。` +
      `需要进一步拆包（共享 chunk 分离 / antd 按需引入）。明细：\n  ${parts.join('\n  ')}`,
  )
  process.exit(1)
}

console.log(`[bundle-budget] PASS: 首屏 eager JS 总量 = ${total} 字节（${totalKB} KB）≤ 预算 ${BUDGET_KB} KB raw`)
console.log(`  入口: ${entryMatch[1]}`)
for (const p of parts) console.log(`  ${p}`)
