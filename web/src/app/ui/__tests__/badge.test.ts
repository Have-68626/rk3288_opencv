import { describe, expect, it } from 'vitest'

import { badgeFor } from '../badge'

// ★ 2026-09-28 新增（外部审查 [P2]「Web 后端断线后仍可能显示『已连接』」的**判别用例**）。
// 修复前**不存在**这条派生：徽标只由 `serverSettings.status` 决定，而该状态只在挂载时一次 +
// 用户手动刷新时更新 ⇒ 断线后徽标无限期显示「已连接」（本条即针对该行为）。
// 纯函数测试（`../badge` 不 import antd/react-router）⇒ 不需要 DOM 或渲染架子。
describe('badgeFor：顶栏徽标 = 健康检查优先', () => {
  it('健康检查失败 ⇒ 「未连接」，即便 settings 仍是 ready（这正是「断线后仍显示已连接」的成因）', () => {
    expect(badgeFor('ready', false)).toEqual({ status: 'error', text: '未连接' })
    expect(badgeFor('loading', false)).toEqual({ status: 'error', text: '未连接' })
  })

  it('健康检查成功 ⇒ 回到 settings 派生的状态', () => {
    expect(badgeFor('ready', true)).toEqual({ status: 'success', text: '已连接' })
    expect(badgeFor('error', true)).toEqual({ status: 'error', text: '异常' })
  })

  it('尚无判定（undefined）⇒ 沿用既有行为、不干涉（启动初期是常态，不得据此报警）', () => {
    expect(badgeFor('ready', undefined)).toEqual({ status: 'success', text: '已连接' })
    expect(badgeFor('idle', undefined)).toEqual({ status: 'default', text: '未加载' })
    expect(badgeFor('loading', undefined)).toEqual({ status: 'processing', text: '请求中' })
    expect(badgeFor('error', undefined)).toEqual({ status: 'error', text: '异常' })
  })
})
