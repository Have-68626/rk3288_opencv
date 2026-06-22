import type { ApiEnvelope, ModelsResponse, ReloadResult } from './types'
import { fetchJson } from './http'
import type { LocalPrefs } from '../state/prefs'

const API_BASE = (import.meta.env.VITE_API_BASE as string | undefined) || ''

export async function getModels(prefs: LocalPrefs) {
  return fetchJson<ApiEnvelope<ModelsResponse>>(`${API_BASE}/api/v1/models`, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function reloadModel(prefs: LocalPrefs, id: string) {
  return fetchJson<ApiEnvelope<ReloadResult>>(`${API_BASE}/api/v1/models/reload`, {
    method: 'POST',
    body: JSON.stringify({ id }),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}
