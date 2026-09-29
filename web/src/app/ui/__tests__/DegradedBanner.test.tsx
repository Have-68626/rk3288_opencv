// DegradedBanner 渲染门单测（真实渲染组件，不是只测判定函数）。
//
// 审计点名的缺陷形态：本批新增的 DegradedBanner 零单测，`App.routes.test.tsx` 把
// FacesPage 整体 mock 掉了 —— 若降级门被写错（**误用 configStatus 而非 status**），
// 没有任何测试会变红。本文件正是为堵这个洞。
//
// 被钉住的语义（见 DegradedBanner.tsx:80-81 的 `if (!data || data.status !== 'degraded') return null`）：
//   · `status`（**运行时装配**）为 degraded ⇒ 渲染横幅；
//   · `configStatus`（**配置装载**，是另一个量）即使为 degraded/rejected，只要 status 为 ok ⇒ **不渲染**。
//
// 环境：node（本仓未装 jsdom / @testing-library）。沿用 AppStore.race.test.tsx 与
// App.routes.test.tsx 既有的"最小 DOM + react-dom/client createRoot + act + antd 整体 mock"范式。
// DegradedBanner 的结构很浅（未降级/取数失败时直接 return null；降级时渲染一个 antd <Alert>），
// 该范式足以**真实执行组件的渲染分支** —— 故无需引入 jsdom，也无需为可测性改生产代码。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'

import type { HealthData } from '../../api/types'
import { defaultPrefs } from '../../state/prefs'

// 探针 + 假实现：vi.hoisted 使这些 spy 可在 vi.mock 工厂（被提升到文件顶部）里引用。
const h = vi.hoisted(() => ({
  getHealth: vi.fn(),
  alert: vi.fn(),
  messageError: vi.fn(),
  messageSuccess: vi.fn(),
  getModels: vi.fn(),
  // 2026-09-28 新增：组件现在会把「最近一次健康检查的判定」回报给 store（供顶栏徽标降级，
  // 见 AppStore 的 healthProbe）；这里用可断言的 spy。
  setHealthProbe: vi.fn(),
  // 可变的 prefs 持有者：供「偏好变化 ⇒ effect 重跑 ⇒ 两条 health 请求重叠」的竞态用例改写，
  // 其余用例保持 null（回落到 defaultPrefs）。
  prefsBox: { current: null as (typeof defaultPrefs) | null },
}))

// 只替换 getHealth，保留真实的 describeReasonCode（description 文案走真实码→中文映射）。
vi.mock('../../api/health', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../../api/health')>()
  return { ...actual, getHealth: h.getHealth }
})

// 组件虽会 import 这两个（重试路径用），但本文件不触发重试 —— mock 掉以免真发网络请求。
vi.mock('../../api/models', () => ({
  getModels: h.getModels,
  reloadModel: h.getModels,
}))

// 避免为取 prefs 去搭 AppStoreProvider：组件只用到 useAppStore().prefs 与 .setHealthProbe。
vi.mock('../../state/AppStore', () => ({
  useAppStore: () => ({
    prefs: h.prefsBox.current ?? defaultPrefs,
    setHealthProbe: h.setHealthProbe,
  }),
}))

vi.mock('@ant-design/icons', () => ({
  ReloadOutlined: () => null,
  WarningOutlined: () => null,
}))

// antd 重组件在无 DOM 环境不可渲染：mock 为探针/空实现。
// <Alert> 被渲染与否 = 横幅是否出现，故在 Alert 上挂探针记录每次渲染的 props。
vi.mock('antd', () => ({
  Alert: (props: { message?: unknown; description?: unknown }) => {
    h.alert(props)
    return null
  },
  Button: () => null,
  Space: () => null,
  Typography: { Text: () => null, Paragraph: () => null },
  message: {
    success: h.messageSuccess,
    error: h.messageError,
    warning: () => {},
    loading: () => () => {},
  },
}))

// ⚠️ import 组件必须在 vi.mock 声明之后（vi.mock 会被提升，此处仅为可读性）。
import { DegradedBanner } from '../DegradedBanner'

// node 环境无 DOM：提供 createRoot 所需的最小容器/全局（不引入 jsdom 依赖，沿用既有范式）。
function installMinimalDom() {
  const noop = () => {}
  const documentObj = {
    addEventListener: noop,
    removeEventListener: noop,
    createElement: () => {
      const el: Record<string, unknown> = {
        nodeType: 1,
        tagName: 'DIV',
        namespaceURI: 'http://www.w3.org/1999/xhtml',
        style: {},
        appendChild: noop,
        removeChild: noop,
        setAttribute: noop,
        addEventListener: noop,
        removeEventListener: noop,
      }
      return el
    },
  }
  const g = globalThis as unknown as Record<string, unknown>
  g.document = documentObj
  g.IS_REACT_ACT_ENVIRONMENT = true
  // 组件的 useEffect 用 window.setInterval —— 指向全局（Node 已有 setInterval）。
  g.window = globalThis
  // react-dom 的 getActiveElementDeep 会执行 element instanceof win.HTMLIFrameElement，补空类。
  g.HTMLIFrameElement = class HTMLIFrameElement {}
  return {
    makeContainer: () => ({
      nodeType: 1,
      tagName: 'DIV',
      namespaceURI: 'http://www.w3.org/1999/xhtml',
      ownerDocument: documentObj,
      addEventListener: noop,
      removeEventListener: noop,
      appendChild: noop,
      removeChild: noop,
      insertBefore: noop,
      removeAttribute: noop,
    }),
  }
}

function cleanupGlobals() {
  const g = globalThis as unknown as Record<string, unknown>
  for (const k of ['document', 'window', 'IS_REACT_ACT_ENVIRONMENT', 'HTMLIFrameElement']) {
    delete g[k]
  }
}

// 挂载组件并冲刷 effect 内 getHealth 的 resolve/reject 引发的 setState 重渲染。
async function mountBanner(): Promise<Root> {
  const env = installMinimalDom()
  const root = createRoot(env.makeContainer() as unknown as Element)
  await act(async () => {
    root.render(<DegradedBanner />)
  })
  await act(async () => {}) // 冲刷 loadHealth 的 promise 回调 + setState
  await act(async () => {})
  return root
}

// health 数据夹具：默认 configStatus/status 均 ok；各用例按需覆盖。
function healthData(overrides: Partial<HealthData>): HealthData {
  return {
    name: 'rk_wcfr',
    port: 8080,
    configStatus: 'ok',
    status: 'ok',
    availableFeatures: [],
    ...overrides,
  }
}

function okEnvelope(data: HealthData) {
  return { ok: true as const, data }
}

let root: Root | null = null

beforeEach(() => {
  h.getHealth.mockReset()
  h.alert.mockReset()
  h.messageError.mockReset()
  h.messageSuccess.mockReset()
  h.getModels.mockReset()
  h.setHealthProbe.mockReset()
  h.prefsBox.current = null
})

afterEach(async () => {
  if (root) {
    await act(async () => {
      root!.unmount()
    })
    root = null
  }
  cleanupGlobals()
})

describe('DegradedBanner 渲染门（Fake getHealth 驱动）', () => {
  it('① status==="degraded"（运行时装配降级；configStatus 仍为 ok）⇒ 渲染降级横幅', async () => {
    // 关键夹具：configStatus 保持 'ok' —— 这样"误用 configStatus 当门"的回归会在本条变红。
    h.getHealth.mockResolvedValue(
      okEnvelope(
        healthData({
          status: 'degraded',
          configStatus: 'ok',
          reasonCode: 'missing_model',
          warning: '模型缺失',
          degradedSince: '2026-01-01 00:00:00',
        }),
      ),
    )
    root = await mountBanner()
    expect(h.alert, 'status=degraded 必须渲染横幅').toHaveBeenCalledTimes(1)
    // 文案确为"降级"横幅（钉住渲染的是对的那个 Alert，而非随便什么元素）。
    expect(h.alert.mock.calls[0][0]).toMatchObject({ message: expect.stringContaining('降级') })
    // 正常降级展示属安静提示，不得弹错误通知。
    expect(h.messageError, '正常降级展示不得弹错误通知').not.toHaveBeenCalled()
  })

  it('② status==="ok" ⇒ 不渲染（零占位）', async () => {
    h.getHealth.mockResolvedValue(okEnvelope(healthData({ status: 'ok', configStatus: 'ok' })))
    root = await mountBanner()
    expect(h.alert, '未降级不得渲染任何横幅').not.toHaveBeenCalled()
  })

  it('③ health 请求失败/超时 ⇒ 不渲染、不弹通知（安静降级，不刷屏）', async () => {
    h.getHealth.mockRejectedValue(new Error('network down / timeout'))
    root = await mountBanner()
    expect(h.alert, '取不到 health 不得渲染横幅').not.toHaveBeenCalled()
    expect(h.messageError, '轮询失败不得弹通知（避免刷屏报错）').not.toHaveBeenCalled()
    expect(h.messageSuccess, '失败路径不得弹成功通知').not.toHaveBeenCalled()
  })

  it('④ configStatus 为 degraded/rejected 但 status 为 ok ⇒ 仍不渲染（审计点名的"写错门"形态）', async () => {
    // 这是判别力核心：configStatus（配置装载层）与 status（运行时装配层）语义不同，
    // 只有后者决定横幅。若门被误改成读 configStatus，本条立即变红。
    for (const configStatus of ['degraded', 'rejected'] as const) {
      h.getHealth.mockReset()
      h.alert.mockReset()
      h.getHealth.mockResolvedValue(
        okEnvelope(healthData({ configStatus, status: 'ok', configWarning: '配置装载层告警' })),
      )
      const r = await mountBanner()
      expect(
        h.alert,
        `configStatus=${configStatus} 且 status=ok ⇒ 不得渲染（配置层与运行层是两个量）`,
      ).not.toHaveBeenCalled()
      await act(async () => {
        r.unmount()
      })
      cleanupGlobals()
    }
  })

  it('⑤ status==="starting" ⇒ 不渲染（只有 degraded 才报警）', async () => {
    h.getHealth.mockResolvedValue(okEnvelope(healthData({ status: 'starting', configStatus: 'ok' })))
    root = await mountBanner()
    expect(h.alert, 'starting 不是降级，不得渲染横幅').not.toHaveBeenCalled()
  })

  it('⑥ 信封 ok:false（HTTP 层失败）⇒ 不渲染（不把错误信封当降级数据）', async () => {
    h.getHealth.mockResolvedValue({ ok: false, data: undefined } as unknown as ReturnType<
      typeof okEnvelope
    >)
    root = await mountBanner()
    expect(h.alert, 'ok:false 信封不得渲染横幅').not.toHaveBeenCalled()
  })
})

// ── 2026-09-28 新增（外部审查 [P2] 两条 Web 发现的判别用例）──────────────────────────────
// 可控 promise：用于精确编排「先发出的请求**后**返回」。
function deferred<T>() {
  let resolve!: (v: T) => void
  let reject!: (e: unknown) => void
  const promise = new Promise<T>((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

describe('过期响应守卫与健康判定回报（2026-09-28 修复项）', () => {
  it('★ 过期响应不得覆盖较新的结果（代次守卫）—— 修复前本条必红', async () => {
    const r1 = deferred<ReturnType<typeof okEnvelope>>()
    const r2 = deferred<ReturnType<typeof okEnvelope>>()
    h.getHealth.mockReturnValueOnce(r1.promise).mockReturnValueOnce(r2.promise)
    h.prefsBox.current = { ...defaultPrefs, apiTimeoutMs: 1000 }

    root = await mountBanner() // 发出 R1（较早）

    // 改 prefs ⇒ loadHealth 换身份 ⇒ effect 重跑 ⇒ 发出 R2（较新）。
    // 这正是设置页的日常路径（该页是**逐键实时写** prefs）。
    h.prefsBox.current = { ...defaultPrefs, apiTimeoutMs: 2000 }
    await act(async () => {
      root!.render(<DegradedBanner />)
    })

    // 较新的 R2 先返回：ok ⇒ 不渲染横幅，且判定回报为「健康」。
    await act(async () => {
      r2.resolve(okEnvelope(healthData({ status: 'ok' })))
    })
    expect(h.alert).not.toHaveBeenCalled()
    expect(h.setHealthProbe).toHaveBeenCalledWith(true)

    // 较旧的 R1 后返回：degraded ⇒ 代次守卫必须**丢弃**它。
    // 修复前 `aliveRef` 会被新 effect 重新上膛为 true ⇒ 这里会点亮横幅 ⇒ 本条必红。
    await act(async () => {
      r1.resolve(
        okEnvelope(
          healthData({ status: 'degraded', reasonCode: 'missing_model', warning: '模型缺失' }),
        ),
      )
    })
    expect(
      h.alert,
      '过期的降级响应不得点亮横幅（修复前 aliveRef 会被新 effect 重新上膛 ⇒ 本条必红）',
    ).not.toHaveBeenCalled()
    expect(h.setHealthProbe).not.toHaveBeenCalledWith(false)
  })

  it('★ 健康检查失败必须回报给 store（顶栏徽标据此显示「未连接」）', async () => {
    h.getHealth.mockRejectedValueOnce(new Error('network down'))
    root = await mountBanner()
    // 修复前顶栏只由 serverSettings 驱动，断线后**无限期**显示「已连接」（见 AppStore 的 healthProbe 说明）。
    expect(h.setHealthProbe).toHaveBeenCalledWith(false)
  })
})
