// 服务端 settings 文档 → 表单模型 的纯映射（无 React / 无 antd 依赖 ⇒ 可在 node 环境直接单测）。
//
// 为什么单独成模块：本映射是「表单能读到哪些字段」的唯一定义点。原先它内联在
//   ServerSettingsTab.tsx 内，测试若想覆盖它就必然连带把 antd 整棵树导进 node 测试环境；
//   拆开后视图（ServerSettingsTab）与模型（本文件）各自可测。
// ⚠️ 覆盖边界（含 PreviewPage 的并行入口、路径字段的 PUT 校验、各值域来源）写在
//   ServerSettingsTab.tsx 顶部的注释块里，改本文件前请一并读。
import type { ServerSettingsDoc } from '../api/types'

// 表单模型 = 服务端 settings 文档去掉元字段（schemaVersion / _error）。
// 用 Omit 而非手抄一遍字段：types.ts 是 settings 契约的单一来源，手抄一份会在契约演进时
//   静默漂移（本表单正因此曾只覆盖 11 段中的 5 段）。用 Omit 后，types.ts 增删段/字段会让
//   下方 buildServerFormModelFromDoc 立即**编译报错**（缺字段），而不是运行时静默漏段。
export type ServerFormModel = Omit<ServerSettingsDoc, 'schemaVersion' | '_error'>

// ServerSettingsDoc 的段名清单，顺序与后端 buildSettingsJson
//   （src/win/src/WinJsonConfig.cpp 的 toSettingsDocObject）写入顺序一致。
// 用途：文档健全性判据（见 isSettingsDocComplete）。
export const SETTINGS_SECTIONS = [
  'camera',
  'recognition',
  'inference',
  'dnn',
  'model',
  'http',
  'poster',
  'log',
  'ui',
  'display',
  'acceleration',
] as const

// 文档是否带齐 SETTINGS_SECTIONS 的全部段（不查段内字段是否存在）。
// 后端 GET 的正常路径**恒**返回全部段（toSettingsDocObject 无条件写入每一段，见上方顺序清单），
//   只有错误 stub（`{"schemaVersion":1,"_error":"key_error"|"encrypt_error"}`）才会缺段。
// 为什么要这个判据：它是 ServerSettingsTab 的 formReady 闸门的一半输入——文档缺失时禁用整份表单，
//   避免用户在"看起来有值（其实是兜底值）"的表单上编辑、再撞一次后端 400。
// ⚠️ 更正一处旧措辞（原称"缺段的文档若被允许提交，兜底默认值会被当成用户的编辑写回去、覆盖真实配置"）：
//   不成立——baseline 与表单模型由**同一份文档、同一个 buildServerFormModelFromDoc** 构造
//   （见 ServerSettingsTab 的同步 effect），兜底值在两侧同值 ⇒ diffPatch 恒判"未变更"。
export function isSettingsDocComplete(doc: ServerSettingsDoc | undefined): boolean {
  if (!doc) return false
  return SETTINGS_SECTIONS.every((section) => doc[section] != null)
}

// 后端脱敏哨兵（src/win/src/WinJsonConfig.cpp 的 kMaskedSentinel）：GET **与 PUT** 的响应体都经
//   WinJsonConfigStore::currentRedactedJsonPretty（redacted=true，见 JsonEndpointHandlers 的
//   handleGetSettings / handlePutSettings）⇒ 前端从任意一侧拿到的 poster.postUrl 都是该字面量，
//   前端据此判断"看到的是掩码而非真值"（生产路径下 masked 因此恒为 true）。
const MASKED_SENTINEL = '***'

// 把后端文档映射为表单模型。
// ⚠️ 兜底默认值与 src/win/include/rk_win/WinConfig.h 的 AppConfig 成员初始化值**并非整体一致**：
//   旧措辞称"除路径字段外一致"，不成立——例外是**成类**的，按本函数的两种写法划分：
//   · 字符串字段一律 `String(… ?? '')` ⇒ 凡 AppConfig 里默认**非空**的字符串都退化为空串
//     （model 段的 detection / recognition / backend / detectorBackend / recognitionBackend，
//      以及 poster.postUrl）。唯一的字符串例外是 inference.throttleMode（兜底 'auto'，与 AppConfig 同值）；
//   · 布尔字段一律 `!!` ⇒ 凡 AppConfig 里默认 **true** 的都退化为 false
//     （dnn.enable、model.autoFallback、http.enable、display.vsync、display.enableSRGB）；
//   · 路径字段（recognition 的 cascadePath / databasePath / arcFaceModelPath、dnn 的 modelPath /
//     configPath、log.logDir）退化为空串另有独立原因：在"config.json 缺失"的兜底路径上，后端由
//     WinConfig.cpp 的 loadConfigFromIniOrDefault 以 INI 默认值 + resolvePathFromExeDir 落成
//     **绝对路径**（空串在后端语义 = "未设置"），前端无从复刻；
//   · 数值字段与 AppConfig **逐一同值**（即本函数所有 `?? 数字` 的写法）。
//   要核对时以 WinConfig.h 的成员初始化值为准逐段对照，勿以本注释的列举为准。
// 为什么本批不把兜底改成与 AppConfig 一致（选"改声明"而非"改实现"）：这些默认值只在文档残缺时生效，
//   而 ① 残缺文档被 formReady 闸门挡住；② baseline 与表单模型同源 ⇒ diffPatch 对兜底值恒判"未变更"。
//   ⇒ 它们在本批**可达路径上行为不可达**，改实现只会换来"前端兜底值 vs 后端生效默认值"的新一轮漂移。
// ⚠️ 这些默认值只在文档残缺（段或段内字段缺失）时生效；正常路径文档恒完整。
export function buildServerFormModelFromDoc(doc: ServerSettingsDoc): {
  model: ServerFormModel
  postUrlMasked: boolean
} {
  const masked = doc.poster?.postUrl === MASKED_SENTINEL
  return {
    postUrlMasked: masked,
    model: {
      camera: {
        preferredDeviceId: String(doc.camera?.preferredDeviceId ?? ''),
        width: Number(doc.camera?.width ?? 640),
        height: Number(doc.camera?.height ?? 480),
        fps: Number(doc.camera?.fps ?? 30),
      },
      recognition: {
        cascadePath: String(doc.recognition?.cascadePath ?? ''),
        databasePath: String(doc.recognition?.databasePath ?? ''),
        minFaceSizePx: Number(doc.recognition?.minFaceSizePx ?? 60),
        identifyThreshold: Number(doc.recognition?.identifyThreshold ?? 55),
        identifyThresholdArcFace: Number(doc.recognition?.identifyThresholdArcFace ?? 0.45),
        enrollSamples: Number(doc.recognition?.enrollSamples ?? 12),
        arcFaceModelPath: String(doc.recognition?.arcFaceModelPath ?? ''),
        arcFaceInputName: String(doc.recognition?.arcFaceInputName ?? ''),
        arcFaceOutputName: String(doc.recognition?.arcFaceOutputName ?? ''),
      },
      inference: {
        throttleMode: String(doc.inference?.throttleMode ?? 'auto'),
        intervalMs: Number(doc.inference?.intervalMs ?? 150),
      },
      dnn: {
        enable: !!doc.dnn?.enable,
        modelPath: String(doc.dnn?.modelPath ?? ''),
        configPath: String(doc.dnn?.configPath ?? ''),
        inputWidth: Number(doc.dnn?.inputWidth ?? 300),
        inputHeight: Number(doc.dnn?.inputHeight ?? 300),
        scale: Number(doc.dnn?.scale ?? 1),
        meanB: Number(doc.dnn?.meanB ?? 104),
        meanG: Number(doc.dnn?.meanG ?? 177),
        meanR: Number(doc.dnn?.meanR ?? 123),
        swapRB: !!doc.dnn?.swapRB,
        confThreshold: Number(doc.dnn?.confThreshold ?? 0.5),
        backend: Number(doc.dnn?.backend ?? 0),
        target: Number(doc.dnn?.target ?? 0),
      },
      model: {
        detection: String(doc.model?.detection ?? ''),
        recognition: String(doc.model?.recognition ?? ''),
        backend: String(doc.model?.backend ?? ''),
        detectorBackend: String(doc.model?.detectorBackend ?? ''),
        recognitionBackend: String(doc.model?.recognitionBackend ?? ''),
        autoFallback: !!doc.model?.autoFallback,
        int8Enabled: !!doc.model?.int8Enabled,
      },
      http: { enable: !!doc.http?.enable, port: Number(doc.http?.port ?? 8080) },
      poster: {
        enable: !!doc.poster?.enable,
        // 哨兵 "***" 映射为空串，语义 = "未填写/不变更"（真实 URL 永不出现在前端）。
        // ⚠️ **已知限制**（旧措辞"真值仍在 baseline 里"不成立）：baseline 与本模型同源——同一份脱敏文档、
        //   由同一个本函数构造（ServerSettingsTab 的同步 effect 把返回的 model 同时喂给 setServerBaseline
        //   与 setFieldsValue）⇒ `serverBaseline.poster.postUrl` 同样是空串。于是 ServerSettingsTab 里那句
        //   `postUrlMasked && trim()==='' ? serverBaseline.poster.postUrl : ...` 在生产上恒等于取 `''`，
        //   **没有**"恢复真值"的作用；连带后果是：用户在 UI 里**清空** postUrl 会被静默吞掉
        //   （归一化回 baseline `''` ⇒ diffPatch 判未变更 ⇒ 不提交、且无任何提示）。
        //   这是**前端表达能力**的缺口而非后端契约问题：后端接受空串（WinJsonConfig 的 getString 不拒空串，
        //   且 HttpFacesPoster::start 对空 URL 直接返回 false = 等价于停发）。
        //   本批**不修**，理由（不是"懒得修"）：要让清空可达，baseline 必须持有与"未填"不同的值——因为
        //   diffPatch 比的是值与值，若 baseline 仍是空串，无论用 touched 还是别的判据，归一化结果依旧等于
        //   baseline ⇒ 仍判未变更；唯一低成本做法是让本分支返回哨兵本身（把 "***" 显示进输入框），属可见
        //   行为变更，且会让"不把哨兵发回后端"从**前端自保**退化为**依赖后端 stripMaskedSentinels 兜底**
        //   （后端确有该兜底，但它一旦被改动，字面量就会覆盖并重加密真实密文）。
        //   ⇒ 登记为**待办**（不在本批闭合，改法超出"改注释/加纯逻辑测试"的范围）：若产品确需"经 UI 清空
        //     postUrl"，须先定 UI 语义（例如让该输入框显示哨兵、并以"留空=显式清空"呈现），再同批改本分支
        //     + ServerSettingsTab 的归一化映射 + 本文件的脱敏用例（现例断言哨兵→空串）。
        postUrl: masked ? '' : String(doc.poster?.postUrl ?? ''),
        throttleMs: Number(doc.poster?.throttleMs ?? 100),
        backoffMinMs: Number(doc.poster?.backoffMinMs ?? 200),
        backoffMaxMs: Number(doc.poster?.backoffMaxMs ?? 5000),
      },
      log: {
        logDir: String(doc.log?.logDir ?? ''),
        maxFileBytes: Number(doc.log?.maxFileBytes ?? 10 * 1024 * 1024),
        maxRollFiles: Number(doc.log?.maxRollFiles ?? 5),
      },
      ui: {
        windowWidth: Number(doc.ui?.windowWidth ?? 1280),
        windowHeight: Number(doc.ui?.windowHeight ?? 800),
        previewScaleMode: Number(doc.ui?.previewScaleMode ?? 0),
      },
      display: {
        outputIndex: Number(doc.display?.outputIndex ?? 0),
        width: Number(doc.display?.width ?? 0),
        height: Number(doc.display?.height ?? 0),
        refreshNumerator: Number(doc.display?.refreshNumerator ?? 0),
        refreshDenominator: Number(doc.display?.refreshDenominator ?? 1),
        vsync: !!doc.display?.vsync,
        swapchainBuffers: Number(doc.display?.swapchainBuffers ?? 2),
        fullscreen: !!doc.display?.fullscreen,
        allowSystemModeSwitch: !!doc.display?.allowSystemModeSwitch,
        enableSRGB: !!doc.display?.enableSRGB,
        gamma: Number(doc.display?.gamma ?? 2.2),
        colorTempK: Number(doc.display?.colorTempK ?? 6500),
        aaSamples: Number(doc.display?.aaSamples ?? 1),
        anisoLevel: Number(doc.display?.anisoLevel ?? 1),
      },
      acceleration: {
        enableOpenCL: !!doc.acceleration?.enableOpenCL,
        enableLibyuv: !!doc.acceleration?.enableLibyuv,
        enableMpp: !!doc.acceleration?.enableMpp,
        enableQualcomm: !!doc.acceleration?.enableQualcomm,
      },
    },
  }
}
