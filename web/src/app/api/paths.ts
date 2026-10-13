/** Central API path constants. All modules MUST use these instead of literal strings. */
const BASE = (import.meta.env.VITE_API_BASE as string | undefined) || ''

export const API = {
  // Health
  health: `${BASE}/api/v1/health`,

  // Faces（当前帧人脸快照；**不套 Envelope**）
  faces: `${BASE}/api/v1/faces`,

  // Cameras
  cameras: `${BASE}/api/v1/cameras`,

  // Settings
  settings: `${BASE}/api/v1/settings`,
  settingsValidate: `${BASE}/api/v1/settings/validate`,

  // Models
  models: `${BASE}/api/v1/models`,
  modelsReload: `${BASE}/api/v1/models/reload`,

  // Actions
  enroll: `${BASE}/api/v1/actions/enroll`,
  clearDb: `${BASE}/api/v1/actions/db/clear`,
  openPrivacy: `${BASE}/api/v1/actions/privacy/open`,
  flip: `${BASE}/api/v1/camera/flip`,
  rotateKey: `${BASE}/api/v1/actions/crypto/rotate`,
} as const
