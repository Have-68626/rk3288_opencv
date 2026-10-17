import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { fetchJson, ApiError, CACHE_KEY_PREFIX } from '../http'
import {
  getApiToken,
  setApiToken,
  clearApiToken,
  API_TOKEN_STORAGE_KEY,
} from '../../state/prefs'

// fetchJson 用 window.setTimeout/clearTimeout —— node 环境无 window，指向全局
// 避免污染其他测试，afterEach 删除。
beforeEach(() => {
  ;(globalThis as unknown as { window: unknown }).window = globalThis
})
afterEach(() => {
  delete (globalThis as unknown as { window?: unknown }).window
})

const mockFetch = vi.fn()
;(globalThis as unknown as { fetch: unknown }).fetch = mockFetch

function jsonResponse(ok: boolean, body: unknown, status = 200) {
  return {
    ok,
    status,
    text: async () => JSON.stringify(body),
  }
}

describe('fetchJson（导入真实 http.ts 实现）', () => {
  beforeEach(() => {
    mockFetch.mockReset()
  })

  it('GET 成功：返回信封数据', async () => {
    mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, data: { x: 1 } }))
    const data = await fetchJson('/api/x', {
      timeoutMs: 100,
      logLevel: 'silent',
      cacheStrategy: 'no-cache',
    })
    expect(data).toEqual({ ok: true, data: { x: 1 } })
  })

  it('memory-30s 缓存：两次 GET 只发一次 fetch', async () => {
    mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 1 }))
    const opts = {
      method: 'GET',
      timeoutMs: 100,
      logLevel: 'silent' as const,
      cacheStrategy: 'memory-30s' as const,
      cacheKey: 'fetchjson-cache-test',
    }
    const a = await fetchJson('/api/x', opts)
    const b = await fetchJson('/api/x', opts)
    expect(mockFetch).toHaveBeenCalledTimes(1)
    expect(a).toEqual(b)
  })

  it('失败信封不阻断重试恢复', async () => {
    const failure = { ok: false, error: { code: 'unavailable', message: '暂未就绪' } }
    const success = { ok: true, data: { ready: true } }
    mockFetch.mockResolvedValueOnce(jsonResponse(true, failure)).mockResolvedValueOnce(jsonResponse(true, success))
    const opts = { method: 'GET', timeoutMs: 100, logLevel: 'silent' as const, cacheStrategy: 'memory-30s' as const, cacheKey: 'retry-after-failed-envelope' }
    expect(await fetchJson('/api/retry-fixture', opts)).toEqual(failure)
    expect(await fetchJson('/api/retry-fixture', opts)).toEqual(success)
    expect(mockFetch).toHaveBeenCalledTimes(2)
  })

  it('HTTP 错误：映射 ApiError（code/httpStatus/details）', async () => {
    mockFetch.mockResolvedValue(
      jsonResponse(false, { ok: false, error: { code: 'bad_request', message: '参数错误', details: ['a'] } }, 400),
    )
    const p = fetchJson('/api/x', { timeoutMs: 100, logLevel: 'silent', cacheStrategy: 'no-cache' })
    await expect(p).rejects.toMatchObject({ code: 'bad_request', httpStatus: 400, details: ['a'] })
    await expect(p).rejects.toBeInstanceOf(ApiError)
  })

  it('HTTP 错误：无 error 信封时回退 http_error + 状态码', async () => {
    mockFetch.mockResolvedValue(jsonResponse(false, { message: 'oops' }, 500))
    const p = fetchJson('/api/x', { timeoutMs: 100, logLevel: 'silent', cacheStrategy: 'no-cache' })
    await expect(p).rejects.toMatchObject({ code: 'http_error', httpStatus: 500 })
  })

  it('网络错误：映射 ApiError(network_error)', async () => {
    mockFetch.mockRejectedValue(new Error('boom'))
    const p = fetchJson('/api/x', { timeoutMs: 100, logLevel: 'silent', cacheStrategy: 'no-cache' })
    await expect(p).rejects.toMatchObject({ code: 'network_error' })
  })

  it('超时：映射 ApiError(timeout)', async () => {
    mockFetch.mockImplementation(
      (_url: unknown, init: RequestInit) =>
        new Promise((_resolve, reject) => {
          init.signal?.addEventListener('abort', () =>
            reject(Object.assign(new Error('aborted'), { name: 'AbortError' })),
          )
        }),
    )
    const p = fetchJson('/api/x', { timeoutMs: 5, logLevel: 'silent', cacheStrategy: 'no-cache' })
    await expect(p).rejects.toMatchObject({ code: 'timeout' })
  })

  it('信封缺少 ok 字段：映射 ApiError(invalid_response)', async () => {
    mockFetch.mockResolvedValue(jsonResponse(true, { noOkField: true }))
    const p = fetchJson('/api/x', { timeoutMs: 100, logLevel: 'silent', cacheStrategy: 'no-cache' })
    await expect(p).rejects.toMatchObject({ code: 'invalid_response' })
  })

  describe('local-5m 缓存（localStorage 分支）', () => {
    // 为什么放 describe 内：localStorage stub 只对 local-5m 用例有意义，
    // 声明在局部可避免其他用例/其他测试文件意外读到这个全局。
    let backingStore: Record<string, string> = {}
    let localStorageMock: ReturnType<typeof installLocalStorageStub>

    function installLocalStorageStub(store: Record<string, string>) {
      return {
        getItem: vi.fn((k: string) => store[k] ?? null),
        setItem: vi.fn((k: string, v: string) => {
          store[k] = v
        }),
        removeItem: vi.fn((k: string) => {
          delete store[k]
        }),
      }
    }

    beforeEach(() => {
      // 每例重建 stub：node 环境无 localStorage，且重置 store 与调用记录
      backingStore = {}
      localStorageMock = installLocalStorageStub(backingStore)
      ;(globalThis as unknown as { localStorage: unknown }).localStorage = localStorageMock
    })
    afterEach(() => {
      // 清理全局，避免污染其他测试文件
      delete (globalThis as unknown as { localStorage?: unknown }).localStorage
    })

    // 存储 key 前缀直接复用 http.ts 导出的 CACHE_KEY_PREFIX（单一来源），
    // 改前缀时实现与测试同步更新，无漂移风险。
    const storageKey = (cacheKey: string) => `${CACHE_KEY_PREFIX}${cacheKey}`

    it('失败信封不写入持久缓存，恢复后才缓存成功结果', async () => {
      const failure = { ok: false, error: { code: 'fixture', message: '暂未就绪' } }
      const success = { ok: true, data: 'recovered' }
      const opts = { method: 'GET', timeoutMs: 100, logLevel: 'silent' as const, cacheStrategy: 'local-5m' as const, cacheKey: 'no-persisted-failure' }
      mockFetch.mockResolvedValueOnce(jsonResponse(true, failure)).mockResolvedValueOnce(jsonResponse(true, success))
      expect(await fetchJson('/api/retry', opts)).toEqual(failure)
      expect(localStorageMock.getItem(storageKey(opts.cacheKey))).toBeNull()
      expect(await fetchJson('/api/retry', opts)).toEqual(success)
      expect(JSON.parse(localStorageMock.getItem(storageKey(opts.cacheKey))!).json).toEqual(success)
    })

    it('忽略旧版本落盘的失败信封并恢复请求', async () => {
      backingStore[storageKey('legacy-failure')] = JSON.stringify({ expiresAt: Date.now() + 60000, json: { ok: false } })
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, data: 'recovered' }))
      const result = await fetchJson('/api/recovery', { method: 'GET', timeoutMs: 100, logLevel: 'silent', cacheStrategy: 'local-5m', cacheKey: 'legacy-failure' })
      expect(result).toEqual({ ok: true, data: 'recovered' })
      expect(mockFetch).toHaveBeenCalledTimes(1)
    })

    it('缓存命中：两次 GET 只发一次 fetch，第二次读 localStorage', async () => {
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 1 }))
      const opts = {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent' as const,
        cacheStrategy: 'local-5m' as const,
        cacheKey: 'local-hit-test',
      }
      const a = await fetchJson('/api/x', opts)
      const b = await fetchJson('/api/x', opts)
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(a).toEqual(b)
      // 首次请求成功后已写入 localStorage，第二次才能直接命中
      expect(localStorageMock.getItem(storageKey('local-hit-test'))).not.toBeNull()
    })

    it('过期条目：重新 fetch 且 removeItem 清除后写入新缓存', async () => {
      // 预置 expiresAt 已过去的条目，模拟“距上次写入超过 5 分钟”
      const key = storageKey('local-expired-test')
      backingStore[key] = JSON.stringify({
        expiresAt: Date.now() - 1_000,
        json: { ok: true, v: 'stale' },
      })
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 'fresh' }))
      const data = await fetchJson('/api/x', {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent',
        cacheStrategy: 'local-5m',
        cacheKey: 'local-expired-test',
      })
      // 过期缓存被判定失效：removeItem 被调用、重新走 fetch、返回新数据
      expect(localStorageMock.removeItem).toHaveBeenCalledWith(key)
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(data).toEqual({ ok: true, v: 'fresh' })
      // fetch 成功后写入新条目，expiresAt 更新为未来时间
      const refreshed = JSON.parse(backingStore[key]) as { expiresAt: number }
      expect(refreshed.expiresAt).toBeGreaterThan(Date.now())
    })

    it('损坏 JSON：忽略缓存并重新 fetch', async () => {
      // 覆盖 getFromCache 的 JSON.parse try/catch 回退分支
      backingStore[storageKey('local-corrupt-test')] = '{broken json'
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 'live' }))
      const data = await fetchJson('/api/x', {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent',
        cacheStrategy: 'local-5m',
        cacheKey: 'local-corrupt-test',
      })
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(data).toEqual({ ok: true, v: 'live' })
    })

    it('expiresAt 类型非法：视为无效缓存重新 fetch（不 removeItem）', async () => {
      // 覆盖 typeof expiresAt !== 'number' 守卫（区别于损坏 JSON 分支：此分支不删除条目）
      backingStore[storageKey('local-badtype-test')] = JSON.stringify({
        expiresAt: 'tomorrow',
        json: { ok: true, v: 'bad' },
      })
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 'live' }))
      const data = await fetchJson('/api/x', {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent',
        cacheStrategy: 'local-5m',
        cacheKey: 'local-badtype-test',
      })
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(data).toEqual({ ok: true, v: 'live' })
      expect(localStorageMock.removeItem).not.toHaveBeenCalled()
    })

    it('localStorage.setItem 抛异常（配额满/禁用）：降级为直接 fetch', async () => {
      // putToCache 的 try/catch 应吞掉写入失败，不影响本次请求返回
      localStorageMock.setItem.mockImplementation(() => {
        throw new Error('QuotaExceededError')
      })
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 1 }))
      const data = await fetchJson('/api/x', {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent',
        cacheStrategy: 'local-5m',
        cacheKey: 'local-quota-test',
      })
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(data).toEqual({ ok: true, v: 1 })
    })

    it('localStorage.getItem 抛异常（禁用）：降级为直接 fetch', async () => {
      // getFromCache 的 try/catch 应吞掉读取失败，回退到网络请求
      localStorageMock.getItem.mockImplementation(() => {
        throw new Error('SecurityError')
      })
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, v: 1 }))
      const data = await fetchJson('/api/x', {
        method: 'GET',
        timeoutMs: 100,
        logLevel: 'silent',
        cacheStrategy: 'local-5m',
        cacheKey: 'local-denied-test',
      })
      expect(mockFetch).toHaveBeenCalledTimes(1)
      expect(data).toEqual({ ok: true, v: 1 })
    })
  })

  describe('Authorization Bearer 注入（API Token）', () => {
    // 与 local-5m 同款 stub：只对以下用例生效，避免污染文件外全局
    let backingStore: Record<string, string> = {}
    let localStorageMock: ReturnType<typeof installLocalStorageStub>

    function installLocalStorageStub(store: Record<string, string>) {
      return {
        getItem: vi.fn((k: string) => store[k] ?? null),
        setItem: vi.fn((k: string, v: string) => {
          store[k] = v
        }),
        removeItem: vi.fn((k: string) => {
          delete store[k]
        }),
      }
    }

    const baseOpts = {
      timeoutMs: 100,
      logLevel: 'silent' as const,
      cacheStrategy: 'no-cache' as const,
    }

    beforeEach(() => {
      backingStore = {}
      localStorageMock = installLocalStorageStub(backingStore)
      ;(globalThis as unknown as { localStorage: unknown }).localStorage = localStorageMock
      mockFetch.mockReset()
      mockFetch.mockResolvedValue(jsonResponse(true, { ok: true, data: { x: 1 } }))
    })
    afterEach(() => {
      // 清理全局，避免污染其他测试文件
      delete (globalThis as unknown as { localStorage?: unknown }).localStorage
    })

    it('已设置 token：请求附加 Authorization: Bearer <token>', async () => {
      // 直接预置 localStorage，模拟用户已保存 token（绕过 setApiToken 以测试 http 读取路径）
      backingStore[API_TOKEN_STORAGE_KEY] = 'secret-token'
      await fetchJson('/api/x', baseOpts)
      const init = mockFetch.mock.calls[0][1] as RequestInit
      expect(init.headers).toEqual({
        'Content-Type': 'application/json',
        Authorization: 'Bearer secret-token',
      })
    })

    it('未设置 token：请求不带 Authorization（headers 与现状完全一致）', async () => {
      await fetchJson('/api/x', baseOpts)
      const init = mockFetch.mock.calls[0][1] as RequestInit
      // 逐字比对：无 token 时 header 必须保持原样，保证默认行为不变
      expect(init.headers).toEqual({ 'Content-Type': 'application/json' })
    })

    it('清除 token 后：后续请求不再附带 Authorization', async () => {
      setApiToken('secret-token')
      await fetchJson('/api/x', baseOpts)
      expect((mockFetch.mock.calls[0][1] as RequestInit).headers).toMatchObject({
        Authorization: 'Bearer secret-token',
      })
      clearApiToken()
      await fetchJson('/api/x', baseOpts)
      expect((mockFetch.mock.calls[1][1] as RequestInit).headers).not.toHaveProperty('Authorization')
    })

    it('空白 token（纯空格）：视为未设置，不带 Authorization', async () => {
      backingStore[API_TOKEN_STORAGE_KEY] = '   '
      await fetchJson('/api/x', baseOpts)
      expect((mockFetch.mock.calls[0][1] as RequestInit).headers).not.toHaveProperty('Authorization')
    })

    it('setApiToken 去除首尾空白后持久化，getApiToken 可读回', async () => {
      setApiToken('  tok-en  ')
      expect(getApiToken()).toBe('tok-en')
    })
  })
})
