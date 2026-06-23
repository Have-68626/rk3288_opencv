export type ThemeMode = 'system' | 'light' | 'dark'
export type Language = 'zh-CN' | 'en-US'
export type StartPage = 'home' | 'preview' | 'settings'

// 这些“高级设置”是前端自己的运行策略，不属于后端 /api/v1/settings 的 schema。
// 为什么要区分：
// - 后端 schema 采用 additionalProperties=false，随便塞字段会被 400 拒绝；
// - 但前端依然需要“超时/日志/缓存”等策略，否则调试与稳定性不可控。
export type LogLevel = 'silent' | 'error' | 'warn' | 'info' | 'debug'
export type CacheStrategy = 'no-cache' | 'memory-30s' | 'local-5m'

export interface LocalPrefs {
  theme: ThemeMode
  language: Language
  startPage: StartPage

  apiTimeoutMs: number
  logLevel: LogLevel
  cacheStrategy: CacheStrategy
}

const STORAGE_KEY = 'rk_wcfr_web_prefs_v1'

export const defaultPrefs: LocalPrefs = {
  theme: 'system',
  language: 'zh-CN',
  startPage: 'home',
  apiTimeoutMs: 8000,
  logLevel: 'info',
  cacheStrategy: 'memory-30s',
}

function clampInt(v: unknown, min: number, max: number, fallback: number) {
  if (typeof v !== 'number' || !Number.isFinite(v)) return fallback
  const n = Math.floor(v)
  if (n < min) return min
  if (n > max) return max
  return n
}

export function loadPrefs(): LocalPrefs {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return defaultPrefs
    const obj = JSON.parse(raw) as Partial<LocalPrefs>

    const theme: ThemeMode =
      obj.theme === 'light' || obj.theme === 'dark' || obj.theme === 'system'
        ? obj.theme
        : defaultPrefs.theme

    const language: Language =
      obj.language === 'en-US' || obj.language === 'zh-CN'
        ? obj.language
        : defaultPrefs.language

    const startPage: StartPage =
      obj.startPage === 'settings' || obj.startPage === 'home' || obj.startPage === 'preview'
        ? obj.startPage
        : defaultPrefs.startPage

    const logLevel: LogLevel =
      obj.logLevel === 'silent' ||
      obj.logLevel === 'error' ||
      obj.logLevel === 'warn' ||
      obj.logLevel === 'info' ||
      obj.logLevel === 'debug'
        ? obj.logLevel
        : defaultPrefs.logLevel

    const cacheStrategy: CacheStrategy =
      obj.cacheStrategy === 'no-cache' ||
      obj.cacheStrategy === 'memory-30s' ||
      obj.cacheStrategy === 'local-5m'
        ? obj.cacheStrategy
        : defaultPrefs.cacheStrategy

    return {
      theme,
      language,
      startPage,
      apiTimeoutMs: clampInt(obj.apiTimeoutMs, 500, 120000, defaultPrefs.apiTimeoutMs),
      logLevel,
      cacheStrategy,
    }
  } catch {
    // 读取失败不应阻断启动；直接回退默认值
    return defaultPrefs
  }
}

export function savePrefs(next: LocalPrefs) {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(next))
}
