import { KeyOutlined, ReloadOutlined, SaveOutlined, UndoOutlined } from '@ant-design/icons'
import {
  Alert,
  Button,
  Card,
  Collapse,
  Descriptions,
  Form,
  Input,
  InputNumber,
  Popconfirm,
  Select,
  Space,
  Spin,
  Tag,
  Tooltip,
  Typography,
  message,
} from 'antd'
import { useEffect, useState } from 'react'

import FlipSwitch from '../components/FlipSwitch'

import { rotateCryptoKey } from '../api/actions'
import { getModels, reloadModel } from '../api/models'
import type { ActiveModelInfo, ModelsResponse, ServerSettingsDoc } from '../api/types'
import { getErrorMessage } from '../api/error'
import { ApiError } from '../api/http'
import { useAppStore } from '../state/AppStore'
// ⚠️ 不直接 import diffPatch / buildSettingsSubmission：归一化 + diff + 提交流程已收进
//   serverFormModel.ts —— buildSettingsSubmission 是其内的纯逻辑，提交编排见 makeSettingsSubmitHandler；
//   本文件只 import makeSettingsSubmitHandler 接线，对 diffPatch 的提及只剩注释。若将来又需要直接调用
//   diffPatch，请连同判别用例一起加回。
import { buildServerFormModelFromDoc, isSettingsDocComplete, makeSettingsSubmitHandler, type ServerFormModel } from '../utils/serverFormModel'

// ⚠️ 覆盖边界（FACE-ARCWIN-005 / S-5）——改本文件前必读。
//
// 本表单的 ServerFormModel（web/src/app/utils/serverFormModel.ts）覆盖 ServerSettingsDoc
//   （web/src/app/api/types.ts）的**全部段**（段名清单以 serverFormModel.ts 的 SETTINGS_SECTIONS 为准，
//   此处不复述数量——避免留下会腐烂的"写死计数"；SETTINGS_SECTIONS 的长度、模型键集合、段内字段集合
//   都由 web/src/app/utils/__tests__/serverFormModel.test.ts 与 types.ts 逐一对拍钉住，段或字段集一变即红）：
//   camera / recognition / inference / dnn / model / http / poster / log / ui / display / acceleration
// ⚠️ 「模型里有这个字段」≠「UI 能编辑它」：真正决定可编辑性的，是本文件下方 Collapse 里有没有
//   对应的 <Form.Item name={[段, 键]}>。当前建模出的**每一个**字段都有对应控件；下面这条提示
//   是给将来留的——若只加建模不加控件，字段不会丢值（提交时原值原样带回，见 submitSettings 的 getValues
//   说明），但用户在 UI 上改不到它。
//   新增字段的落地清单：① types.ts（契约）→ ② serverFormModel.ts（建模 + 兜底默认值）
//   → ③ 本文件的 Collapse 控件（值域照下方"值域"一段：运行时 schema 是事实源，加两份文档镜像共三处）。
//
// 与 PreviewPage 的关系（不构成冲突）：camera 段的 preferredDeviceId / width / height / fps
//   在 PreviewPage（web/src/app/pages/PreviewPage.tsx）另有独立编辑入口；两侧都经同一个
//   updateServerSettings，后端 updateFromJsonBody 把 patch 深合并到落盘配置之上 ⇒ 谁后写谁生效。
//   ⚠️ 副作用：任一侧写入成功都会刷新 serverSettings.data ⇒ 下方同步 effect 会用新值重填表单，
//   本页未保存的编辑会被覆盖。
// camera 段的读方还有 HomePage（web/src/app/pages/HomePage.tsx）：它在摘要里只读展示
//   width / height / fps，不写。
//
// 后端本来就把**每一段**都列为 settings 的对外字段（docs/windows-web-spa/openapi.yaml 与
//   config.schema.json，以及与之一一对应的 src/win/src/JsonSchemaValidator.cpp 的
//   buildSettingsDocSchema）。本表单只是前端**便利层**：PUT /api/v1/settings 从不因
//   「某段没在本表单建模」而拒绝任何一段。
//
// 若仍有字段在 UI 上改不到（后端新增字段而本表单尚未跟进、或有意不暴露），改法：直接编辑事实源
//   %APPDATA%\rk_wcfr\config.json；INI（config/windows_camera_face_recognition.ini）仅供迁移，
//   不是事实源。
//
// 值域：下方各 InputNumber 的 min/max 是**手工镜像**，镜像对象不止一份（见下 ①–③）；
//   本文件的 min/max 是其中**唯一没人替它把关**的一份。改任一处值域，都必须把下列三处与本文件的
//   控件一同改：
//   ① **事实源** —— src/win/src/JsonSchemaValidator.cpp 的 buildSettingsDocSchema（PUT 时真正判 400 的
//      运行时 schema；见 WinJsonConfig.cpp 的 updateFromJsonBody 里那句 JsonSchemaValidator::validate）；
//   ② docs/windows-web-spa/config.schema.json；
//   ③ docs/windows-web-spa/openapi.yaml（如 CameraSettings 的 width/height/fps）。
//   ✅ ①–③ **三者之间已有机械对拍**：tests/scripts/config-contract.test.js 的
//   「值域：config.schema.json ↔ openapi.yaml ↔ JsonSchemaValidator 运行时 schema 逐键对拍
//   minimum/maximum」用例把三处的带界键逐键比对（CI 的 repo-hygiene job 会跑它），任一处改 min/max、
//   或只改其中一两处，都会变红。
//   ⚠️ 真正的残余缺口是**本文件这一份**：UI 自己的 min/max 不在该用例射程内（它只抽上述三处，
//   不读本文件）⇒"UI 放行了后端会拒的值（点保存才撞 400）"或"UI 与三处悄悄分叉"仍不会被 CI 拦住，
//   只能靠人工在改三处时一并改这里的控件。
// 路径字段（分两档，依据 src/win/src/WinConfig.cpp 的 checkConfigPath 两处调用）：
//   · **写型**（recognition.databasePath、log.logDir）：PUT 时以 Strict 校验，相对路径含 `..`
//     段会被 400 拒绝且**整次 PUT 都不生效**；绝对路径被允许（本机既有官方用法）。
//   · **读型**（recognition.cascadePath / arcFaceModelPath、dnn.modelPath / configPath）：
//     PUT 不校验，只在后端启动期以 Warn 档对含 `..` 段的相对路径告警。
//
// 判据来源提示：PUT 失败时后端把逐字段原因放在 envelope 的 details 里（如
//   「recognition.databasePath：拒绝：路径含 `..` 段…」），本文件把 message + details 一并展示，
//   否则用户只看到「写型路径字段不合法」而不知道是哪个字段。

// 写型路径字段的 UI 提示（判据同 checkConfigPath 的 Strict 档）。
const WRITE_PATH_HINT = '相对路径不得含 `..` 段：PUT 会被 400 拒绝且整次保存都不生效；绝对路径允许。'
// 读型路径字段的 UI 提示（判据同 checkConfigPath 的 Warn 档）。
const READ_PATH_HINT = '读型路径：保存时不校验；含 `..` 段的相对路径只在后端启动期告警。'

export function ServerSettingsTab() {
  const { prefs, serverSettings, updateServerSettings, refreshServerSettings } = useAppStore()
  const [serverForm] = Form.useForm<ServerFormModel>()
  const [serverBaseline, setServerBaseline] = useState<ServerFormModel | null>(null)
  const [postUrlMasked, setPostUrlMasked] = useState(false)
  const [isRotatingKey, setIsRotatingKey] = useState(false)

  // 模型状态
  const [modelsData, setModelsData] = useState<ModelsResponse | null>(null)
  const [modelsStatus, setModelsStatus] = useState<'idle' | 'loading' | 'loaded' | 'error'>('idle')
  const [modelsError, setModelsError] = useState('')
  const [reloadingId, setReloadingId] = useState<string | null>(null)

  // 提交处理器：**流程本体在 serverFormModel.ts**（依赖注入），本组件只提供副作用（各 effect 绑定到哪个
  //   antd 调用 / AppStore 方法）。
  // ⚠️ 如实口径：被覆盖的是**流程编排**（四条分支）；**本组件这一层适配**（onFinish 挂接、
  //   `getFieldsValue(true)` 的选取、各 effect 的绑定）**仍只有一条文本形态守卫**（测试组 5），
  //   **没有**行为级覆盖——组件在 node 环境不可渲染（vitest 无 jsdom，导入它会连带拉进整棵 antd）。
  //   不要说成"组件接线已覆盖"。
  // 每次渲染重建（廉价）：`getBaseline` 是 getter ⇒ 永远读到最新的 baseline，不存在闭包过期。
  const submitSettings = makeSettingsSubmitHandler(() => serverBaseline, {
    getValues: () => serverForm.getFieldsValue(true) as ServerFormModel,
    apply: (patch) => updateServerSettings(patch as Partial<ServerSettingsDoc>),
    notifyNoChange: () => { message.info('没有检测到变更') },
    notifyInvalid: (reason) => { message.error(reason) },
    // 前置条件提示用 **warning（黄）** 而非 error（红）：原内联实现在 `!serverBaseline` 分支用的就是
    // `message.warning`。搬流程时冻结接口一度只给了 notifyError ⇒ 会把严重度从 warning 降级为 error，
    // 该回归由视图侧逐行核对被删流程时抓出并修正 —— 故保留一个独立的 warning 通道，不要并进 notifyError。
    notifyWarning: (text) => { message.warning(text) },
    // **返回本次调用的收尾句柄**（`message.loading(..., 0)` 的返回值）。刻意**不**存进渲染期共享变量：
    //   那样同一次渲染内的两次提交会互相覆盖句柄，把先者的常驻 toast（duration=0）永久留在屏上
    //   ——那正是"失败时 loading 卡屏"缺陷的复发形态（对抗性审计与独立复核各自命中）。
    notifyLoadingStart: () => message.loading('正在应用设置并重启引擎...', 0),
    notifySuccess: (text) => { message.success(text) },
    notifyError: (text) => { message.error(text) },
    commitBaseline: (normalized) => { setServerBaseline(normalized) },
    describeError: (e) => {
      // 400 的 details 是逐字段原因（路径校验失败时即在此），只显示 message 用户无从定位
      const details = e instanceof ApiError ? e.details : undefined
      const base = getErrorMessage(e) || '保存失败'
      return details && details.length > 0 ? `${base}：${details.join('；')}` : base
    },
  })

  // —— getValues 为什么取整份 store（getFieldsValue(true)），而不是 onFinish 回调的 values 参数 ——
  //   rc-field-form 的 validateFields() 只收集**已挂载**的字段（getFieldEntities(true)），而下方 Collapse 的
  //   未展开面板是**惰性挂载**的 ⇒ 从未展开过的面板里的字段根本不在回调参数里。
  //   历史反例（旧内联流程取回调参数时）：poster 面板从未展开 ⇒ values.poster 是 undefined ⇒ 随后那句
  //   values.poster.postUrl.trim() 抛 TypeError；该异常发生在 async onFinish 体内 ⇒ 只会变成 onFinish 返回的
  //   **rejected promise**，而 rc-field-form 的 submit 只把 onFinish 包在**同步** try/catch 里（接不到异步
  //   拒绝）⇒ 症状是"点保存毫无反应"（只有控制台 unhandled rejection）。改读整份 store 后症状消失：store
  //   由上方同步 effect 的 setFieldsValue(整份 model) 播种 ⇒ 未展开面板的字段值也在 store 里（仍是文档原值）。
  //   ⚠️ 顺带更正一处旧措辞（原称"回调参数缺段 ⇒ 提交时会被当成用户清空/未填写 ⇒ 整段丢失或被兜底默认值
  //   覆盖"）——**不成立**：流程本体（serverFormModel.ts 的 makeSettingsSubmitHandler）里的 normalized
  //   **不注入任何兜底默认值**，而 diffPatch 对 `undefined` **直接跳过**（见 diffPatch.ts 的
  //   `if (d !== undefined)`）⇒ 某段若真的缺失，后果只是**该段不提交**，既不会被清空、也不会被默认值覆盖。
  //   ⇒ 面板惰性渲染只影响可编辑性，不影响提交的正确性；"未展开面板不丢字段"的真正承重者是
  //   「整份 store 已被播种」+「diffPatch 只提交变化项（undefined 跳过）」这一对前提
  //   （由 serverFormModel.test.ts 的"未展开面板"一组钉住）。
  // 归一化（哨兵/全 `*` 折回 baseline、删空 = 显式清空）+ 最小 patch + 提交与提示的**编排**都在
  //   makeSettingsSubmitHandler 内；本文件不再直接调用 buildSettingsSubmission，只经上方 effects 提供副作用。

  useEffect(() => {
    setModelsStatus('loading')
    getModels(prefs)
      .then((env) => {
        if (env.ok) {
          setModelsData(env.data)
          setModelsStatus('loaded')
        } else {
          setModelsError(env.error.message)
          setModelsStatus('error')
        }
      })
      .catch((e) => {
        setModelsError(e?.message || '获取模型状态失败')
        setModelsStatus('error')
      })
  }, [prefs])

  // 后端 settings：拉到数据时作为 baseline，并填充表单
  useEffect(() => {
    if (!serverSettings.data) return
    const { model, postUrlMasked } = buildServerFormModelFromDoc(serverSettings.data)
    setServerBaseline(model)
    setPostUrlMasked(postUrlMasked)
    serverForm.setFieldsValue(model)
  }, [serverForm, serverSettings.data])

  // 可保存判据 = 有文档 **且** 文档带齐全部段（段名清单见 serverFormModel.ts 的 SETTINGS_SECTIONS）。
  // ⚠️ 更正旧理由（原称"缺段文档若能提交，兜底默认值就会被当成用户的编辑写回去、把真实配置整段
  //   覆盖成默认值"）——**不成立**：baseline 与表单模型由**同一份文档、同一个
  //   buildServerFormModelFromDoc** 构造（见上方同步 effect，返回的 model 同时喂给 setServerBaseline
  //   与 setFieldsValue）⇒ 兜底值在两侧同值 ⇒ diffPatch 恒判"未变更"，兜底值**提交不出去**。
  //   本闸门的真实价值是 UX / 防御性的：文档缺失（错误 stub，或将来的极旧版本文档）时，表单里显示的
  //   是兜底值而非后端真实值，此时整份表单与保存按钮一并禁用，免得用户在"看起来有值"的表单上编辑、
  //   再撞一次后端 400。正常 GET 恒返回全部段（见 isSettingsDocComplete 的说明）。
  //   （isSettingsDocComplete 只查段是否存在、不查段内字段：段内缺字段同样因两侧同源而不会被提交，
  //     故不需要更细的判据。）
  const formReady = !!serverSettings.data && isSettingsDocComplete(serverSettings.data)
  // 后端摘要不含 disabled 计数；从同份活动快照派生，避免把禁用项算作文件缺失。
  const disabledModels = modelsData?.activeModels.filter((m) => m.status === 'disabled').length ?? 0

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card
        title="后端设置（/api/v1/settings）"
        extra={
          <Button icon={<ReloadOutlined />} onClick={() => refreshServerSettings()} loading={serverSettings.status === 'loading'}>
            重新拉取
          </Button>
        }
      >
        {serverSettings.status === 'error' ? (
          <Alert
            type="error"
            showIcon
            message="读取失败"
            description={
              <>
                <div>{serverSettings.error.message}</div>
                {/* details 是逐字段原因（校验失败时后端会填），不展示会丢失定位信息 */}
                {serverSettings.error.details && serverSettings.error.details.length > 0 ? (
                  <ul style={{ margin: '4px 0 0', paddingLeft: 20 }}>
                    {serverSettings.error.details.map((d, i) => (
                      <li key={`${i}:${d}`}>{d}</li>
                    ))}
                  </ul>
                ) : null}
              </>
            }
            style={{ marginBottom: 12 }}
          />
        ) : null}

        <Form
          form={serverForm}
          layout="vertical"
          disabled={!formReady}
          onFinish={submitSettings}
        >
          <Collapse
            defaultActiveKey={['http', 'ui']}
            items={[
              {
                key: 'camera',
                label: '相机（Camera）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item
                      label="首选设备 ID"
                      name={['camera', 'preferredDeviceId']}
                      tooltip="设备 ID 取自「预览」页扫描到的摄像头列表；留空表示由后端自行挑选。"
                    >
                      <Input placeholder="（留空 = 由后端挑选）" />
                    </Form.Item>
                    <Form.Item label="宽度" name={['camera', 'width']}>
                      <InputNumber min={1} max={8192} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="高度" name={['camera', 'height']}>
                      <InputNumber min={1} max={8192} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="帧率（fps）" name={['camera', 'fps']}>
                      <InputNumber min={1} max={240} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'recognition',
                label: '人脸识别（Recognition）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Typography.Text type="secondary">路径</Typography.Text>
                    <Form.Item label="级联分类器路径" name={['recognition', 'cascadePath']} tooltip={READ_PATH_HINT}>
                      <Input placeholder="storage/models/haarcascade_frontalface_default.xml" />
                    </Form.Item>
                    <Form.Item label="人脸库路径" name={['recognition', 'databasePath']} tooltip={WRITE_PATH_HINT}>
                      <Input placeholder="storage/face_db" />
                    </Form.Item>
                    <Form.Item label="ArcFace 模型路径" name={['recognition', 'arcFaceModelPath']} tooltip={READ_PATH_HINT}>
                      <Input placeholder="storage/models/arcface.onnx" />
                    </Form.Item>
                    <Form.Item
                      label="ArcFace 输入节点名"
                      name={['recognition', 'arcFaceInputName']}
                      tooltip="留空 = 由 OpenCV 自动取首个输入节点。图节点名与真实 ONNX 图不符会导致推理期取不到 blob。"
                    >
                      <Input placeholder="（留空 = 自动）" />
                    </Form.Item>
                    <Form.Item
                      label="ArcFace 输出节点名"
                      name={['recognition', 'arcFaceOutputName']}
                      tooltip="留空 = 由 OpenCV 自动取首个输出节点。"
                    >
                      <Input placeholder="（留空 = 自动）" />
                    </Form.Item>

                    <Typography.Text type="secondary">阈值</Typography.Text>
                    <Form.Item
                      label="LBPH 识别阈值（卡方距离）"
                      name={['recognition', 'identifyThreshold']}
                      tooltip="LBPH 专用：卡方距离，判定为「距离 ≤ 该值」。物理值域 [1, 64]（64 是卡方距离上界）；取值 > 64 时判定恒真、阈值形同虚设。"
                    >
                      <InputNumber min={1} max={64} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item
                      label="ArcFace 识别阈值（余弦相似度）"
                      name={['recognition', 'identifyThresholdArcFace']}
                      tooltip="ArcFace 专用：余弦相似度，判定为「相似度 ≥ 该值」。值域 [0, 1]，与上面 LBPH 阈值的量纲不同，切勿互相照搬。"
                    >
                      <InputNumber min={0} max={1} step={0.01} style={{ width: '100%' }} />
                    </Form.Item>

                    <Typography.Text type="secondary">检测与注册</Typography.Text>
                    <Form.Item label="最小人脸像素" name={['recognition', 'minFaceSizePx']}>
                      <InputNumber min={10} max={10000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="注册采样张数" name={['recognition', 'enrollSamples']}>
                      <InputNumber min={1} max={1000} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'inference',
                label: '推理节流（Inference）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item
                      label="节流模式"
                      name={['inference', 'throttleMode']}
                      tooltip="后端只认 auto / manual / off（大小写不敏感）；其它取值会被静默规范化为 auto。"
                    >
                      <Select
                        style={{ width: '100%' }}
                        options={[
                          { value: 'auto', label: 'auto（自动）' },
                          { value: 'manual', label: 'manual（手动指定间隔）' },
                          { value: 'off', label: 'off（不节流）' },
                        ]}
                      />
                    </Form.Item>
                    <Form.Item label="间隔（ms）" name={['inference', 'intervalMs']}>
                      <InputNumber min={80} max={500} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'dnn',
                label: 'OpenCV DNN 检测器（dnn）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Typography.Text type="secondary">模型与输入</Typography.Text>
                    <Form.Item label="启用" name={['dnn', 'enable']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="模型路径" name={['dnn', 'modelPath']} tooltip={READ_PATH_HINT}>
                      <Input placeholder="storage/models/opencv_face_detector_uint8.pb" />
                    </Form.Item>
                    <Form.Item label="模型配置路径" name={['dnn', 'configPath']} tooltip={READ_PATH_HINT}>
                      <Input placeholder="storage/models/opencv_face_detector.pbtxt" />
                    </Form.Item>
                    <Form.Item label="输入宽度" name={['dnn', 'inputWidth']}>
                      <InputNumber min={1} max={8192} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="输入高度" name={['dnn', 'inputHeight']}>
                      <InputNumber min={1} max={8192} style={{ width: '100%' }} />
                    </Form.Item>

                    <Typography.Text type="secondary">预处理</Typography.Text>
                    <Form.Item label="缩放系数（scale）" name={['dnn', 'scale']}>
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="均值 B" name={['dnn', 'meanB']}>
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="均值 G" name={['dnn', 'meanG']}>
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="均值 R" name={['dnn', 'meanR']}>
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="交换 R/B 通道" name={['dnn', 'swapRB']} valuePropName="checked">
                      <FlipSwitch checkedChildren="是" unCheckedChildren="否" />
                    </Form.Item>

                    <Typography.Text type="secondary">后端与阈值</Typography.Text>
                    <Form.Item label="置信度阈值" name={['dnn', 'confThreshold']} tooltip="值域 [0, 1]。">
                      <InputNumber min={0} max={1} step={0.01} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="Backend" name={['dnn', 'backend']} tooltip="OpenCV dnn 的后端枚举值（0 = 默认）。">
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="Target" name={['dnn', 'target']} tooltip="OpenCV dnn 的目标设备枚举值（0 = 默认）。">
                      <InputNumber style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'model',
                label: '模型选择（Model）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item
                      label="人脸 Profile 目录"
                      name={['model', 'faceProfileDir']}
                      tooltip={READ_PATH_HINT}
                      extra="填写含 manifest.json 的目录后按清单装配检测与识别，精度和模板空间由清单决定；留空沿用旧模型配置。"
                    >
                      <Input placeholder="storage/profiles/face" />
                    </Form.Item>
                    <Form.Item label="检测模型 ID" name={['model', 'detection']} tooltip="模型 ID 字符串（如 yolo_face）。本 UI 与 schema 都不校验取值是否被后端支持，写错只会在运行期暴露。">
                      <Input placeholder="yolo_face" />
                    </Form.Item>
                    <Form.Item label="识别模型 ID" name={['model', 'recognition']} tooltip="模型 ID 字符串（如 arcface）。本 UI 与 schema 都不校验取值是否被后端支持。">
                      <Input placeholder="arcface" />
                    </Form.Item>
                    <Form.Item label="通用后端" name={['model', 'backend']} tooltip="后端名由后端规范化（如 opencv_dnn / ncnn）。">
                      <Input placeholder="opencv_dnn" />
                    </Form.Item>
                    <Form.Item label="检测后端" name={['model', 'detectorBackend']} tooltip="留空时由后端回落到「通用后端」。">
                      <Input placeholder="opencv_dnn" />
                    </Form.Item>
                    <Form.Item label="识别后端" name={['model', 'recognitionBackend']} tooltip="留空时由后端回落到「通用后端」。">
                      <Input placeholder="opencv_dnn" />
                    </Form.Item>
                    <Form.Item label="失败自动回落" name={['model', 'autoFallback']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item
                      label="旧入口请求 INT8 模型"
                      name={['model', 'int8Enabled']}
                      tooltip="仅作用于旧模型入口：置 true 仍走 OpenCV FP32，并在模型状态显示已禁用的 int8_recognition。使用 Profile 时，FP32/INT8 由 manifest.json 决定，此开关不改变清单精度。"
                    >
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'models',
                label: '模型状态 (Model Status)',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    {modelsStatus === 'loading' ? (
                      <Spin tip="正在获取模型状态..." />
                    ) : modelsStatus === 'error' ? (
                      <Alert type="error" showIcon message="获取失败" description={modelsError} />
                    ) : modelsData ? (
                      <>
                        {/* 摘要 */}
                        <Space wrap size={[4, 8]}>
                          <Tag>支持 {modelsData.summary.totalSupported}</Tag>
                          <Tag>已配置 {modelsData.summary.totalConfigured}</Tag>
                          <Tag color="green">已加载 {modelsData.summary.totalLoaded}</Tag>
                          {modelsData.summary.totalFailed > 0 && (
                            <Tag color="red">失败 {modelsData.summary.totalFailed}</Tag>
                          )}
                          {modelsData.summary.totalMissing > 0 && (
                            <Tag color="orange">缺失 {modelsData.summary.totalMissing}</Tag>
                          )}
                          {disabledModels > 0 && (
                            <Tag color="default">已禁用 {disabledModels}</Tag>
                          )}
                        </Space>

                        {/* 活跃模型 */}
                        {modelsData.activeModels.map((m: ActiveModelInfo) => (
                          <Card
                            key={m.id}
                            size="small"
                            title={
                              <Space>
                                <span>{m.displayName}</span>
                                <Tag
                                  color={m.status === 'loaded' ? 'green' : m.status === 'failed' ? 'red' : m.status === 'disabled' ? 'default' : 'orange'}
                                  style={{ marginRight: 0 }}
                                >
                                  {m.status === 'loaded' ? '已加载' : m.status === 'failed' ? '失败' : m.status === 'disabled' ? '已禁用' : '缺失'}
                                </Tag>
                                {m.isInUse && <Tag color="blue">使用中</Tag>}
                              </Space>
                            }
                            extra={
                              <Button
                                size="small"
                                loading={reloadingId === m.id}
                                disabled={reloadingId !== null}
                                onClick={async () => {
                                  setReloadingId(m.id)
                                  try {
                                    const env = await reloadModel(prefs, m.id)
                                    if (env.ok) {
                                      message.success(`已请求重载 ${m.displayName}`)
                                      // 刷新模型状态
                                      const refreshed = await getModels(prefs)
                                      if (refreshed.ok) setModelsData(refreshed.data)
                                    } else {
                                      message.error(env.error.message)
                                    }
                                  } catch (e: unknown) {
                                    message.error(getErrorMessage(e) || '重载失败')
                                  } finally {
                                    setReloadingId(null)
                                  }
                                }}
                              >
                                重载
                              </Button>
                            }
                          >
                            <Descriptions size="small" column={2}>
                              <Descriptions.Item label="ID">{m.id}</Descriptions.Item>
                              <Descriptions.Item label="后端">{m.backend}</Descriptions.Item>
                              {m.hash && <Descriptions.Item label="Hash">{m.hash.substring(0, 16)}...</Descriptions.Item>}
                              {m.modelVersion && <Descriptions.Item label="版本">{m.modelVersion}</Descriptions.Item>}
                              <Descriptions.Item label="配置路径" span={2}>
                                <Typography.Text copyable style={{ fontSize: 12 }}>{m.configuredPath}</Typography.Text>
                              </Descriptions.Item>
                              {m.resolvedPath && (
                                <Descriptions.Item label="实际路径" span={2}>
                                  <Typography.Text copyable style={{ fontSize: 12 }}>{m.resolvedPath}</Typography.Text>
                                </Descriptions.Item>
                              )}
                              {m.lastError && (
                                <Descriptions.Item label="错误信息" span={2}>
                                  <Typography.Text type="danger" style={{ fontSize: 12 }}>{m.lastError}</Typography.Text>
                                </Descriptions.Item>
                              )}
                            </Descriptions>
                          </Card>
                        ))}

                        {modelsData.activeModels.length === 0 && (
                          <Typography.Text type="secondary">暂无活跃模型（管线未启动或无配置）</Typography.Text>
                        )}
                      </>
                    ) : null}
                  </Space>
                ),
              },
              {
                key: 'http',
                label: 'HTTP 服务',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="启用" name={['http', 'enable']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="端口" name={['http', 'port']}>
                      <InputNumber min={1} max={65535} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'poster',
                label: '上报（Poster）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="启用" name={['poster', 'enable']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    {/* tooltip 两分支的如实口径（本次修复后两分支的**提交语义已统一**为「空 = 清空」）：
                        · 脱敏分支（**生产路径恒走这支**：GET/PUT 的响应体都经 currentRedactedJsonPretty）：
                          字段初值 = 哨兵 "***"（不再映射为空串），故「没动它」与「把它删空」在 diffPatch
                          眼里不同 ⇒ **删空并保存 = 显式清空该 URL**。提示必须把「清空」这条说清，并告知
                          **保存成功后该框当场就会再次显示 "***"**（PUT 响应本身即脱敏，AppStore 用它刷新
                          serverSettings.data ⇒ 同步 effect 立即重填；界面因此无法区分「已清空」与「已设置」）；
                          **不得**承诺代码做不到的事，也不得把时机说轻（原文写「重新拉取配置时」是**低估**）。
                        · 非脱敏分支只在「响应未脱敏」的假想/测试场景可达；那一支里 baseline 持有真值，
                          留空同样被提交为空值 ⇒ 与脱敏分支语义一致（旧提示「可留空表示不变更」才是与代码相反的）。 */}
                    <Form.Item
                      label="POST URL"
                      name={['poster', 'postUrl']}
                      tooltip={
                        postUrlMasked
                          ? '出于安全考虑，后端 GET/PUT 的响应都会把该字段脱敏为 "***"。把 "***" 删空并保存 = 显式清空该 URL（后端随之停止推送）；如需修改，请直接填入完整 URL。注意：保存成功后该框会立即再次显示 "***"（后端总是脱敏），界面无法区分「已清空」与「已设置」。'
                          : '留空会被提交为空值（= 清空该 URL）'
                      }
                    >
                      <Input placeholder={postUrlMasked ? '（已清空）保存后将清空该 URL' : 'http://...'} />
                    </Form.Item>
                    <Form.Item label="节流（ms）" name={['poster', 'throttleMs']}>
                      <InputNumber min={0} max={3600000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="退避最小（ms）" name={['poster', 'backoffMinMs']}>
                      <InputNumber min={50} max={3600000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="退避最大（ms）" name={['poster', 'backoffMaxMs']}>
                      <InputNumber min={50} max={3600000} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'log',
                label: '日志',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="日志目录" name={['log', 'logDir']} tooltip={WRITE_PATH_HINT}>
                      <Input placeholder="storage/win_logs" />
                    </Form.Item>
                    <Form.Item
                      label="单文件最大字节数"
                      name={['log', 'maxFileBytes']}
                      tooltip="声明上界 1 GiB；生效上界 64 MiB —— 运行期按四份文件集（recognition.csv / recognition.jsonl / event_log.jsonl，以及旧版 UI 目标的 render_metrics.jsonl）合计 512 MiB 的总预算只钳不拒地下调（单份均分额 128 MiB，本键取其一半）。填更大的值不会报错，只会被下调并告警（configStatus 转 degraded）；后端返回的是钳后生效值。"
                    >
                      <InputNumber min={1024} max={1024 * 1024 * 1024} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item
                      label="滚动文件数"
                      name={['log', 'maxRollFiles']}
                      tooltip="声明上界 1000；生效上界由运行期按同一总预算下调 = 单份均分额（128 MiB）÷ 钳后的「单文件最大字节数」− 1（默认 10 MiB 时为 11）。填大于生效上界的值不会报错，只会被下调并告警（configStatus 转 degraded）；后端返回的是钳后生效值。"
                    >
                      <InputNumber min={1} max={1000} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'ui',
                label: '窗口（UI）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="窗口宽度" name={['ui', 'windowWidth']}>
                      <InputNumber min={320} max={7680} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="窗口高度" name={['ui', 'windowHeight']}>
                      <InputNumber min={240} max={4320} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item
                      label="预览缩放模式"
                      name={['ui', 'previewScaleMode']}
                      tooltip="0 = 填充裁切（CropFill）／1 = 信箱（Letterbox），仅这两个取值合法。"
                    >
                      <InputNumber min={0} max={1} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'display',
                label: '显示（Display）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Typography.Text type="secondary">输出与刷新</Typography.Text>
                    <Form.Item label="输出序号" name={['display', 'outputIndex']}>
                      <InputNumber min={0} max={32} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="宽度" name={['display', 'width']}>
                      <InputNumber min={0} max={7680} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="高度" name={['display', 'height']}>
                      <InputNumber min={0} max={4320} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="刷新率分子" name={['display', 'refreshNumerator']}>
                      <InputNumber min={0} max={1000000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="刷新率分母" name={['display', 'refreshDenominator']}>
                      <InputNumber min={1} max={1000000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="垂直同步（vsync）" name={['display', 'vsync']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="交换链缓冲数" name={['display', 'swapchainBuffers']}>
                      <InputNumber min={1} max={8} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="全屏" name={['display', 'fullscreen']} valuePropName="checked">
                      <FlipSwitch checkedChildren="是" unCheckedChildren="否" />
                    </Form.Item>
                    <Form.Item label="允许系统切换显示模式" name={['display', 'allowSystemModeSwitch']} valuePropName="checked">
                      <FlipSwitch checkedChildren="允许" unCheckedChildren="禁止" />
                    </Form.Item>

                    <Typography.Text type="secondary">色彩与画质</Typography.Text>
                    <Form.Item label="sRGB" name={['display', 'enableSRGB']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="Gamma" name={['display', 'gamma']}>
                      <InputNumber min={0.1} max={10} step={0.1} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="色温（K）" name={['display', 'colorTempK']}>
                      <InputNumber min={1000} max={20000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="抗锯齿采样数" name={['display', 'aaSamples']}>
                      <InputNumber min={1} max={16} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="各向异性过滤等级" name={['display', 'anisoLevel']}>
                      <InputNumber min={1} max={16} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'acceleration',
                label: '硬件加速 (Acceleration)',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="OpenCL 加速" name={['acceleration', 'enableOpenCL']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item
                      label="libyuv 加速"
                      name={['acceleration', 'enableLibyuv']}
                      tooltip="本键目前没有任何运行期消费方（全仓只有配置读写/序列化与 schema 引用它）：libyuv 是否可用由构建期的 CMake 开关 RK_ENABLE_LIBYUV 决定（CMake 缺省 Android=ON / Host=OFF，但 app/build.gradle 显式传 OFF ⇒ 标准 Android Gradle 构建与 Windows 构建都不编入），运行期转换路径读的是环境变量 RK_USE_LIBYUV（src/cpp/src/Engine.cpp）。故置 true 不会启用任何加速。"
                    >
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="RK MPP 硬件解码" name={['acceleration', 'enableMpp']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="Qualcomm SNPE 推理加速" name={['acceleration', 'enableQualcomm']} valuePropName="checked">
                      <FlipSwitch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                  </Space>
                ),
              },
            ]}
          />

          <div style={{ marginTop: 12 }}>
            <Space>
              <Tooltip title={!formReady ? '尚未加载后端设置（或文档缺段），无法保存' : ''}>
                <span style={{ display: 'inline-block' }} tabIndex={!formReady ? 0 : undefined} role={!formReady ? 'button' : undefined} aria-disabled={!formReady ? true : undefined}>
                  <Button
                    type="primary"
                    htmlType="submit"
                    icon={<SaveOutlined />}
                    loading={serverSettings.status === 'loading'}
                    disabled={!formReady}
                    style={{ pointerEvents: !formReady ? 'none' : 'auto' }}
                  >
                    保存到后端
                  </Button>
                </span>
              </Tooltip>
              <Tooltip title={!serverSettings.data ? '尚未加载后端设置，无法轮换密钥' : ''}>
                <span style={{ display: 'inline-block' }} tabIndex={!serverSettings.data ? 0 : undefined} role={!serverSettings.data ? 'button' : undefined} aria-disabled={!serverSettings.data ? true : undefined}>
                  <Popconfirm
                    title="高危操作：确认轮换密钥？"
                    description="旧密钥将立即失效，所有包含敏感信息的字段会被重新加密。"
                    okText="确认轮换"
                    cancelText="取消"
                    okButtonProps={{ danger: true, loading: isRotatingKey }}
                    onConfirm={async () => {
                      try {
                        setIsRotatingKey(true)
                        await rotateCryptoKey(prefs)
                        message.success('已触发密钥轮换（敏感字段已重新加密）')
                        refreshServerSettings()
                      } catch (e: unknown) {
                        message.error(getErrorMessage(e) || '密钥轮换失败')
                      } finally {
                        setIsRotatingKey(false)
                      }
                    }}
                    disabled={!serverSettings.data}
                  >
                    <Button
                      icon={<KeyOutlined />}
                      loading={isRotatingKey}
                      disabled={!serverSettings.data}
                      danger
                      style={{ pointerEvents: !serverSettings.data ? 'none' : 'auto' }}
                    >
                      轮换密钥
                    </Button>
                  </Popconfirm>
                </span>
              </Tooltip>
              <Tooltip title={!serverBaseline ? '尚未加载基线数据，无法恢复' : ''}>
                <span style={{ display: 'inline-block' }} tabIndex={!serverBaseline ? 0 : undefined} role={!serverBaseline ? 'button' : undefined} aria-disabled={!serverBaseline ? true : undefined}>
                  <Popconfirm
                    title="确认恢复？"
                    description="将丢弃所有未保存的修改，恢复到上次拉取的设置。"
                    okText="确认"
                    cancelText="取消"
                    onConfirm={() => {
                      if (serverBaseline) serverForm.setFieldsValue(serverBaseline)
                      message.info('已恢复为上次拉取/保存的值')
                    }}
                    disabled={!serverBaseline}
                  >
                    <Button
                      icon={<UndoOutlined />}
                      disabled={!serverBaseline}
                      style={{ pointerEvents: !serverBaseline ? 'none' : 'auto' }}
                    >
                      恢复
                    </Button>
                  </Popconfirm>
                </span>
              </Tooltip>
            </Space>
          </div>
        </Form>
      </Card>

      <Alert
        type="warning"
        showIcon
        message="注意"
        description={
          <>
            <Typography.Text>
              后端启用了 schema 校验（additionalProperties=false）。如果字段名写错或多传字段，
              PUT 会返回 400。
            </Typography.Text>
          </>
        }
      />
    </Space>
  )
}
