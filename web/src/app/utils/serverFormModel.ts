// 服务端 settings 文档 → 表单模型 的纯映射（无 React / 无 antd 依赖 ⇒ 可在 node 环境直接单测）。
//
// 为什么单独成模块：本映射是「表单能读到哪些字段」的唯一定义点。原先它内联在
//   ServerSettingsTab.tsx 内，测试若想覆盖它就必然连带把 antd 整棵树导进 node 测试环境；
//   拆开后视图（ServerSettingsTab）与模型（本文件）各自可测。
// ⚠️ 覆盖边界（含 PreviewPage 的并行入口、路径字段的 PUT 校验、各值域来源）写在
//   ServerSettingsTab.tsx 顶部的注释块里，改本文件前请一并读。
import type { ServerSettingsDoc } from '../api/types'
// 值导入（非 type）：buildSettingsSubmission 内部要做真实 diff，见其 doc。
import { diffPatch } from './diffPatch'

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
        // ★ 哨兵 "***" **原样留在模型里**（本次修复；旧行为是映射为空串）。
        //   为什么必须留在模型里：diffPatch 比的是**值与值**。只有「字段初值」与「用户清空后的值」
        //   不同，「用户没动」与「用户清空」才可能被判成不同 ⇒ 清空才可能被提交。旧实现把哨兵映射
        //   成空串，于是清空后的 '' 与初值 '' 相等 ⇒ 恒判「未变更」⇒ **清空被吞掉**
        //   （并提示「没有检测到变更」，属**误报**而非完全静默）。
        // ⚠️ 旧登记的「已知限制」**已由本次修复闭合**，成因与修法留档（勿再按旧结论理解本字段）：
        //   · 成因（旧实现）：baseline 与本模型**同源**——同一份脱敏文档、由同一个本函数构造
        //     （ServerSettingsTab 的同步 effect 把返回的 model 同时喂给 setServerBaseline 与
        //     setFieldsValue）⇒ `serverBaseline.poster.postUrl` 同样是空串 ⇒ 那边归一化里的
        //     `postUrlMasked && trim()==='' ? serverBaseline.poster.postUrl : …` **恒等于取 ''**，
        //     那句「恢复真值」从未起过作用。
        //   · 修法：初值 = 哨兵本身（输入框里可见 "***"）⇒ 用户删空后 trim 得 '' ≠ 哨兵 ⇒ 判为变更
        //     ⇒ 提交一次**显式清空**（后端接受空串：WinJsonConfig 的 getString 不拒空串，且
        //     HttpFacesPoster::start 对空 URL 返回 false = 等价于停发）。归一化见 normalizePostUrlField。
        //   · 代价（如实登记，勿高估也不要低估）：
        //     ① 输入框会**显示 "***"**，属可见的 UX 变化；
        //     ② 「哨兵永不被提交」由 normalizePostUrlField 的**显式不变量**保证（用例钉住），前端这一层
        //        **仍在**；后端 stripMaskedSentinels 对**本表单**是冗余的第二层，但对**非 UI 客户端**
        //        （curl / 脚本直接回写）它仍是**唯一**防线 —— 故**不得**因本修复而削弱那层兜底；
        //     ③ 清空成功后**当场**就会复显 "***"（**不是**"下次手动拉取才"）：PUT 的响应体本身即脱敏
        //        （JsonEndpointHandlers 的 handlePutSettings 走 currentRedactedJsonPretty），而
        //        AppStore.updateServerSettings 成功后会用它刷新 serverSettings.data ⇒ 本组件的同步
        //        effect 立刻重填表单与 baseline。根因是 GET/PUT **总是**把该字段脱敏成 "***"
        //        （toSettingsDocObject 的 redacted 分支不看真值）⇒ 界面**无法**区分「已清空」与
        //        「已设置」。该不可区分性源自后端脱敏设计本身，不是本表单的缺陷，也无法在不改契约的
        //        前提下消除；
        //     ④ 字段初值 "***" 使「误删一两个字符」成为就近手势（得 "*" / "**"）—— 已由
        //        normalizePostUrlField 的「全 `*` 一律按未改动」判据兜住（见该函数 doc 的事故推导）。
        postUrl: masked ? MASKED_SENTINEL : String(doc.poster?.postUrl ?? ''),
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

// 提交前的 postUrl 归一化（纯函数，node 环境可直接单测）。语义与不变量：
//   · 一般情形：`trim` 后的值原样返回 ⇒ **空串 = 显式清空**（后端接受空串，见 buildServerFormModelFromDoc
//     里 postUrl 字段的登记：getString 不拒空串，HttpFacesPoster::start 对空 URL 返回 false = 停发）。
//   · ★ 不变量一：值为哨兵 "***" 或**任意只由 `*` 组成的串**时，一律**返回 baseline**。
//     于是：① 用户**没动**该字段 ⇒ 返回值仍等于 baseline ⇒ diffPatch 判未变更 ⇒ **不提交**；
//     ② 哨兵**永不**被当作要提交的值送出——这条保证在**前端**成立。
//     ⚠️「只由 `*` 组成」这一条是**为某个真实事故面**加的（勿当作多余的宽容）：字段初值是 "***"，
//     用户误删一两个字符就会得到 "*" / "**"——它们**不等于**哨兵，若原样送出会：
//     后端 `stripMaskedSentinels` 只剔精确 "***"（不剔 "*"）⇒ schema 的串分支无 pattern ⇒ 落盘覆盖
//     **真实 URL**，而运行期 `HttpFacesPoster` 只判空、于是起线程后 `WinHttpCrackUrl("*")` 失败 ⇒
//     上报静默失效；更糟的是 **GET 恒脱敏**，用户**再也拿不回**被覆盖的 URL。所以这一类的判据必须是
//     「它不可能是合法 URL」而不是「它等于哨兵」——合法 URL 不可能只由 `*` 组成，故不会误伤。
//   · ★ 不变量二：非脱敏场景（baseline 持真值）下行为与修复前**逐字一致**（trim 后原样送出）。
//   · 已知边界（如实登记，**本批不改**）：值既非空、又非全 `*`、又非合法 URL 的垃圾串（例如 "xx"）
//     仍会被原样提交并覆盖真实 URL——这是**修复前既有**的行为（非脱敏路径同样如此，HTTP 契约层
//     也不校验 URL 形状）；要闭合需给该字段加 URL 形状校验，属**行为收紧**，超出本次范围。
export function normalizePostUrlField(value: unknown, baseline: string): string {
  const t = String(value ?? '').trim()
  // 全 `*` ⇒ 视为「仍是那个脱敏占位符（可能被误删了几个字符）」⇒ 按未改动处理。
  if (/^\*+$/.test(t)) return baseline
  return t
}

// onFinish 的纯逻辑：**整份归一化 + 最小 patch**（抽出来是为了让"组件里那行接线"本身可被 node 单测，
// 而不是让每个测试各自重写一遍接线——那样只能证明"纯函数组合正确"，证明不了"组件把它接上了"）。
// 语义：`normalized` 是"若用户提交、后端应看到的那份完整模型"（保存成功后作为新 baseline）；
//   `patch` 是它与 baseline 的差量（undefined = 无变更，调用方应提示"没有检测到变更"并**不**发请求）。
// ⚠️ 两处 baseline 必须是**同一个**：归一化用的 baseline 与 diffPatch 用的 baseline 若不同源，
//   归一化把哨兵折回的那个值就可能与 diff 的基准不等 ⇒ 哨兵真的会被送出（本函数把两者绑在一起，
//   正是为了消除这层手写接线的脆弱点）。
export function buildSettingsSubmission(
  baseline: ServerFormModel,
  values: ServerFormModel,
): { normalized: ServerFormModel; patch: unknown | undefined } {
  const normalized: ServerFormModel = {
    ...values,
    poster: {
      ...values.poster,
      postUrl: normalizePostUrlField(values.poster.postUrl, baseline.poster.postUrl),
    },
  }
  return { normalized, patch: diffPatch(baseline, normalized) }
}
