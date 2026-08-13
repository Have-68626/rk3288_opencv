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
import { act } from 'react'
import { createRoot, type Root } from 'react-dom/client'

import App from '../App'

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
    assign: () => {},
    replace: () => {},
    reload: () => {},
    toString: () => location.href,
  }

  const history = {
    state: null as Record<string, unknown> | null,
    pushState(state: Record<string, unknown> | null, _title: string, url?: string) {
      history.state = state
      if (url) setHashFromUrl(url)
    },
    replaceState(state: Record<string, unknown> | null, _title: string, url?: string) {
      history.state = state
      if (url) setHashFromUrl(url)
    },
    go: () => {},
    back: () => {},
    forward: () => {},
    length: 1,
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
})
