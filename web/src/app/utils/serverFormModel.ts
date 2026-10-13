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

// 「这个值是不是脱敏占位符」的**唯一定义点**。
// 判据（trim 后）= **非空且只由 `*` 组成**。故意不写成「等于哨兵 MASKED_SENTINEL」，因为要覆盖两种情形：
//   · 哨兵 "***" 本身（后端 kMaskedSentinel；GET 与 PUT 的响应体都经 currentRedactedJsonPretty ⇒
//     生产路径下该字段初值就是它，见 buildServerFormModelFromDoc 里 postUrl 字段的登记）；
//   · 用户误删一两个字符后的 "*" / "**"（字段初值 "***" 使这类误触成为就近手势）。
// ⚠️ 为什么取「全 `*`」而不是「等于 "***"」：合法 URL 不可能只由 `*` 组成（必先有 scheme 名）⇒
//   「全 `*`」不会误伤任何真 URL；而「等于哨兵」会漏掉误删残留、把它当普通值送出（事故链见下方
//   normalizePostUrlField 与 isValidMaskedPostUrl 的 doc）。
// 为什么要单列成函数：normalizePostUrlField（折回 baseline）复用它，避免"等于哨兵"与"全 `*`"两处判据
//   分散漂移。（isValidMaskedPostUrl 只单独识别**字面**哨兵 "***"、不再复用本函数——见该函数 doc。）
export function isMaskedPlaceholder(value: unknown): boolean {
  return /^\*+$/.test(String(value ?? '').trim())
}

// 提交前的 postUrl 归一化（纯函数，node 环境可直接单测）。语义与不变量：
//   · 一般情形：`trim` 后的值原样返回 ⇒ **空串 = 显式清空**（后端接受空串，见 buildServerFormModelFromDoc
//     里 postUrl 字段的登记：getString 不拒空串，HttpFacesPoster::start 对空 URL 返回 false = 停发）。
//   · ★ 不变量一：值经 isMaskedPlaceholder 判定为占位符时（哨兵 "***" 或**任意只由 `*` 组成的串**），
//     一律**返回 baseline**。于是：① 用户**没动**该字段 ⇒ 返回值仍等于 baseline ⇒ diffPatch 判未变更
//     ⇒ **不提交**；② 哨兵**永不**被当作要提交的值送出——这条保证在**前端**成立。
//     ⚠️「只由 `*` 组成」这一条是**为某个真实事故面**加的（勿当作多余的宽容）：字段初值是 "***"，
//     用户误删一两个字符就会得到 "*" / "**"——它们**不等于**哨兵，若原样送出会：
//     后端 `stripMaskedSentinels` 只剔精确 "***"（不剔 "*"）⇒ schema 的串分支无 pattern ⇒ 落盘覆盖
//     **真实 URL**，而运行期 `HttpFacesPoster` 只判空、于是起线程后 `WinHttpCrackUrl("*")` 失败 ⇒
//     上报静默失效；更糟的是 **GET 恒脱敏**，用户**再也拿不回**被覆盖的 URL。所以这一类的判据必须是
//     「它不可能是合法 URL」而不是「它等于哨兵」——合法 URL 不可能只由 `*` 组成，故不会误伤。
//     （占位符判据不在此重复定义，只调 isMaskedPlaceholder。）
//   · ★ 不变量二：非脱敏场景（baseline 持真值）下行为与修复前**逐字一致**（trim 后原样送出）。
//   · 已知边界（如实登记）：值既非空、又非全 `*`、又非合法 URL 的垃圾串（例如 "xx"）在本函数里仍被
//     原样返回 —— 本函数**只做归一化、不做 URL 形状校验**。形状校验由 isValidMaskedPostUrl 在提交流程
//     入口统一承担（**所有提交路径**，不再限于脱敏分支，见该函数 doc）⇒ 该边界在提交流程内已闭合。
export function normalizePostUrlField(value: unknown, baseline: string): string {
  // ⚠️ null / undefined 按「**未改动**」处理（返回 baseline），**不**当成"清空"：字段值为空是 store 异常或
  //   面板未播种的信号，把它解释成"用户清空了 URL"会**静默毁掉真实 URL**（GET 恒脱敏 ⇒ 拿不回）。
  //   失败安全方向 = 不动它。（对照：`diffPatch` 的"undefined **键** ⇒ 跳过该段、不提交"说的是**段缺失**，
  //   与"字段值为空"不是一回事；本函数只在字段**确实有值**时才可能返回空串 = 用户删空 ⇒ 显式清空。）
  if (value == null) return baseline
  const t = String(value).trim()
  // 占位符（全 `*`）⇒ 视为「仍是那个脱敏占位符（可能被误删了几个字符）」⇒ 按未改动处理。
  if (isMaskedPlaceholder(t)) return baseline
  return t
}

// 提交前 POST URL 的形状判据（true = 允许提交）。**本批收紧，且不再限于脱敏分支**：
//   在任何提交路径上都只接受 http/https URL（空串 = 显式清空、字面哨兵 "***" = 未改动除外）。
// 判据（对入参先 trim）：
//   · 空串（含纯空白）⇒ true —— 显式清空（提交时会成为 ''，后端接受，等价于停发）；
//   · 字面哨兵 "***"（MASKED_SENTINEL）⇒ true —— 未改动，提交时由 normalizePostUrlField 折回 baseline；
//   · 含内嵌空白 ⇒ false；
//   · 其余用 `new URL()` 解析，**仅** `http:` / `https:` 且 hostname 非空 ⇒ true；解析异常或其它 scheme
//     （rtmp / javascript / ftp / data …）⇒ false（拒绝提交）。
// 为什么只认 http/https：后端 `HttpFacesPoster` 走 `WinHttpCrackUrl` 推送，认不出的 scheme 会**静默失效**
//   ——此前 `rtmp://x` / `javascript://h/x` 这类值也能通过（旧判据只要求 `<scheme>://` + 一个非空白字符），
//   落盘后既不能推送、又（因 GET 恒脱敏）拿不回。收紧到 http/https 与后端实际支持的推送面一致。
//   ⚠️ 后端 PUT 另有**独立**校验（不在本文件范围）；本函数只是前端这一层，不得据此认为后端已同样收紧。
// ⚠️ 只由 `*` 组成的**误删残留**（"*" / "**"）**不再**被本函数放行——它们不是合法 URL，会走到 false 分支
//   （拒绝提交）。normalizePostUrlField 的「全 `*` 折回 baseline」不变量仍在，但**校验先于归一化**，故这类
//   值在提交流程里表现为"非法"而非"未变更"。判据仍是"它不可能是合法 URL"，不会误伤真 URL（合法 URL 不可能
//   只由 `*` 组成）。
// ⚠️ 对纯空白输入先 trim 再判空：把该框清成空格 = 显式清空，故放行（与 normalizePostUrlField 把纯空白
//   归一的语义保持一致）。
// ⚠️ null / undefined ⇒ true：字段缺失不是用户的编辑，且提交前已被 normalizePostUrlField 折回 baseline
//   ⇒ 永不外送，无需在此拦。
export function isValidMaskedPostUrl(value: unknown): boolean {
  if (value == null) return true
  const t = String(value).trim()
  if (t === '' || t === MASKED_SENTINEL) return true
  if (/\s/.test(t)) return false
  try {
    const url = new URL(t)
    return (url.protocol === 'http:' || url.protocol === 'https:') && url.hostname.length > 0
  } catch {
    return false
  }
}

// 提交流程里的**整份归一化 + 最小 patch** 一步，由 makeSettingsSubmitHandler 在第 4 步调用。
// （它自身仍是可独立单测的纯函数，供需要"只算 normalized/patch、不跑副作用"的调用方使用。）
// 语义：`normalized` 是"若用户提交、后端应看到的那份完整模型"（保存成功后作为新 baseline）；
//   `patch` 是它与 baseline 的差量（undefined = 无变更，调用方应提示"没有检测到变更"并**不**发请求）。
// ⚠️ 两处 baseline 必须是**同一个**：归一化用的 baseline 与 diffPatch 用的 baseline 若不同源，
//   归一化把哨兵折回的那个值就可能与 diff 的基准不等 ⇒ 哨兵真的会被送出（本函数把两者绑在一起，
//   正是为了消除这层手写接线的脆弱点；组件侧"接线"的行为级覆盖见 makeSettingsSubmitHandler）。
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

// 保存成功后的提示文案；返回 null ⇒ 调用方用通用「保存成功」。
// 为什么需要它（差异化成功提示）：保存成功后输入框**当场**就会再次显示 "***" —— PUT 的响应体本身即脱敏
//   （JsonEndpointHandlers 的 handlePutSettings 走 currentRedactedJsonPretty），而 AppStore.updateServerSettings
//   成功后会用它刷新 serverSettings.data ⇒ ServerSettingsTab 的同步 effect 立刻重填表单与 baseline。
//   于是用户「把 POST URL 删空并保存」后眼看该框又变回 "***"，**无法**从界面判断清空到底生效没有。
//   这条提示是**唯一**不依赖后端契约的确认手段（不要求后端回传明文或额外标志，仅凭前端已知的提交内容判断）。
// 判据（patch 为对象且 patch.poster 为对象时，看 patch.poster.postUrl）：
//   · 恰为空串 '' ⇒ '保存成功：POST URL 已清空（后端已停止推送）'（后端接受空串，等价于停发）；
//   · 非空字符串 ⇒ '保存成功：POST URL 已更新'；
//   · 其余（patch 非对象 / 无 poster / postUrl 缺失或非字符串）⇒ null，交回调用方的通用文案。
// ⚠️ 文案**保留「保存成功」四字**（**不是**把整句换成 POST-URL 专属描述）：一次保存常同时改多个字段
//   （例如 camera.width + poster.postUrl），若整句只提 POST URL，用户会以为**其余编辑没落盘**而重复提交。
//   首版文案正是"整句替换"形态，由对抗性审计抓出 ⇒ 改为"通用成功 + 具体补充"。
export function describeSettingsSaveOutcome(patch: unknown): string | null {
  if (typeof patch !== 'object' || patch === null) return null
  const poster = (patch as { poster?: unknown }).poster
  if (typeof poster !== 'object' || poster === null) return null
  const postUrl = (poster as { postUrl?: unknown }).postUrl
  if (postUrl === '') return '保存成功：POST URL 已清空（后端已停止推送）'
  if (typeof postUrl === 'string') return '保存成功：POST URL 已更新'
  return null
}

// 提交流程的依赖注入版本。**目的：让「提交流程」本身可测**——它的四条分支（未加载 baseline / 无变更 /
//   校验失败 / 成功或异常）都能在 node 环境被行为级测试。
// ⚠️ **不要把它读成"组件接线已覆盖"**（如实登记，勿高估）：被覆盖的是**流程编排**；组件那一层适配
//   （`onFinish` 挂接、`getFieldsValue(true)` 的选取、各 effect 绑定到哪个 antd 调用）**仍然只有一条
//   文本形态守卫**（见测试的组 5），**没有**行为级覆盖——组件在 node 环境不可渲染（vitest 无 jsdom，
//   且导入它会连带把 antd 整棵树拉进测试环境，这正是把模型映射拆到本文件的原因）。
// 结构性收益（比"加测试"更强的那部分）：流程自己取参（`getBaseline()` / `effects.getValues()`），组件
//   **不供参** ⇒ 旧代码里"组件直接 `buildSettingsSubmission(serverBaseline, values)` 这一行零覆盖"的
//   脆弱点被**结构性消除**：组件里已不存在 `values` 这类可被误当 baseline 的变量。
//   ⚠️ 但这不是**类型**强制：`getBaseline` 与 `getValues` 返回同型，误把两者接成同一个 getter 时编译器
//   不报错（该形态仍只能由组件层测试发现，见上）。
// 用法：组件里 `makeSettingsSubmitHandler(() => serverBaseline, {…effects})` 并把返回函数接到 Form 的
//   onFinish（Form 会忽略传入的事件实参；流程自己经 effects.getValues 取**整份 store**）。
// 流程（**逐字**，被测试钉住）：
//   1) 取 baseline；为 null ⇒ notifyWarning('尚未加载 baseline，请先拉取一次后端设置') 并 return（不提交）。
//      ⚠️ 用 warning 而非 error：它是「还不能做这个操作」的前置条件提示，不是失败（旧 onFinish 这里正是
//      message.warning 黄色提示）；此分支**不**进入 loading，故也不调 notifyLoadingStart。文案逐字保持原样。
//   2) 取 values = effects.getValues()。
//   3) POST URL 形状校验（**所有提交路径**，不再限于脱敏分支）：values.poster.postUrl 形状非法
//      （!isValidMaskedPostUrl，只接受 http/https 或空串/字面哨兵）⇒ notifyInvalid('POST URL 需填写以
//      scheme:// 开头的完整地址（如 http:// 或 https://），或留空以清空该字段') 并 return（**不提交**）。
//   4) buildSettingsSubmission(baseline, values) ⇒ { normalized, patch }。
//   5) 无 patch（未变更）⇒ notifyNoChange() 并 return（不发请求）。
//   6) 否则 `const finish = effects.notifyLoadingStart()`（拿到**本次调用专属**的收尾句柄）→
//      await effects.apply(patch) → finish() → notifySuccess(差异化文案) → commitBaseline(normalized)；
//      任一步抛错 ⇒ finish() → notifyError(effects.describeError(e))，catch **不**重新抛出（与既有行为一致）。
//   ⚠️ 收尾在**所有已开始 loading 的路径**都被调用、且**恰好一次**（由 `finished` 一次性守卫保证）：
//      既有组件实现在 catch 路径**漏了** hide() ⇒ message.loading(..., 0)（duration=0 的常驻 toast）
//      永不自动消失 ⇒ **失败时 loading toast 永久卡在屏幕上**（既有缺陷，本流程顺带修掉）。
//      收尾句柄**随调用局部**（不是渲染期共享变量）⇒ 同一次渲染内的两次提交互不干扰。
export type SettingsSubmitEffects = {
  getValues: () => ServerFormModel
  apply: (patch: unknown) => Promise<void>
  notifyNoChange: () => void
  notifyInvalid: (reason: string) => void
  notifyWarning: (text: string) => void
  /** 开始"正在保存"提示，并**返回本次的收尾句柄**；调用方须在所有路径调用它。
   *  刻意**不**拆成 start/end 两个回调：那样收尾句柄只能存在某处共享变量里，而组件把它放在**渲染闭包**中
   *  ⇒ 同一次渲染内的两次提交会互相覆盖（先完成者关掉后者的提示、把先者的常驻 toast 永久留在屏上，
   *  duration=0）——那正是本轮要修的"loading 卡屏"缺陷的**复发形态**（由对抗性审计与独立复核各自命中）。
   *  改成"start 返回句柄"后，收尾状态**随调用局部**，该形态在结构上不可能。 */
  notifyLoadingStart: () => () => void
  notifySuccess: (text: string) => void
  notifyError: (text: string) => void
  commitBaseline: (normalized: ServerFormModel) => void
  describeError: (e: unknown) => string
}

export function makeSettingsSubmitHandler(
  getBaseline: () => ServerFormModel | null,
  effects: SettingsSubmitEffects,
): () => Promise<void> {
  return async () => {
    const baseline = getBaseline()
    if (!baseline) {
      // 用 warning 而非 error：这是「还不能做这个操作」的**前置条件提示**，不是失败——旧 onFinish 这里正是
      // message.warning（黄色），文案逐字保持原样。此分支**不**进入 loading（不得调 notifyLoadingStart）。
      effects.notifyWarning('尚未加载 baseline，请先拉取一次后端设置')
      return
    }
    const values = effects.getValues()
    // 对所有提交路径收紧（不再只看脱敏分支）：见 isValidMaskedPostUrl 的 doc。
    if (!isValidMaskedPostUrl(values.poster.postUrl)) {
      effects.notifyInvalid(
        'POST URL 需填写以 scheme:// 开头的完整地址（如 http:// 或 https://），或留空以清空该字段',
      )
      return
    }
    const { normalized, patch } = buildSettingsSubmission(baseline, values)
    if (!patch) {
      effects.notifyNoChange()
      return
    }
    // 本次调用专属的收尾句柄 + 一次性守卫：保证「失败也收尾」且「恰好收尾一次」（见上方 doc）。
    const finishLoading = effects.notifyLoadingStart()
    let finished = false
    const finish = () => {
      if (finished) return
      finished = true
      finishLoading()
    }
    try {
      await effects.apply(patch)
      finish()
      effects.notifySuccess(describeSettingsSaveOutcome(patch) ?? '保存成功')
      // ⚠️ 真实组件里这一步的**效果会被随后的刷新 effect 覆盖**：AppStore.updateServerSettings 成功时
      //   用 PUT 的**脱敏响应**刷新 serverSettings.data（PUT 响应恒为 "***"）⇒ 组件的同步 effect 立刻
      //   用新文档重建 baseline（postUrl 又是 "***"）。故"normalized 成为新 baseline"在生产中近乎空转
      //   （无害：字段也显示 "***"，二次保存仍判未变更）。**勿**把这里读成"生产上 baseline 会变成清空后的值"。
      effects.commitBaseline(normalized)
    } catch (e) {
      finish()
      effects.notifyError(effects.describeError(e))
    }
  }
}
