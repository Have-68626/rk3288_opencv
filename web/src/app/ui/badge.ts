// 顶栏连接徽标的判据（从 MainLayout 抽出，**刻意不依赖 antd / react-router**）。
// ★ 2026-09-28 新增（外部审查 [P2]「Web 后端断线后仍可能显示『已连接』」+ 独立复核）：
//   徽标此前**只**由 `serverSettings.status` 派生，而该状态只在「挂载时一次 + 用户手动刷新」时更新；
//   `GET /api/v1/health` 的周期检查结果只活在 `DegradedBanner` 的**局部** state 里
//   ⇒ 后端断线后徽标会**无限期**保持「已连接」。而 `docs/windows-web-spa/feature_parity.md` 明写
//   顶栏状态区就是健康检查的入口 ⇒ 这是**承诺与实现的错配**，不只是口味问题。
//
// 为什么抽成独立模块：这样这条判据能用**纯函数单测**覆盖，既不必为它搭 MainLayout 的渲染架子
// （Layout/Menu/react-router 全套），也不会让测试在 node 环境里因为 import antd 而失败。

export type ServerStatus = 'idle' | 'loading' | 'ready' | 'error'

export function statusToBadge(status: ServerStatus) {
  if (status === 'ready') return { status: 'success' as const, text: '已连接' }
  if (status === 'loading') return { status: 'processing' as const, text: '请求中' }
  if (status === 'error') return { status: 'error' as const, text: '异常' }
  return { status: 'default' as const, text: '未加载' }
}

// 徽标 = **健康检查优先**：最近一次健康检查失败 ⇒ 「未连接」；成功或**尚无判定**（`undefined`）
// ⇒ 沿用 settings 派生的状态（保持既有行为不变 —— `undefined` 是启动初期的常态，不得据此报警）。
export function badgeFor(serverStatus: ServerStatus, healthProbe: boolean | undefined) {
  if (healthProbe === false) return { status: 'error' as const, text: '未连接' }
  return statusToBadge(serverStatus)
}
