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
    // ArcFace 专用阈值（余弦相似度，0..1）；与 LBPH 的 identifyThreshold 量纲不同，独立成键
    identifyThresholdArcFace: number
    enrollSamples: number
    arcFaceModelPath: string
    // ArcFace OpenCV-DNN 节点名：默认空串 = 由 OpenCV 自动取默认输入 / 首个输出
    arcFaceInputName: string
    arcFaceOutputName: string
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
    int8Enabled: boolean
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
  configuredPath?: string
  resolvedPath?: string
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
  // 装配成败（data 内字段，与 Envelope 根部的 ok 语义不同）：
  // ok:true = 重载后运行时装配成功；ok:false = 装配失败（识别器未能初始化）。
  // `status:"not_found"` 分支不带该字段，故为可选。
  ok?: boolean
  generation?: number
  warning?: string
}

// --- GET /api/v1/health ---
// 对齐后端 handleGetHealth 的输出（src/win/src/JsonEndpointHandlers.cpp:337）。
// ⚠️ `configStatus`（配置装载）与 `status`（运行时装配）是**不同的量**，并存且互不覆盖。
export type HealthConfigStatus = 'ok' | 'degraded' | 'rejected' | 'unknown'
export type HealthRuntimeStatus = 'ok' | 'degraded' | 'starting'

export interface HealthData {
  name: string
  port: number
  configStatus: HealthConfigStatus
  configWarning?: string
  configQuarantinedTo?: string
  status: HealthRuntimeStatus
  availableFeatures: string[]
  // 仅在 status === 'degraded' 时出现：降级原因码（加速契约 7 个固定码之一）
  reasonCode?: string
  // 仅在降级且文本非空时出现：RuntimeBootstrap 告警原文（可能含文件系统路径）
  warning?: string
  // 仅在降级时出现：**首次**进入降级的时间（本地时间，无时区后缀）
  degradedSince?: string
}

// --- GET /api/v1/faces（当前帧人脸快照，**不套 Envelope**）---
// 对齐 src/win/src/FacesJson.cpp buildFacesJson()。⚠️ 不含 personId 等身份字段。
export interface FaceBox {
  x: number
  y: number
  w: number
  h: number
}

export interface FaceDetection {
  bbox: FaceBox // 帧坐标系
  display_bbox: FaceBox // 预览坐标系（按 preview 尺寸/缩放模式映射后）
  confidence: number | null // 检测置信度；后端在非有限值时输出 null
}

export interface FacesPerf {
  infer_ms: number | null
  drop_rate: number | null
  stride: number
}

export interface FacesFrameSize {
  w: number
  h: number
}

export interface FacesPreviewSize {
  w: number
  h: number
  scale_mode: string // 'letterbox' | 'crop_fill'
}

export interface FacesSnapshot {
  timestamp_ms: number
  frame: FacesFrameSize
  preview: FacesPreviewSize
  perf: FacesPerf
  faces: FaceDetection[]
}

