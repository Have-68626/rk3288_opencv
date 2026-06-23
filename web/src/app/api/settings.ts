import type { ApiEnvelope, ServerSettingsDoc } from './types'
import { fetchJson } from './http'
import type { LocalPrefs } from '../state/prefs'
import { API } from './paths'

export async function getServerSettings(prefs: LocalPrefs) {
  return fetchJson<ApiEnvelope<ServerSettingsDoc>>(API.settings, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: prefs.cacheStrategy,
    cacheKey: 'v1_settings',
  })
}

export async function putServerSettings(prefs: LocalPrefs, patch: unknown) {
  return fetchJson<ApiEnvelope<ServerSettingsDoc>>(API.settings, {
    method: 'PUT',
    body: JSON.stringify(patch),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}
