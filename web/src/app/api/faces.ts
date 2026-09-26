import type { FacesSnapshot } from './types'
import { fetchRawJson } from './http'
import type { LocalPrefs } from '../state/prefs'
import { API } from './paths'

// GET /api/v1/faces：直接返回**当前帧**人脸快照对象，**不套 Envelope**
// （见 openapi.yaml 与 src/win/src/FacesJson.cpp buildFacesJson），故走 fetchRawJson。
// 失败（如 Pipeline 不可用 503，其错误体为 Envelope 形状）会抛 ApiError。
export async function getFacesSnapshot(prefs: LocalPrefs): Promise<FacesSnapshot> {
  return fetchRawJson<FacesSnapshot>(API.faces, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
  })
}
