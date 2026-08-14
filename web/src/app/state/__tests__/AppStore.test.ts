import { describe, it, expect, beforeEach } from 'vitest'
import { loadPrefs, savePrefs, defaultPrefs, type LocalPrefs } from '../prefs'

const STORAGE_KEY = 'rk_wcfr_web_prefs_v1'

// localStorage 假实现（node 环境无 localStorage）
let backingStore: Record<string, string> = {}
beforeEach(() => {
  backingStore = {}
  ;(globalThis as unknown as { localStorage: unknown }).localStorage = {
    getItem: (k: string) => backingStore[k] ?? null,
    setItem: (k: string, v: string) => {
      backingStore[k] = v
    },
    removeItem: (k: string) => {
      delete backingStore[k]
    },
  }
})

describe('prefs（导入真实 store/prefs.ts 实现）', () => {
  it('无存储 → 默认值', () => {
    expect(loadPrefs()).toEqual(defaultPrefs)
  })

  it('保存后读回一致', () => {
    const p: LocalPrefs = { ...defaultPrefs, theme: 'dark', apiTimeoutMs: 10000, language: 'en-US' }
    savePrefs(p)
    expect(loadPrefs()).toEqual(p)
  })

  it('非法枚举字段回退默认', () => {
    backingStore[STORAGE_KEY] = JSON.stringify({ theme: 'bogus', language: 'xx', startPage: 'nope' })
    const p = loadPrefs()
    expect(p.theme).toBe(defaultPrefs.theme)
    expect(p.language).toBe(defaultPrefs.language)
    expect(p.startPage).toBe(defaultPrefs.startPage)
  })

  it('apiTimeoutMs 越界被 clamp 到边界', () => {
    backingStore[STORAGE_KEY] = JSON.stringify({ apiTimeoutMs: 999999 })
    expect(loadPrefs().apiTimeoutMs).toBe(120000)
    backingStore[STORAGE_KEY] = JSON.stringify({ apiTimeoutMs: 1 })
    expect(loadPrefs().apiTimeoutMs).toBe(500)
  })

  it('损坏 JSON → 默认值（不抛异常）', () => {
    backingStore[STORAGE_KEY] = '{bad json'
    expect(loadPrefs()).toEqual(defaultPrefs)
  })
})
