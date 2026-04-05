import type { LocalPrefs } from '../state/prefs'
import { fetchJson } from './http'

export type OkEnvelope = { ok: true }

export async function enroll(prefs: LocalPrefs, input: { personId: string }): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>('/api/v1/actions/enroll', {
    method: 'POST',
    body: JSON.stringify(input),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function clearDb(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>('/api/v1/actions/db/clear', {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function openPrivacySettings(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>('/api/v1/actions/privacy/open', {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function setFlip(prefs: LocalPrefs, input: { flipX: boolean; flipY: boolean }): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>('/api/v1/camera/flip', {
    method: 'PUT',
    body: JSON.stringify(input),
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

export async function rotateCryptoKey(prefs: LocalPrefs): Promise<OkEnvelope> {
  return fetchJson<OkEnvelope>('/api/v1/actions/crypto/rotate', {
    method: 'POST',
    body: '{}',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: 'no-cache',
  })
}

