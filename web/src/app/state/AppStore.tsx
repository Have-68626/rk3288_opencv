/* eslint-disable react-refresh/only-export-components */
import React, { createContext, useCallback, useContext, useMemo, useState } from 'react'
import { message } from 'antd'

import type { ServerSettingsDoc } from '../api/types'
import { getServerSettings, putServerSettings } from '../api/settings'
import type { LocalPrefs } from './prefs'
import { defaultPrefs, loadPrefs, savePrefs } from './prefs'
import { ApiError } from '../api/http'

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
  updateServerSettings: (patch: unknown) => Promise<void>
}

const Ctx = createContext<AppStore | null>(null)

export function AppStoreProvider({ children }: { children: React.ReactNode }) {
  const [prefs, setPrefsState] = useState<LocalPrefs>(() => loadPrefs())
  const [serverSettings, setServerSettings] = useState<ServerSettingsState>({
    status: 'idle',
  })

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
    setServerSettings((prev) =>
      silent
        ? prev.status === 'idle'
          ? { status: 'loading' }
          : prev
        : { status: 'loading', data: prev.data },
    )

    try {
      const env = await getServerSettings(prefs)

      if (!env.ok) {
        throw new ApiError(env.error.code, env.error.message, {
          details: env.error.details,
        })
      }
      setServerSettings({ status: 'ready', data: env.data })
      if (!silent) message.success('Operation successful')
      return true
    } catch (e: unknown) {
      const err = e instanceof ApiError ? e : new ApiError('unknown', (e as Error)?.message || '未知错误')
      setServerSettings((prev) => ({ status: 'error', data: prev.data, error: err }))
      if (!silent) message.error(err.message)
      return false
    }
  }, [prefs])

  const updateServerSettings = useCallback(async (patch: unknown) => {
    setServerSettings((prev) => ({
      status: 'loading',
      data: prev.data,
    }))
    try {
      let timeoutId: number | undefined
      const timeoutPromise = new Promise<never>((_, reject) =>
        timeoutId = setTimeout(() => reject(new Error('Operation timed out (> 5s)')), 5000) as unknown as number
      )
      const env = await Promise.race([
        putServerSettings(prefs, patch).finally(() => clearTimeout(timeoutId)),
        timeoutPromise
      ])

      if (!env.ok) {
        throw new ApiError(env.error.code, env.error.message, {
          details: env.error.details,
        })
      }
      setServerSettings({ status: 'ready', data: env.data })
      message.success('Settings applied')
    } catch (e: unknown) {
      const err = e instanceof ApiError ? e : new ApiError('unknown', (e as Error)?.message || '未知错误')
      setServerSettings((prev) => ({ status: 'error', data: prev.data, error: err }))
      message.error(err.message)
      throw err
    }
  }, [prefs])

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

