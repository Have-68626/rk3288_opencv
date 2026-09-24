// 服务端 settings ↔ 表单模型 的结构契约测试（S-5：让"只覆盖部分段"不再复发）。
//
// 背景：本表单曾只建模 ServerSettingsDoc 的 http/log/poster/ui/acceleration，camera/recognition/
//   inference/dnn/model/display 从不进入提交的 patch —— 用户改不到它们。
// 本文件钉住：
//   1) 完整性：buildServerFormModelFromDoc 必须**逐字段**覆盖文档的每一段（不只是段名，连段内
//      字段也逐个对拍）——漏一个字段即红；
//   2) 机制：diffPatch(baseline, 只改了一处的模型) 必须只产出那一处（含嵌套路径），
//      即"改了什么就只提交什么"，未被编辑的段不会被整体回写；
//   3) 脱敏：poster.postUrl 的 "***" 哨兵映射为空串 + postUrlMasked=true（哨兵不是真值，前端不把它
//      当 URL 发回；后端另有 stripMaskedSentinels 兜底，但前端不应把密文安全托付给那一层）；
//   4) 未展开面板：整份 store 被播种后，未展开面板的字段与 baseline 同值 ⇒ diffPatch 判未变更。
//
// 环境：node（无 jsdom）。本文件只 import 纯模块（serverFormModel / diffPatch），
//   不经 antd —— 这正是把模型映射从 ServerSettingsTab.tsx 拆出来的原因之一。
import { describe, it, expect } from 'vitest'

import type { ServerSettingsDoc } from '../../api/types'
import { diffPatch } from '../diffPatch'
import { buildServerFormModelFromDoc, isSettingsDocComplete, SETTINGS_SECTIONS } from '../serverFormModel'

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

describe('poster.postUrl 脱敏哨兵', () => {
  it('哨兵 "***" → 空串 + postUrlMasked=true（判别点：哨兵不是真值——若映射成原串，前端就会把一个"非真值"发回后端）', () => {
    const doc = fullDoc()
    doc.poster.postUrl = '***'
    const { model, postUrlMasked } = buildServerFormModelFromDoc(doc)
    expect(postUrlMasked).toBe(true)
    expect(model.poster.postUrl).toBe('')
  })
  // 注：后端对"哨兵被原样回写"另有一层兜底（WinJsonConfig 的 stripMaskedSentinels 会把 patch 里等于
  //   "***" 的键剔除，语义 = 不修改）。但该兜底一旦被改动/移除，回写的字面量就会覆盖并重加密真实密文
  //   ⇒ 前端这一侧仍必须自己把哨兵映射掉，不得依赖后端兜底（两条判据独立）。

  it('非哨兵字符串原样保留 + postUrlMasked=false', () => {
    const { model, postUrlMasked } = buildServerFormModelFromDoc(fullDoc())
    expect(postUrlMasked).toBe(false)
    expect(model.poster.postUrl).toBe('http://localhost:8080/api/faces')
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
