import type { LocalPrefs } from '../state/prefs'
import { fetchJson } from './http'
import { API } from './paths'

export type OkEnvelope = { ok: true }

export async function enroll(prefs: LocalPrefs, input: { personId: string }): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>(API.enroll, {
    method: 'POST',
    body: JSON.stringify(input),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function clearDb(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>(API.clearDb, {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function openPrivacySettings(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>(API.openPrivacy, {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function setFlip(prefs: LocalPrefs, input: { flipX: boolean; flipY: boolean }): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>(API.flip, {
    method: 'PUT',
    body: JSON.stringify(input),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function rotateCryptoKey(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>(API.rotateKey, {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

