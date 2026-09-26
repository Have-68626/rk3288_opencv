import type { ApiEnvelope, HealthData } from './types'
import { fetchJson } from './http'
import type { LocalPrefs } from '../state/prefs'
import { API } from './paths'

// GET /api/v1/health：匿名白名单端点，返回统一 Envelope（data 为诊断信息）。
export async function getHealth(prefs: LocalPrefs) {
  return fetchJson<ApiEnvelope<HealthData>>(API.health, {
    method: 'GET',
    timeoutMs: prefs.apiTimeoutMs,
    logLevel: prefs.logLevel,
    // 健康状态随装配/重载变化，缓存会掩盖真实降级，故不缓存。
    cacheStrategy: 'no-cache',
  })
}

// reasonCode → 可读中文。取值域为加速契约的 7 个固定码（见 openapi.yaml /api/v1/health）。
// 未知码原样回显：既不臆造文案，也不静默吞掉服务端可能新增的码。
const REASON_CODE_TEXT: Record<string, string> = {
  ok: '无降级',
  build_disabled: '构建期未启用该能力',
  unsupported_platform: '当前平台不支持',
  missing_dependency: '缺少依赖库',
  missing_model: '缺少模型文件',
  runtime_init_failed: '运行时初始化失败',
  unsupported_input: '输入不受支持',
}

export function describeReasonCode(code?: string): string | undefined {
  if (!code) return undefined
  return REASON_CODE_TEXT[code] ?? code
}
