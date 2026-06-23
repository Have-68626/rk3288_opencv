import type { ApiEnvelope, ModelsResponse, ReloadResult } from './types'
import { fetchJson } from './http'
import type { LocalPrefs } from '../state/prefs'
import { API } from './paths'

export async function getModels(prefs: LocalPrefs) {
  return fetchJson<ApiEnvelope<ModelsResponse>>(API.models, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function reloadModel(prefs: LocalPrefs, id: string) {
  return fetchJson<ApiEnvelope<ReloadResult>>(API.modelsReload, {
    method: 'POST',
    body: JSON.stringify({ id }),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}
