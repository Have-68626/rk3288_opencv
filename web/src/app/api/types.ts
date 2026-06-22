export interface ApiOk<T> {
  ok: true
  data: T
}

export interface ApiErr {
  ok: false
  error: {
    code: string
    message: string
    details?: string[]
  }
}

export type ApiEnvelope<T> = ApiOk<T> | ApiErr

export type ServerSettingsSchemaVersion = 1

// 对齐后端 WinJsonConfigStore::buildSettingsJson() 的输出（见 src/win/src/WinJsonConfig.cpp）。
// 注意：这里是“脱敏输出”版本；敏感字段（例如 poster.postUrl）在后端可能会被脱敏/加密落盘。
export interface ServerSettingsDoc {
  schemaVersion: ServerSettingsSchemaVersion
  _error?: string

  camera: {
    preferredDeviceId: string
    width: number
    height: number
    fps: number
  }
  recognition: {
    cascadePath: string
    databasePath: string
    minFaceSizePx: number
    identifyThreshold: number
    enrollSamples: number
  }
  inference: {
    throttleMode: string
    intervalMs: number
  }
  dnn: {
    enable: boolean
    modelPath: string
    configPath: string
    inputWidth: number
    inputHeight: number
    scale: number
    meanB: number
    meanG: number
    meanR: number
    swapRB: boolean
    confThreshold: number
    backend: number
    target: number
  }
  model: {
    detection: string
    recognition: string
    backend: string
    detectorBackend: string
    recognitionBackend: string
    autoFallback: boolean
  }
  http: {
    enable: boolean
    port: number
  }
  poster: {
    enable: boolean
    postUrl: string
    throttleMs: number
    backoffMinMs: number
    backoffMaxMs: number
  }
  log: {
    logDir: string
    maxFileBytes: number
    maxRollFiles: number
  }
  ui: {
    windowWidth: number
    windowHeight: number
    previewScaleMode: number
  }
  display: {
    outputIndex: number
    width: number
    height: number
    refreshNumerator: number
    refreshDenominator: number
    vsync: boolean
    swapchainBuffers: number
    fullscreen: boolean
    allowSystemModeSwitch: boolean
    enableSRGB: boolean
    gamma: number
    colorTempK: number
    aaSamples: number
    anisoLevel: number
  }
  acceleration: {
    enableOpenCL: boolean
    enableLibyuv: boolean
    enableMpp: boolean
    enableQualcomm: boolean
  }
}

// --- GET /api/v1/models ---
export interface ModelInfo {
  id: string
  displayName: string
  taskType: string
  notes: string
  recommendedFor: string
}

export interface ActiveModelInfo {
  id: string
  displayName: string
  taskType: string
  configuredPath: string
  resolvedPath: string
  backend: string
  hash?: string
  status: 'loaded' | 'failed' | 'missing'
  isInUse: boolean
  modelVersion?: string
  lastError?: string
}

export interface ModelSummary {
  totalSupported: number
  totalConfigured: number
  totalLoaded: number
  totalFailed: number
  totalMissing: number
}

export interface ModelsResponse {
  supportedModels: ModelInfo[]
  activeModels: ActiveModelInfo[]
  summary: ModelSummary
}

export interface ReloadResult {
  id: string
  status: string
}

