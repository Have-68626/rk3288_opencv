import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { fetchJson, ApiError } from '../http'

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
})
