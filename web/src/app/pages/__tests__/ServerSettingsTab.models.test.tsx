import { act, type ReactNode } from 'react'
import { createRoot, type Root } from 'react-dom/client'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

import { defaultPrefs } from '../../state/prefs'
import type { ModelsResponse, ServerSettingsDoc } from '../../api/types'
import { buildServerFormModelFromDoc, SETTINGS_SECTIONS } from '../../utils/serverFormModel'

const h = vi.hoisted(() => ({
  getModels: vi.fn(),
  tag: vi.fn(),
  item: vi.fn(),
  formRender: vi.fn(),
  updateServerSettings: vi.fn(),
  serverSettings: { status: 'idle', data: undefined as ServerSettingsDoc | undefined },
  form: { getFieldsValue: vi.fn(), setFieldsValue: vi.fn() },
}))

vi.mock('../../api/models', () => ({ getModels: h.getModels, reloadModel: vi.fn() }))
vi.mock('../../state/AppStore', () => ({
  useAppStore: () => ({
    prefs: defaultPrefs,
    serverSettings: h.serverSettings,
    updateServerSettings: h.updateServerSettings,
    refreshServerSettings: vi.fn(),
  }),
}))
vi.mock('../../components/FlipSwitch', () => ({ default: () => null }))
vi.mock('@ant-design/icons', () => ({
  KeyOutlined: () => null, ReloadOutlined: () => null,
  SaveOutlined: () => null, UndoOutlined: () => null,
}))
// 透传容器，让真实设置页的模型状态分支和 Tag 接线执行；仅替换依赖浏览器的 antd 控件。
vi.mock('antd', () => {
  const pass = ({ children }: { children?: ReactNode }) => children
  const Form = Object.assign((props: { children?: ReactNode }) => { h.formRender(props); return props.children }, {
    Item: (props: unknown) => { h.item(props); return null }, useForm: () => [h.form],
  })
  return {
    Form,
    Card: ({ children, title }: { children?: ReactNode; title?: ReactNode }) => <>{title}{children}</>,
    Collapse: ({ items }: { items: { key: string; children: ReactNode }[] }) => <>{items.map(i => <div key={i.key}>{i.children}</div>)}</>,
    Tag: (props: { color?: string; children?: ReactNode }) => { h.tag(props); return null },
    Space: pass, Tooltip: pass, Popconfirm: pass,
    Descriptions: Object.assign(({ children }: { children?: ReactNode }) => children, { Item: () => null }),
    Typography: { Text: () => null, Paragraph: () => null },
    Alert: () => null, Button: () => null, Spin: () => null,
    Input: () => null, InputNumber: () => null, Select: () => null,
    message: { info: vi.fn(), error: vi.fn(), warning: vi.fn(), loading: () => () => {}, success: vi.fn() },
  }
})

import { ServerSettingsTab } from '../ServerSettingsTab'

let root: Root | undefined
function mountContainer() {
  const noop = () => {}
  const doc = {
    addEventListener: noop, removeEventListener: noop,
    createTextNode: (text: string) => ({ nodeType: 3, textContent: text }),
  }
  Object.assign(globalThis, {
    document: doc, window: globalThis, HTMLIFrameElement: class {}, IS_REACT_ACT_ENVIRONMENT: true,
  })
  const element = () => ({
    nodeType: 1, tagName: 'DIV', namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: doc,
    style: {}, addEventListener: noop, removeEventListener: noop, appendChild: noop, removeChild: noop,
    insertBefore: noop, setAttribute: noop, removeAttribute: noop,
  })
  Object.assign(doc, { createElement: element })
  return element() as unknown as Element
}

beforeEach(() => {
  h.getModels.mockReset(); h.tag.mockReset(); h.item.mockReset(); h.formRender.mockReset()
  h.updateServerSettings.mockReset(); h.form.getFieldsValue.mockReset(); h.form.setFieldsValue.mockReset()
  h.serverSettings = { status: 'idle', data: undefined }
})
afterEach(async () => {
  if (root) await act(async () => { root!.unmount() })
  root = undefined
  const globals = globalThis as unknown as Record<string, unknown>
  for (const key of ['document', 'window', 'HTMLIFrameElement', 'IS_REACT_ACT_ENVIRONMENT']) delete globals[key]
})

describe('模型状态契约与真实设置页渲染', () => {
  it('真实设置页保存 profile 目录后可显式清空，提交仅包含该字段', async () => {
    const doc = { schemaVersion: 1, ...Object.fromEntries(SETTINGS_SECTIONS.map(section => [section, {}])) } as unknown as ServerSettingsDoc
    h.serverSettings = { status: 'loaded', data: doc }
    h.getModels.mockResolvedValue({ ok: true, data: { activeModels: [], supportedModels: [], summary: {} } })
    root = createRoot(mountContainer())
    await act(async () => { root!.render(<ServerSettingsTab />) })
    const { model } = buildServerFormModelFromDoc(doc)
    h.form.getFieldsValue.mockReturnValue({ ...model, model: { ...model.model, faceProfileDir: 'storage/profiles/synthetic' } })
    await act(async () => { await h.formRender.mock.calls.at(-1)![0].onFinish() })
    expect(h.updateServerSettings).toHaveBeenLastCalledWith({ model: { faceProfileDir: 'storage/profiles/synthetic' } })
    h.form.getFieldsValue.mockReturnValue({ ...model, model: { ...model.model, faceProfileDir: '' } })
    await act(async () => { await h.formRender.mock.calls.at(-1)![0].onFinish() })
    expect(h.updateServerSettings).toHaveBeenCalledTimes(2)
    expect(h.updateServerSettings).toHaveBeenLastCalledWith({ model: { faceProfileDir: '' } })
  })

  it('模型设置提供 profile 目录输入并说明旧配置语义', async () => {
    h.getModels.mockResolvedValue({ ok: true, data: { activeModels: [], supportedModels: [], summary: {} } })
    root = createRoot(mountContainer())
    await act(async () => { root!.render(<ServerSettingsTab />) })
    const field = h.item.mock.calls.map(([props]) => props).find(props => props.name?.join('.') === 'model.faceProfileDir')
    expect(field).toBeDefined()
    expect(field.extra).toContain('留空')
    expect(field.extra).toContain('清单')
  })
  it('disabled 卡片显示中性已禁用并独立计数，不冒充缺失', async () => {
    const data: ModelsResponse = {
      supportedModels: [],
      activeModels: (['loaded', 'failed', 'missing', 'disabled'] as const).map(status => ({
        id: status, displayName: status, taskType: 'detect', backend: 'opencv_dnn', status, isInUse: false,
      })),
      summary: { totalSupported: 0, totalConfigured: 4, totalLoaded: 1, totalFailed: 1, totalMissing: 1 },
    }
    h.getModels.mockResolvedValue({ ok: true, data })
    root = createRoot(mountContainer())
    await act(async () => { root!.render(<ServerSettingsTab />) })
    const tags = h.tag.mock.calls.map(([props]) => ({
      text: Array.isArray(props.children) ? props.children.join('') : props.children,
      color: props.color,
    }))
    expect(tags.filter(t => t.text === '缺失')).toHaveLength(1)
    expect(tags).toContainEqual({ text: '已禁用', color: 'default' })
    expect(tags).toContainEqual({ text: '已禁用 1', color: 'default' })
  })

})
