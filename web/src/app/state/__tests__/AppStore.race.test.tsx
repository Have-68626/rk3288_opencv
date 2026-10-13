import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { act, useEffect } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import type { ServerSettingsDoc } from '../../api/types'
import { AppStoreProvider, useAppStore } from '../AppStore'
import { getServerSettings, putServerSettings } from '../../api/settings'

// —— Q-CQ-B08-003 竞态判别测试 ——
// 目标：设置加载/保存逆序完成时，仅最新 generation 生效；旧响应（慢 GET 后完成）不得覆盖新状态。
// 判定：mock settings API，用可手工 resolve 的 deferred promise 精确编排
//   GET#1(慢) → PUT → GET#2(快) → GET#1(最后完成)，
// 断言最终状态是 GET#2（最新代次）的数据，而非被 GET#1 的过期数据覆盖。

// settings API 整体 mock：不发真实网络请求，仅返回编排好的 promise
vi.mock('../../api/settings', () => ({
  getServerSettings: vi.fn(),
  putServerSettings: vi.fn(),
}))

const mockGet = vi.mocked(getServerSettings)
const mockPut = vi.mocked(putServerSettings)

function deferred<T>() {
  let resolve!: (v: T) => void
  let reject!: (e: unknown) => void
  const promise = new Promise<T>((res, rej) => {
    resolve = res
    reject = rej
  })
  return { promise, resolve, reject }
}

type OkEnv = { ok: true; data: ServerSettingsDoc }

// 完整文档骨架：结构对齐后端 schema；仅用 log.logDir 作可判别标记
const BASE: ServerSettingsDoc = {
  schemaVersion: 1,
  camera: { preferredDeviceId: '', width: 640, height: 480, fps: 30 },
  recognition: { cascadePath: '', databasePath: '', minFaceSizePx: 100, identifyThreshold: 0.6, enrollSamples: 5, arcFaceModelPath: '' },
  inference: { throttleMode: 'interval', intervalMs: 100 },
  dnn: { enable: false, modelPath: '', configPath: '', inputWidth: 640, inputHeight: 640, scale: 1, meanB: 0, meanG: 0, meanR: 0, swapRB: false, confThreshold: 0.5, backend: 0, target: 0 },
  model: { detection: '', recognition: '', backend: '', detectorBackend: '', recognitionBackend: '', autoFallback: true, int8Enabled: false },
  http: { enable: true, port: 8080 },
  poster: { enable: false, postUrl: '', throttleMs: 1000, backoffMinMs: 1000, backoffMaxMs: 10000 },
  log: { logDir: '', maxFileBytes: 1048576, maxRollFiles: 3 },
  ui: { windowWidth: 1280, windowHeight: 720, previewScaleMode: 1 },
  display: { outputIndex: 0, width: 1920, height: 1080, refreshNumerator: 60, refreshDenominator: 1, vsync: true, swapchainBuffers: 2, fullscreen: false, allowSystemModeSwitch: true, enableSRGB: false, gamma: 1, colorTempK: 6500, aaSamples: 1, anisoLevel: 1 },
  acceleration: { enableOpenCL: false, enableLibyuv: false, enableMpp: false, enableQualcomm: false },
}
function docWith(logDir: string): ServerSettingsDoc {
  return { ...BASE, log: { ...BASE.log, logDir } }
}

// node 环境无 DOM：提供 createRoot 所需的最小容器/全局（不引入 jsdom 依赖）。
// getRootHostContext 的 default 分支会读 container.tagName/namespaceURI，必须补齐。
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
  ;(globalThis as unknown as Record<string, unknown>).document = documentObj
  ;(globalThis as unknown as Record<string, unknown>).IS_REACT_ACT_ENVIRONMENT = true
  ;(globalThis as unknown as Record<string, unknown>).window = globalThis
  // react-dom 的 getActiveElementDeep 会执行 element instanceof win.HTMLIFrameElement，
  // node 环境缺此类，补一个空类避免 instanceof 抛错（实例不会命中）。
  ;(globalThis as unknown as Record<string, unknown>).HTMLIFrameElement = class HTMLIFrameElement {}
  return {
    container: {
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
    },
  }
}

// 渲染 Provider 并暴露最新 store 引用；每次 re-render 都会重写，供 act 冲刷后读取。
// 注意：不能在 render 期间写外部变量（react-hooks/globals 视为 render 副作用），
// 改为在 useEffect 中写入 ref —— act 冲刷时 effect 必然已执行，时序与直接赋值等价。
async function mountStore() {
  const storeRef: { current: ReturnType<typeof useAppStore> | null } = { current: null }
  function Capture() {
    const v = useAppStore()
    useEffect(() => {
      storeRef.current = v
    }, [v])
    return null
  }
  const { container } = installMinimalDom()
  const root = createRoot(container as unknown as Element)
  await act(async () => {
    root.render(
      <AppStoreProvider>
        <Capture />
      </AppStoreProvider>,
    )
  })
  return { root, getStore: () => storeRef.current! }
}

let backing: Record<string, string> = {}
let root: Root | null = null

beforeEach(() => {
  mockGet.mockReset()
  mockPut.mockReset()
  backing = {}
  ;(globalThis as unknown as { localStorage: unknown }).localStorage = {
    getItem: (k: string) => backing[k] ?? null,
    setItem: (k: string, v: string) => {
      backing[k] = v
    },
    removeItem: (k: string) => {
      delete backing[k]
    },
  }
})

afterEach(async () => {
  if (root) {
    await act(async () => {
      root!.unmount()
    })
    root = null
  }
  delete (globalThis as unknown as { document?: unknown }).document
  delete (globalThis as unknown as { window?: unknown }).window
  delete (globalThis as unknown as { localStorage?: unknown }).localStorage
  delete (globalThis as unknown as { IS_REACT_ACT_ENVIRONMENT?: unknown }).IS_REACT_ACT_ENVIRONMENT
  delete (globalThis as unknown as { HTMLIFrameElement?: unknown }).HTMLIFrameElement
})

describe('AppStore 设置加载/保存竞态（Q-CQ-B08-003）', () => {
  it('慢 GET#1 最后完成时不得覆盖 GET#2/PUT 的最新状态', async () => {
    const get1 = deferred<OkEnv>()
    const put = deferred<OkEnv>()
    const get2 = deferred<OkEnv>()
    mockGet.mockReturnValueOnce(get1.promise).mockReturnValueOnce(get2.promise)
    mockPut.mockReturnValueOnce(put.promise)

    const m = await mountStore()
    root = m.root
    const store = m.getStore()

    // 发起顺序：GET#1（gen=1）→ PUT（gen=2）→ GET#2（gen=3）。
    // 注意：不 await store 函数，它们的内部 promise 是 deferred，等待后续手工 resolve。
    await act(async () => {
      store.refreshServerSettings({ silent: true })
    })
    await act(async () => {
      store.updateServerSettings({ ui: { windowWidth: 999 } } as Partial<ServerSettingsDoc>)
    })
    await act(async () => {
      store.refreshServerSettings({ silent: true })
    })

    // GET#2 先返回（gen=3 最新）→ 状态应为 GET#2 数据
    await act(async () => {
      get2.resolve({ ok: true, data: docWith('get2') })
    })
    expect(m.getStore().serverSettings.status).toBe('ready')
    expect(m.getStore().serverSettings.data?.log.logDir).toBe('get2')

    // PUT 逆序完成（gen=2 过期）→ 不得回退
    await act(async () => {
      put.resolve({ ok: true, data: docWith('put') })
    })
    expect(m.getStore().serverSettings.data?.log.logDir).toBe('get2')

    // GET#1 最后完成（gen=1 最旧）→ 不得覆盖新状态（修复前：竞态覆盖为 get1 → 失败）
    await act(async () => {
      get1.resolve({ ok: true, data: docWith('get1') })
    })
    expect(m.getStore().serverSettings.status).toBe('ready')
    expect(m.getStore().serverSettings.data?.log.logDir).toBe('get2')
  })

  it('PUT 为最新代次时其响应生效（GET 逆序完成不得覆盖）', async () => {
    const get1 = deferred<OkEnv>()
    const put = deferred<OkEnv>()
    mockGet.mockReturnValueOnce(get1.promise)
    mockPut.mockReturnValueOnce(put.promise)

    const m = await mountStore()
    root = m.root
    const store = m.getStore()

    // 发起顺序：GET#1（gen=1）→ PUT（gen=2，最新）→ PUT 先返回
    await act(async () => {
      store.refreshServerSettings({ silent: true })
    })
    await act(async () => {
      store.updateServerSettings({ ui: { windowWidth: 999 } } as Partial<ServerSettingsDoc>)
    })
    await act(async () => {
      put.resolve({ ok: true, data: docWith('put') })
    })
    expect(m.getStore().serverSettings.data?.log.logDir).toBe('put')

    // GET#1 逆序完成（gen=1 过期）→ 不得覆盖 PUT 结果
    await act(async () => {
      get1.resolve({ ok: true, data: docWith('get1') })
    })
    expect(m.getStore().serverSettings.status).toBe('ready')
    expect(m.getStore().serverSettings.data?.log.logDir).toBe('put')
  })
})
