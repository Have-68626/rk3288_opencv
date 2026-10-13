// describeReasonCode 纯函数单测（导入真实 health.ts 实现）。
//
// 为什么单独钉这个函数：上层 DegradedBanner 用它把 health.reasonCode 翻成中文文案。
// 判据三条必须同时成立，否则降级横幅会出错：
//   ① 7 个固定码各自映射到**确定的中文**（写错一个字即误导用户）；
//   ② 未知码**原样回显**（服务端将来新增码时既不臆造文案、也不静默吞成空白）；
//   ③ 空 / undefined 的**实测**行为（见实现 `if (!code) return undefined`）。
//
// 判别力说明：
//   · 删掉实现里的未知码分支（`?? code`）⇒ 第 ② 组与"恰为这 7 个码"那条立即变红；
//   · 改错任一已知码的文案 ⇒ 第 ① 组对应那条变红。
import { describe, it, expect } from 'vitest'

import { describeReasonCode } from '../health'

// 已知码域 = 加速契约的 7 个固定码（见 health.ts 的 REASON_CODE_TEXT 与 openapi.yaml /api/v1/health）。
// 期望值逐字复述实现里的中文，作为独立的"契约复述"（若实现文案被误改，这里即红）。
const KNOWN_CODES: ReadonlyArray<readonly [string, string]> = [
  ['ok', '无降级'],
  ['build_disabled', '构建期未启用该能力'],
  ['unsupported_platform', '当前平台不支持'],
  ['missing_dependency', '缺少依赖库'],
  ['missing_model', '缺少模型文件'],
  ['runtime_init_failed', '运行时初始化失败'],
  ['unsupported_input', '输入不受支持'],
]

describe('describeReasonCode：7 个固定码 → 中文（逐码）', () => {
  for (const [code, text] of KNOWN_CODES) {
    it(`已知码 ${code} → ${text}`, () => {
      expect(describeReasonCode(code)).toBe(text)
    })
  }

  it('映射表恰为这 7 个码：每个都被翻译成"非原码"的中文', () => {
    // 判别点：若某个码漏配（落到未知分支）⇒ got === code，下面 not.toBe(code) 即红。
    const seen = new Set<string>()
    for (const [code, text] of KNOWN_CODES) {
      const got = describeReasonCode(code)
      expect(got, `${code} 应被翻译`).toBe(text)
      expect(got, `${code} 不应原样回显（漏配时会退化为原码）`).not.toBe(code)
      seen.add(code)
    }
    // 顺带钉住"没有重复码"（重复会让上面逐码用例覆盖不全）。
    expect(seen.size).toBe(KNOWN_CODES.length)
  })
})

describe('describeReasonCode：未知码原样回显', () => {
  it('完全未知的码 → 回显自身（不臆造文案、不吞成空白）', () => {
    expect(describeReasonCode('brand_new_code')).toBe('brand_new_code')
  })

  it('与已知码前缀相似但非精确匹配 → 仍回显自身（判据是精确键，不是前缀/包含）', () => {
    expect(describeReasonCode('mpp_unsupported_platform')).toBe('mpp_unsupported_platform')
    expect(describeReasonCode('ok!')).toBe('ok!')
  })

  // 判别点：把实现改成 `return REASON_CODE_TEXT[code]`（删掉 `?? code`）⇒ 本组两条立即变红。
})

describe('describeReasonCode：空 / undefined 的实测行为', () => {
  // 按实现 `if (!code) return undefined`：falsy 短路 ⇒ 一律返回 undefined（不返回空串）。
  it('undefined ⇒ undefined', () => {
    expect(describeReasonCode(undefined)).toBeUndefined()
  })

  it('省略实参 ⇒ undefined', () => {
    expect(describeReasonCode()).toBeUndefined()
  })

  it('空串 ⇒ undefined（falsy 短路，而非返回空串）', () => {
    expect(describeReasonCode('')).toBeUndefined()
  })
})
