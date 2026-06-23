import type { LocalPrefs } from '../state/prefs'
import { fetchJson } from './http'

export type CameraFormatInfo = { w: number; h: number; fps: number }
export type CameraDeviceInfo = {
  index: number
  name: string
  deviceId: string
  formats: CameraFormatInfo[]
}

export type CamerasOk = { ok: true; data: { devices: CameraDeviceInfo[] } }
export type CamerasErr = { ok: false; error: { code: string; message: string; details?: string[] } }
export type CamerasEnvelope = CamerasOk | CamerasErr

export async function getCameras(prefs: LocalPrefs): Promise<CamerasEnvelope> {
  return fetchJson<CamerasEnvelope>('/api/v1/cameras', {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    cacheStrategy: prefs.cacheStrategy,
    cacheKey: 'cameras',
  })
}

