// 服务端 settings ↔ 表单模型 的结构契约测试（S-5：让"只覆盖部分段"不再复发）。
//
// 背景：本表单曾只建模 ServerSettingsDoc 的 http/log/poster/ui/acceleration，camera/recognition/
//   inference/dnn/model/display 从不进入提交的 patch —— 用户改不到它们。
// 本文件钉住：
//   1) 完整性：buildServerFormModelFromDoc 必须**逐字段**覆盖文档的每一段（不只是段名，连段内
//      字段也逐个对拍）——漏一个字段即红；
//   2) 机制：diffPatch(baseline, 只改了一处的模型) 必须只产出那一处（含嵌套路径），
//      即"改了什么就只提交什么"，未被编辑的段不会被整体回写；
//   3) 脱敏：poster.postUrl 的 "***" 哨兵**原样留在模型里**（使「没动」与「清空」在 diffPatch 眼里
//      不同 ⇒ 清空可达）+ postUrlMasked=true；normalizePostUrlField 的语义与**不变量**（哨兵、
//      以及"全 `*`"的误删残留，都永不作为待提交的值送出）；buildSettingsSubmission 与
//      makeSettingsSubmitHandler 覆盖**提交流程本体**的行为。
//      ⚠️ 如实口径：**组件适配层**（onFinish 挂接 / getFieldsValue(true) 的选取 / effect 绑定）
//      **仍无行为级覆盖**，只有组 5 那条文本形态守卫——不要读成"组件接线已覆盖"。
//      后端另有 stripMaskedSentinels 兜底，但前端**不**把"哨兵不回写"托付给那一层
//      （该层对**非 UI 客户端**仍是唯一防线）。
//   4) 未展开面板：整份 store 被播种后，未展开面板的字段与 baseline 同值 ⇒ diffPatch 判未变更。
//
// 环境：node（无 jsdom）。本文件只 import 纯模块（serverFormModel / diffPatch），
//   不经 antd —— 这正是把模型映射从 ServerSettingsTab.tsx 拆出来的原因之一。
import { describe, it, expect, vi } from 'vitest'
// 组 5 的接线形态守卫要**读生产源码**，故需 Node 的 fs/path/url。
// ⚠️ `tsconfig.app.json` 的 `types` 仅 `["vite/client"]` ⇒ `tsc -b`（`pnpm build` 的第一步）**不加载 node 类型**，
//   这几个模块在 build 期会报 TS2307；而 vitest 运行时它们是可用的。本仓对同一问题的既定处置见
//   `src/__tests__/App.routes.test.tsx` 对 `node:module` 的写法（`@ts-expect-error` + 说明），此处沿用。
//   每个 import 各带一条指令：若将来 tsconfig 补上 node 类型，这些指令会因"未使用"而**自己报错**
//   ⇒ 那时应删掉它们（自证式钉住，避免抑制长期残留而无人清理）。
// @ts-expect-error node:fs 在 vitest 运行时可用；build 期类型检查不加载 node 类型
import { readFileSync } from 'node:fs'
// @ts-expect-error 同上（node:path 仅测试期需要）
import { dirname, resolve } from 'node:path'
// @ts-expect-error 同上（node:url 仅测试期需要）
import { fileURLToPath } from 'node:url'

import type { ServerSettingsDoc } from '../../api/types'
import { diffPatch } from '../diffPatch'
import { buildServerFormModelFromDoc, buildSettingsSubmission, isSettingsDocComplete, normalizePostUrlField, SETTINGS_SECTIONS } from '../serverFormModel'
// 本批新增导出（脱敏 POST URL 行为收紧）；与上一行同源，分两行是为了不打乱既有导入的排版。
import {
  describeSettingsSaveOutcome,
  isMaskedPlaceholder,
  isValidMaskedPostUrl,
  makeSettingsSubmitHandler,
  type ServerFormModel,
} from '../serverFormModel'

// 完整文档夹具：字段集必须与 web/src/app/api/types.ts 的 ServerSettingsDoc 一致。
// 为什么手写而不是从别处派生：本文件的用途就是"以手写的期望值独立复述契约"，派生会自证。
function fullDoc(): ServerSettingsDoc {
  return {
    schemaVersion: 1,
    camera: { preferredDeviceId: 'cam-0', width: 1280, height: 720, fps: 30 },
    recognition: {
      cascadePath: 'storage/models/haarcascade.xml',
      databasePath: 'storage/face_db',
      minFaceSizePx: 60,
      identifyThreshold: 55,
      identifyThresholdArcFace: 0.45,
      enrollSamples: 12,
      arcFaceModelPath: 'storage/models/arcface.onnx',
      arcFaceInputName: '',
      arcFaceOutputName: '',
    },
    inference: { throttleMode: 'auto', intervalMs: 150 },
    dnn: {
      enable: true,
      modelPath: 'storage/models/detector.pb',
      configPath: 'storage/models/detector.pbtxt',
      inputWidth: 300,
      inputHeight: 300,
      scale: 1,
      meanB: 104,
      meanG: 177,
      meanR: 123,
      swapRB: false,
      confThreshold: 0.5,
      backend: 0,
      target: 0,
    },
    model: {
      faceProfileDir: 'storage/profiles/synthetic',
      detection: 'yolo_face',
      recognition: 'arcface',
      backend: 'opencv_dnn',
      detectorBackend: 'opencv_dnn',
      recognitionBackend: 'opencv_dnn',
      autoFallback: true,
      int8Enabled: false,
    },
    http: { enable: true, port: 8080 },
    poster: {
      enable: false,
      postUrl: 'http://localhost:8080/api/faces',
      throttleMs: 100,
      backoffMinMs: 200,
      backoffMaxMs: 5000,
    },
    log: { logDir: 'storage/win_logs', maxFileBytes: 10485760, maxRollFiles: 5 },
    ui: { windowWidth: 1280, windowHeight: 800, previewScaleMode: 0 },
    display: {
      outputIndex: 0,
      width: 0,
      height: 0,
      refreshNumerator: 0,
      refreshDenominator: 1,
      vsync: true,
      swapchainBuffers: 2,
      fullscreen: false,
      allowSystemModeSwitch: false,
      enableSRGB: true,
      gamma: 2.2,
      colorTempK: 6500,
      aaSamples: 1,
      anisoLevel: 1,
    },
    acceleration: { enableOpenCL: false, enableLibyuv: false, enableMpp: false, enableQualcomm: false },
  }
}

describe('buildServerFormModelFromDoc 完整性（S-5）', () => {
  it('profile 目录完整进入提交，清空显式回到旧入口', () => {
    const { model: baseline } = buildServerFormModelFromDoc(fullDoc())
    expect(baseline.model.faceProfileDir).toBe('storage/profiles/synthetic')
    const changed = { ...baseline, model: { ...baseline.model, faceProfileDir: 'storage/profiles/next' } }
    expect(buildSettingsSubmission(baseline, changed).patch).toEqual({ model: { faceProfileDir: 'storage/profiles/next' } })
    const cleared = { ...baseline, model: { ...baseline.model, faceProfileDir: '' } }
    expect(buildSettingsSubmission(baseline, cleared).patch).toEqual({ model: { faceProfileDir: '' } })
  })

  it('旧响应缺失 profile 字段时保持空入口，其他编辑不回写该字段', () => {
    const doc = fullDoc()
    delete (doc.model as Partial<ServerSettingsDoc['model']>).faceProfileDir
    const { model: baseline } = buildServerFormModelFromDoc(doc)
    expect(baseline.model.faceProfileDir).toBe('')
    const changed = { ...baseline, camera: { ...baseline.camera, width: 800 } }
    expect(buildSettingsSubmission(baseline, changed).patch).toEqual({ camera: { width: 800 } })
  })

  it('段集合与 ServerSettingsDoc 的段集合逐一相等（判别点：漏一段即红）', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    expect(Object.keys(model).sort()).toEqual([...SETTINGS_SECTIONS].sort())
    // SETTINGS_SECTIONS 自身的长度也钉住：它若被误改短，上一句会同向一起变，不自证。
    expect(SETTINGS_SECTIONS).toHaveLength(11)
  })

  it('每一段的**字段集合**与文档逐一相等（判别点：段内漏一个字段即红）', () => {
    const doc = fullDoc()
    const { model } = buildServerFormModelFromDoc(doc)
    for (const section of SETTINGS_SECTIONS) {
      expect(Object.keys(model[section]).sort(), `段 ${section} 的字段集合`).toEqual(
        Object.keys(doc[section]).sort(),
      )
    }
  })

  it('路径/阈值类字段按文档原值无损映射（抽查识别段）', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    expect(model.recognition.databasePath).toBe('storage/face_db')
    expect(model.recognition.identifyThreshold).toBe(55)
    expect(model.recognition.identifyThresholdArcFace).toBe(0.45)
  })
})

describe('poster.postUrl 脱敏哨兵与清空可达性（本次修复的落点）', () => {
  it('哨兵 "***" **原样留在模型里** + postUrlMasked=true（判别点：若映射成空串，则"用户没动"与"用户清空"同值 ⇒ 清空恒判未变更、不可达）', () => {
    const doc = fullDoc()
    doc.poster.postUrl = '***'
    const { model, postUrlMasked } = buildServerFormModelFromDoc(doc)
    expect(postUrlMasked).toBe(true)
    expect(model.poster.postUrl).toBe('***')
  })

  it('非哨兵字符串原样保留 + postUrlMasked=false', () => {
    const { model, postUrlMasked } = buildServerFormModelFromDoc(fullDoc())
    expect(postUrlMasked).toBe(false)
    expect(model.poster.postUrl).toBe('http://localhost:8080/api/faces')
  })
})

describe('normalizePostUrlField：语义与**不变量**', () => {
  // ★ 不变量：哨兵（及"全 `*`"的误删残留）**永不**作为"要提交的值"被送出；保证在**前端**成立。
  //   判别点（**两处独立**，勿混为一谈）：
  //   · 把函数体改成 `return t` ⇒ 本组第 1 条（`('***','http://real')` 期望真值）与第 2 条（全 `*`）变红；
  //   · ★ 但下面「用户没动 ⇒ 不提交」那条**仍绿**——baseline 本身就是哨兵、`'***'` 与之相等，
  //     diffPatch 照样判未变更。首版注释曾声称"两条同时红"，**那是夸大**，已由对抗性审计抓出并订正。
  it('哨兵 → 返回 baseline（未改动 ⇒ 与 baseline 等值 ⇒ 不提交；哨兵绝不外送）', () => {
    expect(normalizePostUrlField('***', '***')).toBe('***')
    // 即便 baseline 已持有真值（例如刚成功保存过一次），手打 "***" 也按"未改动"处理，不外送哨兵。
    expect(normalizePostUrlField('***', 'http://real')).toBe('http://real')
  })

  it('★ 部分删除哨兵（"*" / "**"）→ 返回 baseline（本批**新引入事故面**的兜底）', () => {
    // 为什么必须兜：字段初值是 "***"，用户误删一两个字符就得 "*" / "**"；它们**不等于**哨兵，
    //   而后端 stripMaskedSentinels 只剔精确 "***"、schema 的串分支无 pattern ⇒ 会被落盘、
    //   覆盖**真实 URL**；更糟的是 GET 恒脱敏 ⇒ 用户**再也拿不回**。故判据取"不可能是合法 URL"
    //   （全 `*`）而不是"等于哨兵"。
    expect(normalizePostUrlField('*', '***')).toBe('***')
    expect(normalizePostUrlField('**', '***')).toBe('***')
    expect(normalizePostUrlField('****', '***')).toBe('***')
    expect(normalizePostUrlField(' * ', '***')).toBe('***') // trim 后仍全 `*`
    // 非全 `*` 的串不在兜底之列（属**修复前既有**行为，见该函数 doc 的"已知边界"）。
    expect(normalizePostUrlField('*x', '***')).toBe('*x')
  })

  it('空串 / 纯空白 → 空串（**显式清空**；脱敏字段初值是哨兵，故空串 ≠ baseline ⇒ 判为变更）', () => {
    expect(normalizePostUrlField('', '***')).toBe('')
    expect(normalizePostUrlField('   ', '***')).toBe('')
  })

  it('普通 URL → trim 后原样返回', () => {
    expect(normalizePostUrlField('  http://a/b  ', '***')).toBe('http://a/b')
  })

  it('★ undefined / null → **返回 baseline**（按「未改动」处理，**不**当作清空）', () => {
    // 判别点：把实现改回 `String(value ?? '').trim()` 那一形态 ⇒ 这两条变红（得到 '' ⇒ 语义变成"清空"）。
    // 为什么必须如此：字段值为空是 store 异常 / 面板未播种的信号，把它当成"用户清空了 URL"会**静默毁掉
    //   真实 URL**（GET 恒脱敏 ⇒ 拿不回）。失败安全方向 = 不动它。
    expect(normalizePostUrlField(undefined, '***')).toBe('***')
    expect(normalizePostUrlField(null, '***')).toBe('***')
    expect(normalizePostUrlField(undefined, 'http://real')).toBe('http://real')
    expect(normalizePostUrlField(null, 'http://real')).toBe('http://real')
  })
})

describe('buildSettingsSubmission：归一化 + diff 的组合（提交流程内的一步）', () => {
  // ⚠️ 本组的历史与**如实口径**（勿读成"组件接线已覆盖"）：本组原称"组件 onFinish 的真实接线"——
  //   **该措辞在流程被搬进 makeSettingsSubmitHandler 之后已不成立**（组件不再直接调 buildSettingsSubmission，
  //   改由工厂内部调）。现在本组覆盖的是「归一化 + diffPatch 的组合」这一步本身。
  //   而**组件适配层**（onFinish 挂接 / getFieldsValue(true) 的选取 / effect 绑定）**仍无行为级覆盖**
  //   ——组件在 node 环境不可渲染（vitest 无 jsdom），只有下方组 5 的文本形态守卫。
  const maskedModel = () => {
    const doc = fullDoc()
    doc.poster.postUrl = '***'
    return buildServerFormModelFromDoc(doc).model
  }
  const withPostUrl = (baseline: ReturnType<typeof maskedModel>, typed: string) => ({
    ...baseline,
    poster: { ...baseline.poster, postUrl: typed },
  })

  it('用户没动（值仍是哨兵）→ patch 为 undefined（不发哨兵、不提交）', () => {
    const baseline = maskedModel()
    expect(buildSettingsSubmission(baseline, withPostUrl(baseline, '***')).patch).toBeUndefined()
  })

  it('★ 用户清空 → patch 恰为 { poster: { postUrl: "" } }（修复前这里是 undefined，清空被吞掉）', () => {
    const baseline = maskedModel()
    expect(buildSettingsSubmission(baseline, withPostUrl(baseline, '')).patch).toEqual({
      poster: { postUrl: '' },
    })
  })

  it('用户改写 → patch 恰为 { poster: { postUrl: <新 URL> } }，且**不含**哨兵', () => {
    const baseline = maskedModel()
    const url = 'http://10.0.0.5:9000/hook'
    expect(buildSettingsSubmission(baseline, withPostUrl(baseline, url)).patch).toEqual({
      poster: { postUrl: url },
    })
  })

  it('★ 误删哨兵字符 → 判未变更，绝不把 "*" / "**" 提交出去（审计抓出的新事故面）', () => {
    const baseline = maskedModel()
    expect(buildSettingsSubmission(baseline, withPostUrl(baseline, '*')).patch).toBeUndefined()
    expect(buildSettingsSubmission(baseline, withPostUrl(baseline, '**')).patch).toBeUndefined()
  })

  it('normalized 是"后端应看到的完整模型"：除 postUrl 归一化外逐字段与入参一致', () => {
    const baseline = maskedModel()
    const values = withPostUrl(baseline, '  http://x/  ')
    const { normalized } = buildSettingsSubmission(baseline, values)
    expect(normalized.poster.postUrl).toBe('http://x/') // trim 生效
    expect(normalized).toEqual({ ...values, poster: { ...values.poster, postUrl: 'http://x/' } })
  })
})

describe('提交粒度：改了什么就只提交什么', () => {
  it('未编辑 → diffPatch 返回 undefined（不会提交任何段）', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    // ⚠️ 传**深拷贝**而非同一个引用：diffPatch 首行是 `Object.is(oldValue, newValue)` 短路，传同一引用
    //   会立刻返回 undefined ⇒ 断言与模型内容无关、**不可能红**（旧写法正是如此，已订正）。
    //   深拷贝后要逐字段走完递归比较才返回 undefined（判别点：diffPatch 若对"值相等的对象"返回内容，本行即红）。
    //   structuredClone 是 Node ≥ 17 的全局（CI 的 web job 用 Node 22）；模型是纯 JSON 数据，克隆无损。
    expect(diffPatch(model, structuredClone(model))).toBeUndefined()
  })

  it('只改识别段一个字段 → patch 恰好只含该字段（判别点：patch 里出现额外段即红）', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    const edited = {
      ...model,
      recognition: { ...model.recognition, identifyThreshold: 40 },
    }
    expect(diffPatch(model, edited)).toEqual({ recognition: { identifyThreshold: 40 } })
  })

  it('只改 display 段一个字段 → patch 恰好只含该字段（判别点是"离散字段"而非"整段替换"）', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    const edited = {
      ...model,
      display: { ...model.display, vsync: false },
    }
    expect(diffPatch(model, edited)).toEqual({ display: { vsync: false } })
  })
})

// 未展开面板不丢字段：钉住 Collapse 惰性挂载与"提交正确性"的关系。
//
// 真实链路（ServerSettingsTab）：store 由同步 effect 的 setFieldsValue(整份 model) 播种 ⇒ 未展开的面板
//   虽没有挂载控件，其字段值仍在 store 里（= 文档原值）⇒ diffPatch(baseline, store) 对它们判"未变更"、
//   天然不提交。出问题的是旧代码取的 onFinish **回调参数**（只含已挂载的字段）。
// ⚠️ 覆盖边界（如实说明）：本组是**纯逻辑**护栏——组件本身（ServerSettingsTab 的
//   getFieldsValue(true) 与哨兵映射）在 node 环境**不可导入**（会连带把 antd 整棵树拉进来，这正是模型
//   映射被拆到 serverFormModel.ts 的原因）⇒「组件若改成整份文档提交」这类回归**不会**被本组挡住，
//   需要 DOM/E2E 层覆盖。本组钉住的，是组件那条链路所依赖的两条纯逻辑前提。
describe('未展开面板不丢字段（提交粒度 · 模拟 Collapse 惰性挂载）', () => {
  it('整份 store 被播种（值与 baseline 逐值相等但是**另一个对象**）→ 判未变更', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    // 深拷贝（而非同一引用）才能真走到递归比较，见上方"未编辑"用例的判别点说明。
    expect(diffPatch(model, structuredClone(model))).toBeUndefined()
  })

  it('提交模型缺失整段（该面板从未展开）⇒ 只"不提交"该段：不清空、也不被兜底默认值覆盖', () => {
    const { model } = buildServerFormModelFromDoc(fullDoc())
    // 模拟"只有展开过的面板进了提交模型"的形态（组件的 defaultActiveKey 是 http/ui）。
    const onlyExpandedPanels = { http: structuredClone(model.http), ui: structuredClone(model.ui) }
    // diffPatch 对 newValue 为 undefined 的键**直接跳过**（`if (d !== undefined)`，见 diffPatch.ts）
    //   ⇒ 缺失段既不会被清空、也不会被写入兜底默认值——只有 patch 里**显式出现**的键才会被提交。
    //   （判别点：若 diffPatch 把 undefined 当成"删除/清空"发出，本行即红；这正是 onFinish 注释所依赖的前提。）
    expect(diffPatch(model, onlyExpandedPanels)).toBeUndefined()
  })
})

describe('isSettingsDocComplete：缺段文档必须被挡住', () => {
  it('完整文档 → true', () => {
    expect(isSettingsDocComplete(fullDoc())).toBe(true)
  })

  // 本判据的用途是 ServerSettingsTab 的 formReady 闸门（缺段时整份表单禁用）。判别点与"兜底默认值"
  //   **无关**（旧措辞称"否则兜底默认值会覆盖真实配置"，不成立：baseline 与表单模型同源 ⇒ diffPatch
  //   恒判未变更）：stub 里没有任何真实配置，本判据把它与正常文档区分开，从而禁用表单与保存按钮。
  it('错误 stub（只有 schemaVersion/_error）→ false（判别点：formReady 据此禁用整份表单）', () => {
    // 双断言：ServerSettingsDoc 的段字段是必填的，故缺段字面量无法直接断言过去
    const stub = { schemaVersion: 1, _error: 'key_error' } as unknown as ServerSettingsDoc
    expect(isSettingsDocComplete(stub)).toBe(false)
  })

  it('undefined → false', () => {
    expect(isSettingsDocComplete(undefined)).toBe(false)
  })
})

// ===========================================================================
// POST URL 的**行为收紧**（本批扩展到所有提交路径）
//   （字面哨兵识别 / URL 形状判据 / 保存结果文案 / 提交处理器工厂的行为级覆盖 / 接线形态守卫）
//
// 事故面（本批判据要挡的东西）：poster.postUrl 的**字段初值是脱敏占位符 "***"**（见
//   buildServerFormModelFromDoc）。用户手误（把 "***" 敲成 "*x" / "xx"）若被当作合法值提交：
//   后端 stripMaskedSentinels 只剔**精确** "***"（不剔 "*x"），而 schema 的串分支**无 pattern**
//   ⇒ 落盘生效、**静默覆盖真实 URL**；更糟的是 GET 恒脱敏 ⇒ 用户**再也拿不回**被覆盖的 URL。
//   ⇒ 非空且非字面哨兵 "***" 的值**必须**是 http/https URL，否则拒绝提交。
// ⚠️ 本批把收紧从「仅脱敏分支」扩到**所有提交路径**：任何 baseline 下都只接受 http/https
//   （空串 = 清空、字面哨兵 "***" = 不改，二者除外）；只由 `*` 组成的误删残留（"*" / "**"）也判非法。
// ===========================================================================

describe('isMaskedPlaceholder：全 `*` 占位符识别（trim 后判据）', () => {
  it('全 `*`（含首尾空白）⇒ true', () => {
    expect(isMaskedPlaceholder('***'), '"***" 应为占位符').toBe(true)
    expect(isMaskedPlaceholder('*'), '"*" 应为占位符').toBe(true)
    expect(isMaskedPlaceholder('**'), '"**" 应为占位符').toBe(true)
    expect(isMaskedPlaceholder('****'), '"****" 应为占位符').toBe(true)
    expect(isMaskedPlaceholder(' *** '), '" *** " trim 后全 `*`，应为占位符').toBe(true)
  })

  it('空串 / 非全 `*` / 真 URL / nullish ⇒ false', () => {
    expect(isMaskedPlaceholder(''), '空串应为 false').toBe(false)
    expect(isMaskedPlaceholder('**x'), '"**x" 含非 `*` 字符，应为 false').toBe(false)
    expect(isMaskedPlaceholder('x'), '"x" 应为 false').toBe(false)
    expect(isMaskedPlaceholder('http://a'), '真 URL 应为 false').toBe(false)
    expect(isMaskedPlaceholder(undefined), 'undefined 应为 false').toBe(false)
    expect(isMaskedPlaceholder(null), 'null 应为 false').toBe(false)
  })
})

describe('isValidMaskedPostUrl：空串/字面哨兵 ⇒ 合法；否则须为 http(s) URL', () => {
  it('空串 / 纯空白 / 字面哨兵 "***" ⇒ true（分别代表"清空"与"未改动"，都不该报校验错）', () => {
    expect(isValidMaskedPostUrl(''), '空串 ⇒ 合法（= 显式清空）').toBe(true)
    expect(isValidMaskedPostUrl('   '), '纯空白 ⇒ 合法').toBe(true)
    expect(isValidMaskedPostUrl('***'), '字面哨兵 ⇒ 合法（未改动）').toBe(true)
  })

  it('http/https URL ⇒ true（大小写 scheme 经 new URL 归一化后判定；首尾空白经 trim）', () => {
    expect(isValidMaskedPostUrl('http://a/b')).toBe(true)
    expect(isValidMaskedPostUrl('https://a')).toBe(true)
    // new URL 把 scheme 归一化为小写 ⇒ 大写/混合写法同样合法。
    expect(isValidMaskedPostUrl('HTTP://a/b'), '大写 scheme 归一化后合法').toBe(true)
    expect(isValidMaskedPostUrl('HtTpS://a'), '混合大小写 scheme 归一化后合法').toBe(true)
    expect(isValidMaskedPostUrl('  http://a/b  '), 'trim 后合法').toBe(true)
  })

  it('★ 仅接受 http/https：其它 scheme ⇒ false（本批收紧的落点）', () => {
    // 旧判据只要求 `<scheme>://`（不限定 scheme），rtmp/javascript/ftp/data 都能通过；现收紧为仅 http/https。
    expect(isValidMaskedPostUrl('rtmp://h/x'), 'rtmp 不再放行').toBe(false)
    expect(isValidMaskedPostUrl('javascript://h/x')).toBe(false)
    expect(isValidMaskedPostUrl('ftp://x')).toBe(false)
    expect(isValidMaskedPostUrl('data://x')).toBe(false)
  })

  it('★ 非 URL 垃圾串 / 缺 host / 内嵌空白 ⇒ false', () => {
    expect(isValidMaskedPostUrl('xx'), '"xx" 非 URL').toBe(false)
    expect(isValidMaskedPostUrl('*x'), '"*x" 是误删占位符的残串、非 URL').toBe(false)
    // 误删到只剩 `*`（"*" / "**"）：不是**字面哨兵** "***"，也不是合法 URL ⇒ 拒（本批收紧，见上方组说明）。
    expect(isValidMaskedPostUrl('*'), '"*" 非字面哨兵、非 URL ⇒ 拒').toBe(false)
    expect(isValidMaskedPostUrl('**'), '"**" 非字面哨兵、非 URL ⇒ 拒').toBe(false)
    expect(isValidMaskedPostUrl('example.com/api'), '缺 scheme').toBe(false)
    expect(isValidMaskedPostUrl('://a'), '缺 scheme 名').toBe(false)
    // ⚠️ hostname 必须非空：`http://`（空 host）经 new URL 解析失败 ⇒ 拒。
    expect(isValidMaskedPostUrl('http://'), '"http://" 空 host ⇒ 非法').toBe(false)
    expect(isValidMaskedPostUrl('http://   '), '"http://   " trim 后仍空 host ⇒ 非法').toBe(false)
    // ⚠️ 内嵌空白（trim 只去首尾）⇒ 拒。
    expect(isValidMaskedPostUrl('http://a b/c'), '内嵌空白 ⇒ 拒').toBe(false)
  })

  it('undefined / null ⇒ true（字段缺失不是编辑；实现按 value == null 直接放行）', () => {
    // ⚠️ 期望值来自实现语义：null/undefined 直接 true（字段缺失时提交前已被 normalizePostUrlField 折回 baseline，永不外送）。
    expect(isValidMaskedPostUrl(undefined)).toBe(true)
    expect(isValidMaskedPostUrl(null)).toBe(true)
  })
})

describe('describeSettingsSaveOutcome：保存结果文案', () => {
  it('清空 postUrl ⇒ 明示后端停止推送', () => {
    expect(describeSettingsSaveOutcome({ poster: { postUrl: '' } })).toBe(
      '保存成功：POST URL 已清空（后端已停止推送）',
    )
  })

  it('改写 postUrl ⇒ 已更新', () => {
    expect(describeSettingsSaveOutcome({ poster: { postUrl: 'http://x' } })).toBe('保存成功：POST URL 已更新')
  })

  it('与 postUrl 无关 / 形态非法 ⇒ null，且不抛', () => {
    const cases: [string, unknown][] = [
      ['仅改 poster.enable', { poster: { enable: true } }],
      ['空 patch', {}],
      ['undefined', undefined],
      ['字符串 patch', 'str'],
      ['poster 为 null', { poster: null }],
    ]
    for (const [label, value] of cases) {
      expect(() => describeSettingsSaveOutcome(value), `${label} 不应抛`).not.toThrow()
      expect(describeSettingsSaveOutcome(value), `${label} 应为 null`).toBeNull()
    }
  })
})

// ---- 提交处理器工厂的公共夹具 ----------------------------------------------

// maskedFormModel = 字段初值为脱敏占位符 "***" 的模型（生产脱敏路径）；realFormModel = 普通真值模型。
function maskedFormModel(): ServerFormModel {
  const doc = fullDoc()
  doc.poster.postUrl = '***'
  return buildServerFormModelFromDoc(doc).model
}
function realFormModel(): ServerFormModel {
  return buildServerFormModelFromDoc(fullDoc()).model
}
// 只改 poster.postUrl，其余字段保持不变（避免误触其他段的 diff）。
function setPostUrl(model: ServerFormModel, postUrl: string): ServerFormModel {
  return { ...model, poster: { ...model.poster, postUrl } }
}

// effects 替身：全部以 vi.fn 记录"调了几次 / 收到什么"。apply 默认成功，可按用例覆盖为抛异常。
// ⚠️ 说明：本组只断言"工厂调了哪个副作用、收到什么"，不重实现工厂内部逻辑——这正是把流程抽成
//   可注入副作用的工厂（而非直接调 antd message）的目的：流程本身在 node 环境可执行、可断言。
function makeSubmitEffects(opts: {
  values: ServerFormModel
  apply?: (patch: unknown) => Promise<void>
  // 可选：覆盖某个通知回调（用于"成功路径的后续回调抛错"这类边角）
  notifySuccess?: () => void
}) {
  // 收尾句柄替身：接口约定是「notifyLoadingStart **返回**本次的收尾句柄」（不是 start/end 两个回调）——
  //   那样收尾句柄必须寄存在某处共享变量里，同一次渲染内的两次提交会互相覆盖（本轮修掉的"卡屏"复发形态）。
  const finish = vi.fn()
  return {
    getValues: vi.fn(() => opts.values),
    apply: vi.fn(opts.apply ?? (async () => {})),
    notifyNoChange: vi.fn(),
    notifyInvalid: vi.fn(),
    // 前置条件提示（黄色 warning）——与"失败"（notifyError）区分：baseline 未加载属提示而非错误。
    notifyWarning: vi.fn(),
    notifyLoadingStart: vi.fn(() => finish),
    notifySuccess: vi.fn(opts.notifySuccess),
    notifyError: vi.fn(),
    commitBaseline: vi.fn(),
    describeError: vi.fn((e: unknown) => `描述:${String(e)}`),
    /** 本次调用拿到的收尾句柄（spy），用于断言"收尾恰好一次"。 */
    finish,
  }
}

describe('makeSettingsSubmitHandler：提交流程的行为级覆盖（本批核心闭合项）', () => {
  it('1) baseline 为 null ⇒ notifyWarning（**逐字**文案）；不进入提交（apply / notifyLoadingStart 均未调用）', async () => {
    // 这条分支是**前置条件提示**（warning），不是失败（error）——原实现用的就是 message.warning。
    const effects = makeSubmitEffects({ values: setPostUrl(maskedFormModel(), 'http://a/b') })
    const handler = makeSettingsSubmitHandler(() => null, effects)
    await handler()
    expect(effects.notifyWarning, 'baseline 缺失应给逐字提示').toHaveBeenCalledWith(
      '尚未加载 baseline，请先拉取一次后端设置',
    )
    expect(effects.notifyError, '这是提示而非失败，不得用 error 严重度').not.toHaveBeenCalled()
    expect(effects.notifyInvalid, '不得报字段校验错').not.toHaveBeenCalled()
    expect(effects.apply, 'baseline 缺失时不得提交').not.toHaveBeenCalled()
    expect(effects.notifyLoadingStart, 'baseline 缺失时不应起 loading').not.toHaveBeenCalled()
  })

  it('2) 脱敏 baseline + 值仍是哨兵 "***" ⇒ 判无变更、不提交', async () => {
    const baseline = maskedFormModel()
    const effects = makeSubmitEffects({ values: setPostUrl(baseline, '***') })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.notifyNoChange, '值未改动应提示无变更').toHaveBeenCalledTimes(1)
    expect(effects.apply, '未改动不得提交').not.toHaveBeenCalled()
    expect(effects.notifyInvalid, '哨兵是合法占位符，不得报校验错').not.toHaveBeenCalled()
  })

  it('3) ★ 脱敏 baseline + 值清空（""）⇒ 提交 { poster: { postUrl: "" } }；成功文案 = 清空；commitBaseline 收到归一化整份模型', async () => {
    const baseline = maskedFormModel()
    const effects = makeSubmitEffects({ values: setPostUrl(baseline, '') })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.apply, '清空应恰好发一次提交').toHaveBeenCalledTimes(1)
    expect(effects.apply.mock.calls[0][0], 'patch 恰为清空 postUrl').toEqual({ poster: { postUrl: '' } })
    expect(effects.notifySuccess, '成功文案应明示已清空（且保留「保存成功」以免多字段保存时误判其余编辑未落盘）').toHaveBeenCalledWith('保存成功：POST URL 已清空（后端已停止推送）')
    expect(effects.commitBaseline, '成功后应更新 baseline').toHaveBeenCalledTimes(1)
    const committed = effects.commitBaseline.mock.calls[0][0] as ServerFormModel
    expect(committed.poster.postUrl, '新 baseline 的 postUrl 归一后为 ""').toBe('')
    // 判别点：若工厂误把 patch 而非"整份归一化模型"交给 commitBaseline，committed.camera 会是 undefined ⇒ 本行红。
    expect(committed.camera, 'commitBaseline 收到的是整份模型（而非 patch）').toBeDefined()
    expect(effects.notifyNoChange, '清空是变更，不应提示无变更').not.toHaveBeenCalled()
  })

  it('4) 脱敏 baseline + 值改写为合法 URL ⇒ 提交该 URL；成功文案 = 已更新', async () => {
    const baseline = maskedFormModel()
    const url = 'http://10.0.0.5:9000/hook'
    const effects = makeSubmitEffects({ values: setPostUrl(baseline, url) })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.apply).toHaveBeenCalledTimes(1)
    expect(effects.apply.mock.calls[0][0], 'patch 恰为新 URL').toEqual({ poster: { postUrl: url } })
    expect(effects.notifySuccess).toHaveBeenCalledWith('保存成功：POST URL 已更新')
    expect(effects.notifyInvalid, '合法 URL 不应被拦').not.toHaveBeenCalled()
  })

  it('5) ★ 脱敏 baseline + 值 "xx"（非 URL 垃圾串）⇒ notifyInvalid，且不提交（行为收紧的闭合点）', async () => {
    const baseline = maskedFormModel()
    const effects = makeSubmitEffects({ values: setPostUrl(baseline, 'xx') })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.notifyInvalid, '脱敏分支下垃圾串应被拒').toHaveBeenCalledTimes(1)
    expect(effects.apply, '非法值不得提交（否则静默覆盖真实 URL）').not.toHaveBeenCalled()
    expect(effects.commitBaseline, '被拒的提交不得更新 baseline').not.toHaveBeenCalled()
  })

  it('6) ★ 非脱敏 baseline（持真值）+ "xx"（非 URL）⇒ 同样被拦（收紧已扩到所有提交路径，不再只限脱敏分支）', async () => {
    const baseline = realFormModel()
    const effects = makeSubmitEffects({ values: setPostUrl(baseline, 'xx') })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.notifyInvalid, '所有提交路径都做 URL 形状校验').toHaveBeenCalledTimes(1)
    expect(effects.apply, '非法值在任何分支都不得提交').not.toHaveBeenCalled()
    expect(effects.commitBaseline, '被拒的提交不得更新 baseline').not.toHaveBeenCalled()
  })

  it('7) ★ 占位符误删（值 "*" / "**"）⇒ 判非法并拒绝提交（只认字面哨兵 "***"；旧实现按"未变更"放行）', async () => {
    const baseline = maskedFormModel()
    for (const typed of ['*', '**']) {
      const effects = makeSubmitEffects({ values: setPostUrl(baseline, typed) })
      await makeSettingsSubmitHandler(() => baseline, effects)()
      expect(effects.notifyInvalid, `值 ${typed} 非字面哨兵、非合法 URL ⇒ 报校验错`).toHaveBeenCalledTimes(1)
      expect(effects.notifyNoChange, `值 ${typed} 不是"未改动"，不得提示无变更`).not.toHaveBeenCalled()
      expect(effects.apply, `值 ${typed} 不得提交`).not.toHaveBeenCalled()
    }
  })

  it('8) ★ apply 抛异常 ⇒ notifyError 且**收尾句柄**仍被调用（回归钉住既有缺陷：catch 路径漏收尾 loading）', async () => {
    // 回归钉住的对象是**既有组件实现**：其 catch 分支只 message.error、**没有** hide()，
    //   而 message.loading(..., 0) 是"永不自动关闭"的 ⇒ 保存失败后 loading 蒙层**永久卡屏**。
    //   本用例钉住"异常路径也必须收尾 loading"，防止该缺陷在重构中被原样带过来。
    const baseline = maskedFormModel()
    const boom = new Error('boom')
    const effects = makeSubmitEffects({
      values: setPostUrl(baseline, 'http://a'),
      apply: async () => {
        throw boom
      },
    })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.notifyError, 'apply 失败必须报错').toHaveBeenCalled()
    expect(effects.describeError, '错误文案应经 describeError 生成').toHaveBeenCalled()
    expect(effects.finish, '异常路径也必须收尾 loading（否则 loading 永久卡屏）').toHaveBeenCalledTimes(1)
  })

  it('9) loading 起止成对：成功与异常路径都是「start 一次 + 收尾一次」', async () => {
    const baseline = maskedFormModel()
    const ok = makeSubmitEffects({ values: setPostUrl(baseline, 'http://a') })
    await makeSettingsSubmitHandler(() => baseline, ok)()
    expect(ok.notifyLoadingStart, '成功路径起 loading 一次').toHaveBeenCalledTimes(1)
    expect(ok.finish, '成功路径收 loading 一次').toHaveBeenCalledTimes(1)

    const bad = makeSubmitEffects({
      values: setPostUrl(baseline, 'http://a'),
      apply: async () => {
        throw new Error('x')
      },
    })
    await makeSettingsSubmitHandler(() => baseline, bad)()
    expect(bad.notifyLoadingStart, '异常路径起 loading 一次').toHaveBeenCalledTimes(1)
    expect(bad.finish, '异常路径收 loading 一次').toHaveBeenCalledTimes(1)
  })

  it('9b) ★ 成功路径的后续回调抛错（notifySuccess 抛）⇒ 收尾仍**恰好一次**（随调用局部的一次性守卫）', async () => {
    // 对抗性审计指出的边角：若收尾只在"成功分支"里直接调，而成功分支的后半段（notifySuccess /
    //   commitBaseline）抛错，就会落进 catch **再收尾一次**（双调）。实现用随调用局部的一次性守卫消除之。
    const baseline = maskedFormModel()
    const effects = makeSubmitEffects({
      values: setPostUrl(baseline, 'http://a'),
      notifySuccess: () => {
        throw new Error('toast 抛了')
      },
    })
    await makeSettingsSubmitHandler(() => baseline, effects)()
    expect(effects.finish, '收尾必须恰好一次（不得因成功分支后半段抛错而双调）').toHaveBeenCalledTimes(1)
    expect(effects.commitBaseline, 'notifySuccess 抛错后，commitBaseline 不应被执行（顺序如此）').not.toHaveBeenCalled()
    expect(effects.notifyError, '该抛错落进 catch ⇒ 报错（防御性行为）').toHaveBeenCalledTimes(1)
  })

  it('10) 成功路径不调 notifyError；异常路径不调 notifySuccess 与 commitBaseline', async () => {
    const baseline = maskedFormModel()
    const ok = makeSubmitEffects({ values: setPostUrl(baseline, 'http://a') })
    await makeSettingsSubmitHandler(() => baseline, ok)()
    expect(ok.notifyError, '成功路径不得报错').not.toHaveBeenCalled()

    const bad = makeSubmitEffects({
      values: setPostUrl(baseline, 'http://a'),
      apply: async () => {
        throw new Error('x')
      },
    })
    await makeSettingsSubmitHandler(() => baseline, bad)()
    expect(bad.notifySuccess, '异常路径不得报成功').not.toHaveBeenCalled()
    expect(bad.commitBaseline, '异常路径不得更新 baseline').not.toHaveBeenCalled()
  })
})

// ---- 接线形态守卫 -----------------------------------------------------------

// 读**生产源码**（node 环境直接读文件，不经 Vite 的 import 变换）；路径由本测试文件位置推出：
//   web/src/app/utils/__tests__ → ../../pages/ServerSettingsTab.tsx。
const testFileDir = dirname(fileURLToPath(import.meta.url))
const serverSettingsTabSource = readFileSync(resolve(testFileDir, '../../pages/ServerSettingsTab.tsx'), 'utf8')

// 去掉 `/* */` 块注释与 `//` 行注释后的源码。说明：正则是朴素实现（例如字符串里的 `//`，如 'http://'
//   斜杠之后的部分也会被吞掉）；但本守卫只搜两个不与字符串混同的调用记号
//   （`makeSettingsSubmitHandler(` / `buildSettingsSubmission(`），朴素剥离足够。剥离的目的是避免
//   "注释里提到某记号"造成假阳性/假阴性。
function stripComments(src: string): string {
  return src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '')
}

describe('接线形态守卫（⚠️ 文本判据，非行为覆盖）', () => {
  // ⚠️ 局限如实登记：本组是**形态/文本判据** —— 只看去注释后的源码里是否出现某个调用记号。
  //   · 挡不住**等价改写**：把工厂调用包进一个别名函数、或挪到别的模块再 re-export，记号仍在别处，本守卫仍绿；
  //   · 挡不住**条件恒假**：工厂被调用了，但其结果从不进入 onFinish（例如赋给一个未使用的变量）时，本守卫仍绿。
  //   ⇒ 它只是上方 makeSettingsSubmitHandler 行为用例之外的**补充**，**不得**被声称等价于行为覆盖。
  it('正向：组件源码（去注释后）含 makeSettingsSubmitHandler( 调用', () => {
    expect(stripComments(serverSettingsTabSource), '组件应由工厂接线提交处理器').toContain(
      'makeSettingsSubmitHandler(',
    )
  })

  it('负向：组件源码（去注释后）不再内联 buildSettingsSubmission(（归一化 + diff 的接线已搬进工厂）', () => {
    expect(stripComments(serverSettingsTabSource), '组件不应再直接调用 buildSettingsSubmission').not.toContain(
      'buildSettingsSubmission(',
    )
  })
})
