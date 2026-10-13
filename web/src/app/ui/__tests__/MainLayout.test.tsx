// MainLayout 顶栏徽标**接线**渲染测试。
//
// 审计点名的缺口（S4 登记）：徽标判据虽已抽为纯函数 `badgeFor` 并有 `badge.test.ts`，
// 但那 3 条只覆盖**纯函数本身**；真正决定行为的接线 —— `MainLayout` 是否真的把 store 里的
// `healthProbe` 连同 `serverSettings.status` 一起喂给 `badgeFor`、并把返回值渲染到顶栏 ——
// 没有任何用例。`App.routes.test.tsx` 把 `MainLayout` **整体 mock 掉**（只让它渲染 `<Outlet/>`），
// 因此接线写错（参数写反、忘了取 `healthProbe`、取了却没用）不会被任何测试变红。
//
// 被钉住的语义（见 MainLayout.tsx 的 `const badge = badgeFor(serverSettings.status, healthProbe)`）：
//   · `healthProbe === false`（最近一次健康检查失败）⇒ 顶栏徽标为「未连接」（**优先于** status）；
//   · `healthProbe === undefined`（尚无判定）⇒ 回落为按 `status` 派生（= 旧行为，启动初期常态）；
//   · `healthProbe === true` ⇒ 回落为按 `status` 派生。
//
// 覆盖边界（如实登记）：本文件既覆盖「单次挂载时的接线」（①②③④⑤⑥），也覆盖
// 「已有实例上 store 的 `healthProbe` 变化 → 重渲染 → 徽标更新」（⑦，全程不 unmount、不重建 root）。
// 但两者都经**被 mock 的** `useAppStore` 驱动：⑦ 走的是 `useSyncExternalStore` 订阅通道，
// 钉住的是「store 值变化 ⇒ 消费者重渲染并重算徽标」这一环；**仍无覆盖**的是
// `AppStoreProvider` 里 `useContext` 的真实 context 订阅链路（谁把 `setHealthProbe` 接进 store、
// `DegradedBanner` 回报 `false` 的实际接线）—— 那属于 provider 侧，不在本文件范围。
//
// 环境：node（本仓未装 jsdom / @testing-library）。沿用 DegradedBanner.test.tsx 与
// App.routes.test.tsx 既有的「最小 DOM + react-dom/client createRoot + act + 模块整体 mock」范式；
// 与 DegradedBanner 的差别在于 MainLayout 额外依赖 react-router-dom，故一并 mock。
// antd 以透传组件替换（Layout/Space/Typography/Button 透传 children，Menu 渲染 null），
// 顶栏 `<Badge>` 挂探针记录每次渲染的 props —— 徽标显示什么即由该探针断言。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'

import type { ServerSettingsState } from '../../state/AppStore'

// 探针 + 可变 store 持有者：vi.hoisted 使这些 spy 可在 vi.mock 工厂（被提升到文件顶部）里引用。
const h = vi.hoisted(() => {
  // 订阅通道：模拟「store 值变化驱动消费者重渲染」。变更时通知监听者，
  // 由被 mock 的 useAppStore（内部 useSyncExternalStore）把通知转成 React 重渲染 ——
  // 是**重渲染**而非重新挂载，正是本轮要钉住的那一环。
  const listeners = new Set<() => void>()
  const storeBox = { current: null as unknown }
  return {
    // 顶栏 <Badge> 每次渲染的 props 记录（末次即最终显示内容）。
    badge: vi.fn(),
    // 被测组件从 useAppStore() 读到的对象；各用例在挂载前写值。
    storeBox,
    subscribe: (cb: () => void) => {
      listeners.add(cb)
      return () => {
        listeners.delete(cb)
      }
    },
    getSnapshot: () => storeBox.current,
    // 替换 store 值（新引用，令 getSnapshot 结果变化）并通知订阅者。
    setStore: (next: unknown) => {
      storeBox.current = next
      for (const cb of listeners) cb()
    },
  }
})

// MainLayout 只消费 serverSettings / refreshServerSettings / healthProbe。
// 这里替换 useAppStore：不是「每次调用重读一个盒子的静态取值」，而是经 useSyncExternalStore
// 把 store 变更**真正接到** React 的订阅-重渲染链路上（见上方 h 的说明）。
vi.mock('../../state/AppStore', async () => {
  const { useSyncExternalStore } = await import('react')
  return {
    useAppStore: () => useSyncExternalStore(h.subscribe, h.getSnapshot, h.getSnapshot),
  }
})

// DegradedBanner 另有既有单测；本文件聚焦 MainLayout 的**消费端**接线，
// 把它替换为空实现（否则会连带触发 health 轮询与另一套 mock 需求）。
vi.mock('../DegradedBanner', () => ({ DegradedBanner: () => null }))

// MainLayout 用 useNavigate/useLocation/Outlet；此处无需真实 Router 上下文。
vi.mock('react-router-dom', () => ({
  useNavigate: () => () => {},
  useLocation: () => ({ pathname: '/home' }),
  Outlet: () => null,
}))

vi.mock('@ant-design/icons', () => ({
  AppstoreOutlined: () => null,
  ReloadOutlined: () => null,
  SettingOutlined: () => null,
  TeamOutlined: () => null,
  VideoCameraOutlined: () => null,
}))

// antd 重组件在无 jsdom 环境不可渲染：透传 children / 空实现。
// <Badge> 被渲染的 props = 顶栏徽标内容，故在 Badge 上挂探针。
vi.mock('antd', () => {
  const passthrough = ({ children }: { children?: unknown }) => children
  const Layout = Object.assign(passthrough, {
    Header: passthrough,
    Sider: passthrough,
    Content: passthrough,
  })
  return {
    Badge: (props: unknown) => {
      h.badge(props)
      return null
    },
    Button: passthrough,
    Menu: () => null,
    Space: passthrough,
    Layout,
    Typography: { Title: passthrough, Text: passthrough },
  }
})

// ⚠️ import 组件必须在 vi.mock 声明之后（vi.mock 会被提升，此处仅为可读性）。
import { MainLayout } from '../MainLayout'

// store 夹具：默认提供三字段，其余字段不受 MainLayout 消费。
function storeWith(serverSettings: ServerSettingsState, healthProbe: boolean | undefined) {
  return {
    serverSettings,
    refreshServerSettings: () => Promise.resolve(true),
    healthProbe,
  }
}

const ready: ServerSettingsState = {
  status: 'ready',
  data: { } as unknown as Extract<ServerSettingsState, { status: 'ready' }>['data'],
}
const errored: ServerSettingsState = {
  status: 'error',
  error: { code: 'x', message: '后端不可达' } as unknown as Extract<
    ServerSettingsState,
    { status: 'error' }
  >['error'],
}

// node 环境无 DOM：提供 createRoot 所需的最小容器/全局（不引入 jsdom 依赖，沿用既有范式）。
function installMinimalDom() {
  const noop = () => {}
  const textNode = () => ({
    nodeType: 3,
    textContent: '',
    appendChild: noop,
    removeChild: noop,
    setAttribute: noop,
    addEventListener: noop,
    removeEventListener: noop,
  })
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
    // 透传组件会渲染字符串子节点（如「刷新后端设置」），react-dom 需据此建文本节点。
    createTextNode: textNode,
    createComment: textNode,
  }
  const g = globalThis as unknown as Record<string, unknown>
  g.document = documentObj
  g.IS_REACT_ACT_ENVIRONMENT = true
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

// 写 store 后真实渲染 MainLayout，返回 root 与「顶栏 Badge 末次 props」读取器。
// 读取器**每次调用都取最新一次**（而非挂载时快照），以便重渲染用例断言变化后的徽标。
async function mountLayout(serverSettings: ServerSettingsState, healthProbe: boolean | undefined) {
  h.storeBox.current = storeWith(serverSettings, healthProbe)
  const env = installMinimalDom()
  const root = createRoot(env.makeContainer() as unknown as Element)
  await act(async () => {
    root.render(<MainLayout />)
  })
  await act(async () => {})
  return {
    root,
    badge: () =>
      h.badge.mock.calls.at(-1)?.[0] as { status?: unknown; text?: unknown } | undefined,
  }
}

let root: Root | null = null

beforeEach(() => {
  h.badge.mockReset()
  h.storeBox.current = null
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

describe('MainLayout 顶栏徽标接线（healthProbe 真的被喂给 badgeFor 并显示出来）', () => {
  it('① status=ready 且 healthProbe=false ⇒ 徽标「未连接」（healthProbe 生效，压过 status）', async () => {
    // 这是判别力核心：若接线把 healthProbe 漏掉（例如写死 undefined）或两个实参对调，
    // ready 会被派生为「已连接」，本条立即 RED。
    const m = await mountLayout(ready, false)
    root = m.root
    expect(m.badge(), 'healthProbe=false 时徽标必须降级为「未连接」').toEqual({
      status: 'error',
      text: '未连接',
    })
  })

  it('② status=error 且 healthProbe=false ⇒ 仍「未连接」（健康判定优先）', async () => {
    const m = await mountLayout(errored, false)
    root = m.root
    expect(m.badge()).toEqual({ status: 'error', text: '未连接' })
  })

  it('③ healthProbe=undefined ⇒ 回落为按 status 派生（等价旧行为，不干涉启动初期）', async () => {
    const m = await mountLayout(ready, undefined)
    root = m.root
    expect(m.badge(), 'undefined 不得改变既有 status 派生行为').toEqual({
      status: 'success',
      text: '已连接',
    })
  })

  it('④ healthProbe=undefined 且 status=error ⇒ 派生为「异常」（回落路径覆盖另一状态）', async () => {
    const m = await mountLayout(errored, undefined)
    root = m.root
    expect(m.badge()).toEqual({ status: 'error', text: '异常' })
  })

  it('⑤ healthProbe=true 且 status=ready ⇒「已连接」（健康成功回落 status）', async () => {
    const m = await mountLayout(ready, true)
    root = m.root
    expect(m.badge()).toEqual({ status: 'success', text: '已连接' })
  })

  it('⑥ healthProbe=true 但 status=error ⇒ 回落为「异常」（健康成功也不掩盖 settings 失败）', async () => {
    const m = await mountLayout(errored, true)
    root = m.root
    expect(m.badge()).toEqual({ status: 'error', text: '异常' })
  })
})

// 重渲染（**非重新挂载**）用例：本批修复要解决的是「后端断线后徽标无限期显示已连接」，
// 其成立前提是 store 里 `healthProbe` 变化会驱动已在挂载的 MainLayout 重渲染并重算徽标。
// 上面 6 条每条只挂载一次、且只读挂载时快照，钉不到「变化 → 重渲染 → 徽标更新」这一环。
// 这里通过 store 的订阅通道（h.setStore）触发重渲染，root 全程不 unmount、不重建。
describe('MainLayout 顶栏徽标随 healthProbe 变化重渲染（非重新挂载）', () => {
  it('⑦ healthProbe 由 true 变 false 再变回 true ⇒ 徽标「已连接」→「未连接」→「已连接」', async () => {
    const m = await mountLayout(ready, true)
    root = m.root
    expect(m.badge(), '初始 healthProbe=true 且 ready ⇒ 已连接').toEqual({
      status: 'success',
      text: '已连接',
    })

    // 模拟 DegradedBanner 探到断线后回报 false：仅改 store，不重新挂载。
    await act(async () => {
      h.setStore(storeWith(ready, false))
    })
    expect(m.badge(), 'healthProbe 变 false 后徽标必须重渲染为「未连接」').toEqual({
      status: 'error',
      text: '未连接',
    })

    // 模拟健康恢复：再次改 store，仍不重新挂载。
    await act(async () => {
      h.setStore(storeWith(ready, true))
    })
    expect(m.badge(), 'healthProbe 变回 true 后徽标必须回到「已连接」').toEqual({
      status: 'success',
      text: '已连接',
    })
  })
})
