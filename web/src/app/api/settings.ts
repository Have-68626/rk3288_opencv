import type { ApiEnvelope, ServerSettingsDoc } from './types'
import { fetchJson } from './http'
import type { LocalPrefs } from '../state/prefs'

// 统一从同源访问：生产形态由 CivetWeb 托管静态文件 + 同端口提供 /api/v1/*
// 开发形态由 Vite 代理到后端（见 web/vite.config.ts）。
const API_BASE = (import.meta.env.VITE_API_BASE as string | undefined) || ''

export async function getServerSettings(prefs: LocalPrefs) {
  return fetchJson<ApiEnvelope<ServerSettingsDoc>>(`${API_BASE}/api/v1/settings`, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: prefs.cacheStrategy,
    cacheKey: 'v1_settings',
  })
}

export async function putServerSettings(prefs: LocalPrefs, patch: unknown) {
  return fetchJson<ApiEnvelope<ServerSettingsDoc>>(`${API_BASE}/api/v1/settings`, {
    method: 'PUT',
    body: JSON.stringify(patch),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache', // PUT 一律不走缓存，避免“写了但读到旧值”的错觉
  })
}

