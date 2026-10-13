/* eslint-disable react-refresh/only-export-components */
import React, { createContext, useCallback, useContext, useMemo, useRef, useState } from 'react'

import type { ServerSettingsDoc } from '../api/types'
import { getServerSettings, putServerSettings } from '../api/settings'
import type { LocalPrefs } from './prefs'
import { defaultPrefs, loadPrefs, savePrefs } from './prefs'
import { ApiError } from '../api/http'

/** Safely extract ApiError from catch clause */
function toApiError(e: unknown): ApiError {
  if (e instanceof ApiError) return e
  const msg = typeof e === 'object' && e !== null && 'message' in e && typeof (e as Record<string, unknown>).message === 'string'
    ? (e as Record<string, unknown>).message as string
    : '未知错误'
  return new ApiError('unknown', msg)
}

/** Overridable notification sink — production wires to antd message, tests no-op. */
export let notify: { loading: (msg: string) => () => void; success: (msg: string) => void; error: (msg: string) => void } = {
  loading: () => () => {},
  success: () => {},
  error: () => {},
}

export function setNotify(
  n: typeof notify,
) {
  notify = n
}

export type ServerSettingsState =
  | { status: 'idle'; data?: undefined; error?: undefined }
  | { status: 'loading'; data?: ServerSettingsDoc; error?: undefined }
  | { status: 'ready'; data: ServerSettingsDoc; error?: undefined }
  | { status: 'error'; data?: ServerSettingsDoc; error: ApiError }

interface AppStore {
  prefs: LocalPrefs
  setPrefs: (patch: Partial<LocalPrefs>) => void

  serverSettings: ServerSettingsState
  refreshServerSettings: (opts?: { silent?: boolean }) => Promise<boolean>
  updateServerSettings: (patch: Partial<ServerSettingsDoc>) => Promise<void>
}

const Ctx = createContext<AppStore | null>(null)

export function AppStoreProvider({ children }: { children: React.ReactNode }) {
  const [prefs, setPrefsState] = useState<LocalPrefs>(() => loadPrefs())
  const prefsRef = useRef(prefs)
  prefsRef.current = prefs
  const [serverSettings, setServerSettings] = useState<ServerSettingsState>({
    status: 'idle',
  })

  // 请求代次提交（Q-CQ-B08-003）：每次发起 GET/PUT 递增代次，响应回调先校验代次是否仍最新，
  // 过期响应（逆序完成的旧请求）只丢弃状态写入与提示，保证仅最新 generation 能覆盖状态，
  // 避免“慢 GET 后完成覆盖快 GET/PUT 结果”的竞态。
  const settingsGenRef = useRef(0)

  const setPrefs = (patch: Partial<LocalPrefs>) => {
    setPrefsState((prev) => {
      const next = { ...prev, ...patch }
      // 兜底：避免被错误写入破坏启动
      const safe: LocalPrefs = {
        ...defaultPrefs,
        ...next,
      }
      savePrefs(safe)
      return safe
    })
  }

  const refreshServerSettings = useCallback(async (opts?: { silent?: boolean }) => {
    const silent = !!opts?.silent
    // 代次提交：本次请求独占一个递增代次，作为“是否仍最新”的判定依据
    const gen = ++settingsGenRef.current
    setServerSettings((prev) =>
      silent
        ? prev.status === 'idle'
          ? { status: 'loading' }
          : prev
        : { status: 'loading', data: prev.data },
    )

    let hide: (() => void) | undefined
    if (!silent) {
      hide = notify.loading('正在刷新后端设置...')
    }

    try {
      const env = await getServerSettings(prefsRef.current)

      if (!env.ok) {
        throw new ApiError(env.error.code, env.error.message, {
          details: env.error.details,
        })
      }
      // 过期响应：后续已发起更新代次的请求，状态由新代次写入，不再重复覆盖
      if (gen !== settingsGenRef.current) return true
      setServerSettings({ status: 'ready', data: env.data })
      if (!silent) notify.success('操作成功')
      return true
    } catch (e: unknown) {
      const err = toApiError(e)
      // 过期失败同样不污染新状态（新请求可能已成功）
      if (gen === settingsGenRef.current) {
        setServerSettings((prev) => ({ status: 'error', data: prev.data, error: err }))
        if (!silent) notify.error(err.message)
      }
      return false
    } finally {
      if (hide) hide()
    }
  }, [])

  const updateServerSettings = useCallback(async (patch: Partial<ServerSettingsDoc>) => {
    // 代次提交：PUT 同样参与代次递增，过期 PUT 响应不覆盖更新的代次
    const gen = ++settingsGenRef.current
    setServerSettings((prev) => ({
      status: 'loading',
      data: prev.data,
    }))
    try {
      const env = await putServerSettings(prefsRef.current, patch)

      if (!env.ok) {
        throw new ApiError(env.error.code, env.error.message, {
          details: env.error.details,
        })
      }
      if (gen === settingsGenRef.current) {
        setServerSettings({ status: 'ready', data: env.data })
      }
    } catch (e: unknown) {
      const err = toApiError(e)
      if (gen === settingsGenRef.current) {
        setServerSettings((prev) => ({ status: 'error', data: prev.data, error: err }))
      }
      throw err
    }
  }, [])

  const value = useMemo<AppStore>(
    () => ({
      prefs,
      setPrefs,
      serverSettings,
      refreshServerSettings,
      updateServerSettings,
    }),
    [prefs, serverSettings, refreshServerSettings, updateServerSettings],
  )

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>
}

export function useAppStore() {
  const v = useContext(Ctx)
  if (!v) throw new Error('useAppStore 必须在 AppStoreProvider 内使用')
  return v
}

