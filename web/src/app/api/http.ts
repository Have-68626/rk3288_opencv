import type { CacheStrategy, LogLevel } from '../state/prefs'

export class ApiError extends Error {
  public readonly code: string
  public readonly httpStatus?: number
  public readonly details?: string[]

  constructor(
    code: string,
    message: string,
    opts?: { httpStatus?: number; details?: string[] },
  ) {
    super(message)
    this.name = 'ApiError'
    this.code = code
    this.httpStatus = opts?.httpStatus
    this.details = opts?.details
  }
}

function log(
  level: LogLevel,
  want: Exclude<LogLevel, 'silent'>,
  ...args: unknown[]
) {
  // 为什么不用复杂 logger：这个项目的 UI 是嵌入式/本地工具，优先“可用+可控”；
  // 真正的落盘日志在后端（C++）完成。
  const order: Record<LogLevel, number> = {
    silent: 0,
    error: 1,
    warn: 2,
    info: 3,
    debug: 4,
  }
  if (order[level] < order[want]) return
  console[want](...args)
}

type CacheEntry = { expiresAt: number; json: unknown }
const memoryCache = new Map<string, CacheEntry>()

function getFromCache(strategy: CacheStrategy, key: string): unknown | undefined {
  const now = Date.now()

  if (strategy === 'memory-30s') {
    const hit = memoryCache.get(key)
    if (!hit) return undefined
    if (hit.expiresAt < now) {
      memoryCache.delete(key)
      return undefined
    }
    return hit.json
  }

  if (strategy === 'local-5m') {
    try {
      const raw = localStorage.getItem(`rk_wcfr_cache:${key}`)
      if (!raw) return undefined
      const parsed = JSON.parse(raw) as CacheEntry
      if (typeof parsed?.expiresAt !== 'number') return undefined
      if (parsed.expiresAt < now) {
        localStorage.removeItem(`rk_wcfr_cache:${key}`)
        return undefined
      }
      return parsed.json
    } catch {
      return undefined
    }
  }

  return undefined
}

function putToCache(strategy: CacheStrategy, key: string, json: unknown) {
  const now = Date.now()
  if (strategy === 'memory-30s') {
    memoryCache.set(key, { expiresAt: now + 30_000, json })
  } else if (strategy === 'local-5m') {
    try {
      localStorage.setItem(
        `rk_wcfr_cache:${key}`,
        JSON.stringify({ expiresAt: now + 5 * 60_000, json } satisfies CacheEntry),
      )
    } catch {
      // localStorage 可能满/禁用；忽略即可
    }
  }
}

export async function fetchJson<T>(
  input: string,
  init: RequestInit & {
    timeoutMs: number
    logLevel: LogLevel
    cacheStrategy: CacheStrategy
    cacheKey?: string
  },
): Promise<T> {
  const cacheKey = init.cacheKey ?? input
  if (init.method?.toUpperCase() === 'GET' && init.cacheStrategy !== 'no-cache') {
    const hit = getFromCache(init.cacheStrategy, cacheKey)
    if (hit !== undefined) {
      log(init.logLevel, 'debug', '[api] cache hit', cacheKey)
      return hit as T
    }
  }

  const controller = new AbortController()
  const timeoutId = window.setTimeout(() => controller.abort(), init.timeoutMs)
  try {
    log(init.logLevel, 'debug', '[api] request', init.method ?? 'GET', input)
    const res = await fetch(input, {
      ...init,
      signal: controller.signal,
      headers: {
        'Content-Type': 'application/json',
        ...(init.headers ?? {}),
      },
    })

    const text = await res.text()
    const json = text ? (JSON.parse(text) as unknown) : null
    if (!res.ok) {
      // 后端使用统一 envelope：{ok:false,error:{code,message,details?}}
      const errObj = json as Record<string, unknown>
      const errorField = errObj?.error as Record<string, unknown> | undefined
      const code = typeof errorField?.code === 'string' ? errorField.code : 'http_error'
      const message = typeof errorField?.message === 'string' ? errorField.message : `HTTP ${res.status}`
      const details: string[] | undefined = Array.isArray(errorField?.details)
        ? (errorField.details as string[])
        : undefined
      throw new ApiError(code, message, { httpStatus: res.status, details })
    }

    if (init.method?.toUpperCase() === 'GET' && init.cacheStrategy !== 'no-cache') {
      putToCache(init.cacheStrategy, cacheKey, json)
    }

    return json as T
  } catch (e: unknown) {
    const err = e as Error
    if (err?.name === 'AbortError') {
      throw new ApiError('timeout', `请求超时（>${init.timeoutMs}ms）`, {
        httpStatus: 0,
      })
    }
    if (err instanceof ApiError) throw err
    throw new ApiError('network_error', err?.message || '网络错误/解析失败')
  } finally {
    window.clearTimeout(timeoutId)
  }
}

