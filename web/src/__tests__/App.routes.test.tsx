// SEC-11 / S-SEC-B10-002：App.tsx 路由单测 + open-redirect 回归（CVE-2026-40181）。
//
// 背景：CVE-2026-40181 为 react-router-dom <6.30.4 的 open redirect —— navigate()/
// Link 收到以 `//` 开头的"协议相对路径"时会被当作绝对 URL 导航出站。本项目于提交
// 6589968 升级至 6.30.4 修复，且应用结构上始终使用硬编码路径（HashRouter + 声明式
// 路由 + 兜底 <Navigate to="/" replace />）。本测试把两层性质钉住：
//  1) 路由表：`/` → SplashPage，`/home` → HomePage，`/preview` → PreviewPage，
//     `/settings` → SettingsPage，未知路径 → 兜底重定向到 `/`（SplashPage）；
//  2) open-redirect 回归：以 `#//evil.com` 起始时不得离开 SPA（被兜底重定向到 #/，
//     href 始终停留在 http://localhost 站内）。
//
// 环境：node（无 jsdom），沿用 AppStore.race.test.tsx 的 minimal DOM 模式；
// HashRouter 经 createHashHistory 读取 window.location/history，故额外提供最小
// history 桩（pushState/replaceState 同步回写 location.hash）。antd 与页面模块均
// mock（页面内部已有 e2e 覆盖），测试聚焦路由分派本身。
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { act, useEffect, useRef } from 'react'
import { createRoot, type Root } from 'react-dom/client'
// @ts-expect-error node:module 在 vitest 运行时可用；tsconfig.app types 仅 vite/client（build typecheck 不加载 node 类型），故此处显式抑制
import { createRequire } from 'node:module'
import { HashRouter, Navigate, Route, Routes, useNavigate, type NavigateFunction } from 'react-router-dom'

import App from '../App'

// P3-1：CVE-2026-40181 修复版本钉住。6.30.4 起才修复 navigate()/Link 收到 `//` 协议
// 相对路径时的 open redirect；运行时构建并不导出 version（CJS 无该 key），故经
// createRequire 读取实际安装的 react-router-dom/package.json（pnpm 解析到 node_modules
// 真实版本）——比断言声明式 `^6.30.4` 更强：lockfile/依赖一旦被降级 <6.30.4，下方用例
// 立即 RED。
const reactRouterDomPkg = createRequire(import.meta.url)('react-router-dom/package.json') as {
  version: string
}

// 语义化版本比较（按数字逐段比较，避免 "6.9" > "6.30" 的字符串比较陷阱）。
function versionAtLeast(actual: string, min: string): boolean {
  const a = actual.split('.').map((n) => Number.parseInt(n, 10))
  const b = min.split('.').map((n) => Number.parseInt(n, 10))
  for (let i = 0; i < Math.max(a.length, b.length); i += 1) {
    const x = a[i] ?? 0
    const y = b[i] ?? 0
    if (x !== y) return x > y
  }
  return true
}

// P8-2：versionAtLeast 边界用例（独立 comparator 钉住，不依赖真实安装版本）。
// 判别点：语义化版本按数字逐段比较 —— 相等/更高 → true；更低 → false；
// "6.9" < "6.30"（数字逐段而非字符串字典序）；非数字段经 parseInt 得 NaN，
// 参与比较一律返回 false（安全降级，不抛异常）。
describe('versionAtLeast 边界（P8-2）', () => {
  it('相等版本返回 true（6.30.4 vs 6.30.4）', () => {
    expect(versionAtLeast('6.30.4', '6.30.4')).toBe(true)
  })

  it('修复版之上返回 true（6.30.5、7.0.0）', () => {
    expect(versionAtLeast('6.30.5', '6.30.4')).toBe(true)
    expect(versionAtLeast('7.0.0', '6.30.4')).toBe(true)
  })

  it('降级版本返回 false（6.30.3、6.30.2）', () => {
    expect(versionAtLeast('6.30.3', '6.30.4')).toBe(false)
    expect(versionAtLeast('6.30.2', '6.30.4')).toBe(false)
  })

  it('字符串陷阱：6.9 < 6.30（数字逐段比较而非字典序）', () => {
    expect(versionAtLeast('6.9', '6.30')).toBe(false)
    expect(versionAtLeast('6.30', '6.9')).toBe(true)
  })

  it('非数字段/畸形版本安全处理为 false（abc、6.x、空串）', () => {
    expect(versionAtLeast('abc', '6.30.4')).toBe(false)
    expect(versionAtLeast('6.x', '6.30.4')).toBe(false)
    expect(versionAtLeast('', '6.30.4')).toBe(false)
  })
})

// 页面挂载探针：mock 页面组件渲染时调用，用于断言"哪条路由被渲染"。
const routeSpy = vi.hoisted(() => ({
  splash: vi.fn(),
  home: vi.fn(),
  preview: vi.fn(),
  settings: vi.fn(),
  layout: vi.fn(),
}))

// antd 重组件（ConfigProvider/message/Spin/theme/locale）在无 jsdom 环境不可渲染，
// mock 为透传/空实现（ErrorBoundary 仅异常时渲染 Result/Button，此处提供空实现即可）。
vi.mock('antd', () => ({
  ConfigProvider: ({ children }: { children?: unknown }) => children,
  message: {
    loading: () => () => {},
    success: () => {},
    error: () => {},
  },
  Spin: () => null,
  Button: ({ children }: { children?: unknown }) => children,
  Result: () => null,
  theme: { darkAlgorithm: {}, defaultAlgorithm: {} },
}))
vi.mock('antd/locale/en_US', () => ({ default: {} }))
vi.mock('antd/locale/zh_CN', () => ({ default: {} }))

// 页面 mock：以探针标记渲染目标页面。SplashPage/MainLayout 为静态导入，页面为 lazy
// 动态导入，均按解析后的模块 id 命中 mock。MainLayout 为布局路由，须渲染 Outlet 以
// 呈现嵌套页面。
vi.mock('../app/pages/SplashPage', () => ({
  SplashPage: () => {
    routeSpy.splash()
    return null
  },
}))
vi.mock('../app/pages/HomePage', () => ({
  HomePage: () => {
    routeSpy.home()
    return null
  },
}))
vi.mock('../app/pages/PreviewPage', () => ({
  PreviewPage: () => {
    routeSpy.preview()
    return null
  },
}))
vi.mock('../app/pages/SettingsPage', () => ({
  SettingsPage: () => {
    routeSpy.settings()
    return null
  },
}))
vi.mock('../app/ui/MainLayout', async () => {
  const { Outlet } = await import('react-router-dom')
  return {
    MainLayout: () => {
      routeSpy.layout()
      return <Outlet />
    },
  }
})

// ── 无 jsdom 最小 DOM + HashRouter 全局桩 ─────────────────────────
// HashRouter 通过 document.defaultView 取 location/history；createHashHistory 的
// pushState/replaceState 需同步回写 location.hash，令 history.location 反映导航结果。
// 空 url（""）表示"保持当前 URL"，不得改动 hash（init 的 idx 播种即用空 url）。
function installHashEnvironment(initialHash: string) {
  let hash = initialHash.startsWith('#') ? initialHash : `#${initialHash}`
  const popListeners = new Set<() => void>()

  const location = {
    get hash() {
      return hash
    },
    set hash(v: string) {
      setHash(v)
    },
    href: `http://localhost/#${hash}`,
    origin: 'http://localhost',
    protocol: 'http:',
    host: 'localhost',
    hostname: 'localhost',
    port: '',
    pathname: '/',
    search: '',
    assign: (url: string) => navigateBrowser(url),
    replace: (url: string) => navigateBrowser(url),
    reload: () => {},
    toString: () => location.href,
  }

  const history = {
    state: null as Record<string, unknown> | null,
    pushState(state: Record<string, unknown> | null, _title: string, url?: string) {
      history.state = state
      if (url) navigateBrowser(url)
    },
    replaceState(state: Record<string, unknown> | null, _title: string, url?: string) {
      history.state = state
      if (url) navigateBrowser(url)
    },
    go: () => {},
    back: () => {},
    forward: () => {},
    length: 1,
  }

  // 浏览器 URL 语义桩：pushState/replaceState/location.assign 统一收口。
  // 关键：以 `//` 开头、不含 `#` 前缀的 URL 是"协议相对"外部地址（相对当前协议解析到
  // 外部源，如 http: + //evil.example → http://evil.example）。真实浏览器会把页面导航
  // 到该外部源（CVE-2026-40181 的逃逸机制）；桩忠实模拟：href 变为外部 URL。
  // 站内导航（hash 历史始终以 `#` 开头）不受影响，仍走 setHashFromUrl。
  function navigateBrowser(url: string) {
    if (!url.startsWith('#') && /^\/\//.test(url)) {
      location.href = new URL(`http:${url}`).href // 逃逸：页面离开站内，跳向协议相对外部源
      return
    }
    setHashFromUrl(url)
  }

  function setHashFromUrl(url: string) {
    const idx = url.indexOf('#')
    setHash(idx >= 0 ? url.slice(idx) : `#${url}`)
  }

  function setHash(v: string) {
    hash = v.startsWith('#') ? v : `#${v}`
    location.href = `http://localhost/${hash}`
  }

  const fakeNode = () => ({
    nodeType: 1,
    tagName: 'DIV',
    namespaceURI: 'http://www.w3.org/1999/xhtml',
    style: {},
    attributes: {} as Record<string, string>,
    textContent: '',
    appendChild: () => {},
    removeChild: () => {},
    insertBefore: () => {},
    setAttribute: () => {},
    removeAttribute: () => {},
    addEventListener: () => {},
    removeEventListener: () => {},
  })

  const documentObj = {
    nodeType: 9,
    defaultView: globalThis,
    createElement: fakeNode,
    createTextNode: () => ({ nodeType: 3, textContent: '' }),
    querySelector: () => null,
    addEventListener: () => {},
    removeEventListener: () => {},
  }

  const win = globalThis as unknown as Record<string, unknown>
  win.window = globalThis
  win.location = location
  win.history = history
  win.document = documentObj
  win.addEventListener = (type: string, cb: () => void) => {
    if (type === 'popstate') popListeners.add(cb)
  }
  win.removeEventListener = (type: string, cb: () => void) => {
    if (type === 'popstate') popListeners.delete(cb)
  }
  win.IS_REACT_ACT_ENVIRONMENT = true
  win.HTMLIFrameElement = class HTMLIFrameElement {}
  const backing: Record<string, string> = {}
  win.localStorage = {
    getItem: (k: string) => backing[k] ?? null,
    setItem: (k: string, v: string) => {
      backing[k] = v
    },
    removeItem: (k: string) => {
      delete backing[k]
    },
  }

  return {
    hash: () => location.hash,
    href: () => location.href,
    makeContainer: () => ({
      nodeType: 1,
      tagName: 'DIV',
      namespaceURI: 'http://www.w3.org/1999/xhtml',
      ownerDocument: documentObj,
      addEventListener: () => {},
      removeEventListener: () => {},
      appendChild: () => {},
      removeChild: () => {},
      insertBefore: () => {},
      removeAttribute: () => {},
    }),
  }
}

function cleanupGlobals() {
  const win = globalThis as unknown as Record<string, unknown>
  for (const k of [
    'window',
    'location',
    'history',
    'document',
    'localStorage',
    'IS_REACT_ACT_ENVIRONMENT',
    'HTMLIFrameElement',
    'addEventListener',
    'removeEventListener',
  ]) {
    delete win[k]
  }
}

let rootRef: Root | null = null

async function mountApp(initialHash: string) {
  const env = installHashEnvironment(initialHash)
  const container = env.makeContainer()
  const root = createRoot(container as unknown as Element)
  rootRef = root
  await act(async () => {
    root.render(<App />)
  })
  // 冲刷 lazy 动态导入解析 + <Navigate> 兜底重定向的 effect
  await act(async () => {})
  await act(async () => {})
  return { root, hash: env.hash, href: env.href }
}

// ── CVE-2026-40181 原语探针 ─────────────────────────────────
// 路由级回归（App 兜底 <Navigate to="/" replace />）是第二道防线；本探针直接调用
// react-router 的 navigate 原语（不经 App 的硬编码路由/兜底），钉住"导航原语本身收到
// `//` 协议相对路径时不得把 location 带出站内"。渲染于 HashRouter 上下文内（与 App
// 相同的最外层 router），经 useEffect 暴露 navigate 供测试在 act 内驱动。
function NavigateProbe({ onReady }: { onReady: (nav: NavigateFunction) => void }) {
  const navigate = useNavigate()
  const ready = useRef(false)
  useEffect(() => {
    if (!ready.current) {
      ready.current = true
      onReady(navigate)
    }
  }, [navigate, onReady])
  return null
}

describe('App.tsx 路由（HashRouter，SEC-11 / S-SEC-B10-002）', () => {
  beforeEach(() => {
    for (const spy of [
      routeSpy.splash,
      routeSpy.home,
      routeSpy.preview,
      routeSpy.settings,
      routeSpy.layout,
    ]) {
      spy.mockReset()
    }
  })

  afterEach(async () => {
    if (rootRef) {
      await act(async () => {
        rootRef!.unmount()
      })
      rootRef = null
    }
    cleanupGlobals()
  })

  it('hash=#/ 渲染 SplashPage（启动页，不含主框架）', async () => {
    const m = await mountApp('/')
    expect(routeSpy.splash).toHaveBeenCalledTimes(1)
    expect(routeSpy.layout).not.toHaveBeenCalled()
    expect(routeSpy.home).not.toHaveBeenCalled()
    expect(m.hash()).toBe('#/')
  })

  it('hash=#/home 渲染 HomePage', async () => {
    const m = await mountApp('/home')
    expect(routeSpy.home).toHaveBeenCalledTimes(1)
    expect(routeSpy.splash).not.toHaveBeenCalled()
    expect(routeSpy.preview).not.toHaveBeenCalled()
    expect(routeSpy.settings).not.toHaveBeenCalled()
    expect(m.hash()).toBe('#/home')
  })

  it('hash=#/preview 渲染 PreviewPage', async () => {
    const m = await mountApp('/preview')
    expect(routeSpy.preview).toHaveBeenCalledTimes(1)
    expect(routeSpy.home).not.toHaveBeenCalled()
    expect(routeSpy.settings).not.toHaveBeenCalled()
    expect(m.hash()).toBe('#/preview')
  })

  it('hash=#/settings 渲染 SettingsPage', async () => {
    const m = await mountApp('/settings')
    expect(routeSpy.settings).toHaveBeenCalledTimes(1)
    expect(routeSpy.home).not.toHaveBeenCalled()
    expect(routeSpy.preview).not.toHaveBeenCalled()
    expect(m.hash()).toBe('#/settings')
  })

  it('未知路径重定向到 #/（SplashPage）', async () => {
    const m = await mountApp('/no-such-route')
    expect(routeSpy.splash).toHaveBeenCalledTimes(1)
    expect(routeSpy.home).not.toHaveBeenCalled()
    expect(m.hash()).toBe('#/')
  })

  it('协议相对路径 // 不离开 SPA（open-redirect 回归，CVE-2026-40181）', async () => {
    const m = await mountApp('//evil.example')
    // 兜底 <Navigate to="/" replace /> 生效：SplashPage 渲染，hash 归一为站内 #/
    expect(routeSpy.splash).toHaveBeenCalledTimes(1)
    expect(m.hash()).toBe('#/')
    // 始终停留在本地 SPA：href 前缀为 http://localhost，绝不为 //evil.example / 外部 host
    expect(m.href()).toMatch(/^http:\/\/localhost\//)
    expect(m.href()).not.toContain('evil.example')
  })

  // P3-4a：CVE-2026-40181 原语钉住 —— 直接调用 useNavigate()('//evil.example')，
  // 不经 App 路由/兜底（该回归用例覆盖的是"应用第二道防线"，本用例钉的是"原语本体"）。
  // 探针路由镜像 App 的兜底（* → <Navigate to="/" replace />）：navigate 收到
  // `//` 协议相对路径时，修复版本（6.30.4+）将其视为站内 pathname（hash 历史以
  // fragment 承载），再由兜底归一为 #/；若 react-router 降级 <6.30.4，navigate 会把
  // `//evil.example` 当协议相对外部 URL 逃逸（桩模拟真实浏览器：href 变为外部源）→ RED。
  it('useNavigate 原语：navigate("//evil.example") 不得离开站内（CVE-2026-40181 本体）', async () => {
    const env = installHashEnvironment('/')
    const container = env.makeContainer()
    const root = createRoot(container as unknown as Element)
    rootRef = root
    let navigate: NavigateFunction | null = null
    await act(async () => {
      root.render(
        <HashRouter>
          <Routes>
            <Route path="/" element={<NavigateProbe onReady={(nav) => (navigate = nav)} />} />
            <Route path="*" element={<Navigate to="/" replace />} />
          </Routes>
        </HashRouter>,
      )
    })
    expect(navigate).not.toBeNull()
    await act(async () => {
      navigate!('//evil.example')
    })
    await act(async () => {})
    await act(async () => {})
    // 判别点：hash 被兜底归一为站内 #/，href 始终停留在 http://localhost ——
    // 原语不得把 location 带出站内（协议相对逃逸时 href 为外部源，下列断言 RED）。
    expect(env.hash()).toBe('#/')
    expect(env.href()).toMatch(/^http:\/\/localhost\//)
    expect(env.href()).not.toContain('evil.example')
  })

  // P3-1：版本断言钉住 CVE-2026-40181 修复版本。上述原语用例在 HashRouter 结构下
  // 6.30.3/6.30.4 行为一致恒绿（判别力缺陷）——若 react-router-dom 被降级 <6.30.4，
  // 原语用例仍可能不 RED，但本用例直接断言安装版本 ≥ 6.30.4 即 RED。
  it('react-router-dom 安装版本 ≥ 6.30.4（CVE-2026-40181 修复版）', () => {
    expect(versionAtLeast(reactRouterDomPkg.version, '6.30.4')).toBe(true)
  })
})
