// 深度 diff：返回一个“最小 patch”，只包含发生变化的字段。
// 为什么需要它：
// - 后端 /api/v1/settings 的 PUT 支持“局部更新（深度合并）”；
// - schema additionalProperties=false，发送多余字段会被拒绝；
// - 因此我们尽量只发送变化项，降低冲突与校验失败概率。
export function diffPatch(oldValue: unknown, newValue: unknown): unknown | undefined {
  if (Object.is(oldValue, newValue)) return undefined

  if (
    oldValue &&
    newValue &&
    typeof oldValue === 'object' &&
    typeof newValue === 'object' &&
    !Array.isArray(oldValue) &&
    !Array.isArray(newValue)
  ) {
    const oldObj = oldValue as Record<string, unknown>
    const newObj = newValue as Record<string, unknown>
    const out: Record<string, unknown> = {}

    const keys = new Set([...Object.keys(oldObj), ...Object.keys(newObj)])
    for (const k of keys) {
      const d = diffPatch(oldObj[k], newObj[k])
      if (d !== undefined) out[k] = d
    }
    return Object.keys(out).length ? out : undefined
  }

  // 其他类型（包含数组）：只要不相等就全量替换该节点
  return newValue
}

