import {
  Alert,
  Button,
  Card,
  Collapse,
  Form,
  Input,
  InputNumber,
  Popconfirm,
  Select,
  Switch,
  Space,
  Tabs,
  Tooltip,
  Typography,
  message,
} from 'antd'
import { useEffect, useState } from 'react'

import { rotateCryptoKey } from '../api/actions'
import { useAppStore } from '../state/AppStore'
import { diffPatch } from '../utils/diffPatch'

type LocalFormModel = {
  theme: 'system' | 'light' | 'dark'
  language: 'zh-CN' | 'en-US'
  startPage: 'home' | 'preview' | 'settings'
  apiTimeoutMs: number
  logLevel: 'silent' | 'error' | 'warn' | 'info' | 'debug'
  cacheStrategy: 'no-cache' | 'memory-30s' | 'local-5m'
}

type ServerFormModel = {
  http: { enable: boolean; port: number }
  log: { logDir: string; maxFileBytes: number; maxRollFiles: number }
  poster: {
    enable: boolean
    postUrl: string // GET 可能返回 "***"（脱敏）；表单这里用空串表示“未填写/不变更”
    throttleMs: number
    backoffMinMs: number
    backoffMaxMs: number
  }
  ui: { windowWidth: number; windowHeight: number; previewScaleMode: number }
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any
function buildServerFormModelFromDoc(doc: Record<string, any>): { model: ServerFormModel; postUrlMasked: boolean } {
  const masked = doc?.poster?.postUrl === '***'
  return {
    postUrlMasked: masked,
    model: {
      http: { enable: !!doc?.http?.enable, port: Number(doc?.http?.port ?? 8080) },
      log: {
        logDir: String(doc?.log?.logDir ?? ''),
        maxFileBytes: Number(doc?.log?.maxFileBytes ?? 10 * 1024 * 1024),
        maxRollFiles: Number(doc?.log?.maxRollFiles ?? 5),
      },
      poster: {
        enable: !!doc?.poster?.enable,
        postUrl: masked ? '' : String(doc?.poster?.postUrl ?? ''),
        throttleMs: Number(doc?.poster?.throttleMs ?? 100),
        backoffMinMs: Number(doc?.poster?.backoffMinMs ?? 200),
        backoffMaxMs: Number(doc?.poster?.backoffMaxMs ?? 5000),
      },
      ui: {
        windowWidth: Number(doc?.ui?.windowWidth ?? 1280),
        windowHeight: Number(doc?.ui?.windowHeight ?? 800),
        previewScaleMode: Number(doc?.ui?.previewScaleMode ?? 0),
      },
    },
  }
}

export function SettingsPage() {
  const { prefs, setPrefs, serverSettings, updateServerSettings, refreshServerSettings } = useAppStore()
  const [localForm] = Form.useForm<LocalFormModel>()
  const [serverForm] = Form.useForm<ServerFormModel>()
  const [serverBaseline, setServerBaseline] = useState<ServerFormModel | null>(null)
  const [postUrlMasked, setPostUrlMasked] = useState(false)
  const [isRotatingKey, setIsRotatingKey] = useState(false)

  // 本地设置：用 onValuesChange 实时生效（减少“点保存但忘了”的坑）
  useEffect(() => {
    localForm.setFieldsValue({
      theme: prefs.theme,
      language: prefs.language,
      startPage: prefs.startPage,
      apiTimeoutMs: prefs.apiTimeoutMs,
      logLevel: prefs.logLevel,
      cacheStrategy: prefs.cacheStrategy,
    })
  }, [localForm, prefs])

  // 后端 settings：拉到数据时作为 baseline，并填充表单
  useEffect(() => {
    if (!serverSettings.data) return
    const { model, postUrlMasked } = buildServerFormModelFromDoc(serverSettings.data)
    setServerBaseline(model)
    setPostUrlMasked(postUrlMasked)
    serverForm.setFieldsValue(model)
  }, [serverForm, serverSettings.data])

  const localTab = (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card title="应用设置（本地，写入 localStorage）">
        <Form
          form={localForm}
          layout="vertical"
          onValuesChange={(_: Partial<LocalFormModel>, all: LocalFormModel) => {
            setPrefs(all)
          }}
        >
          <Collapse
            defaultActiveKey={['basic']}
            items={[
              {
                key: 'basic',
                label: '基础（主题 / 语言 / 启动页）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="主题" name="theme">
                      <Select
                        options={[
                          { value: 'system', label: '跟随系统' },
                          { value: 'light', label: '浅色' },
                          { value: 'dark', label: '深色' },
                        ]}
                      />
                    </Form.Item>
                    <Form.Item label="语言" name="language">
                      <Select
                        options={[
                          { value: 'zh-CN', label: '中文' },
                          { value: 'en-US', label: 'English' },
                        ]}
                      />
                    </Form.Item>
                    <Form.Item
                      label="启动页"
                      name="startPage"
                      tooltip="下一次打开页面时默认进入哪个模块"
                    >
                      <Select
                        options={[
                          { value: 'home', label: '概览' },
                          { value: 'preview', label: '预览' },
                          { value: 'settings', label: '设置' },
                        ]}
                      />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'advanced',
                label: '高级（API 超时 / 日志级别 / 缓存策略）',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item
                      label="API 超时（ms）"
                      name="apiTimeoutMs"
                      tooltip="前端请求 /api/v1/* 的超时阈值；过小会误判，过大会拖慢卡死体验"
                    >
                      <InputNumber min={500} max={120000} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item
                      label="前端日志级别"
                      name="logLevel"
                      tooltip="仅影响浏览器控制台输出，不影响后端落盘日志"
                    >
                      <Select
                        options={[
                          { value: 'silent', label: '静默' },
                          { value: 'error', label: '错误' },
                          { value: 'warn', label: '警告' },
                          { value: 'info', label: '信息' },
                          { value: 'debug', label: '调试' },
                        ]}
                      />
                    </Form.Item>
                    <Form.Item
                      label="缓存策略"
                      name="cacheStrategy"
                      tooltip="仅作用于 GET /api/v1/settings；调试时建议 no-cache"
                    >
                      <Select
                        options={[
                          { value: 'no-cache', label: '不缓存' },
                          { value: 'memory-30s', label: '内存 30s' },
                          { value: 'local-5m', label: '本地 5min' },
                        ]}
                      />
                    </Form.Item>
                  </Space>
                ),
              },
            ]}
          />
        </Form>
      </Card>

      <Alert
        type="info"
        showIcon
        message="说明"
        description="“应用设置”不写入后端 /api/v1/settings（后端 schema 禁止多余字段），仅用于控制前端行为。"
      />
    </Space>
  )

  const serverTab = (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card
        title="后端设置（/api/v1/settings）"
        extra={
          <Button onClick={() => refreshServerSettings()} loading={serverSettings.status === 'loading'}>
            重新拉取
          </Button>
        }
      >
        {serverSettings.status === 'error' ? (
          <Alert
            type="error"
            showIcon
            message="读取失败"
            description={serverSettings.error.message}
            style={{ marginBottom: 12 }}
          />
        ) : null}

        <Form
          form={serverForm}
          layout="vertical"
          disabled={!serverSettings.data}
          onFinish={async (values: ServerFormModel) => {
            if (!serverBaseline) {
              message.warning('尚未加载 baseline，请先拉取一次后端设置')
              return
            }

            // 若 postUrl 是脱敏字段：空串表示“用户未填写，不变更”
            const normalized: ServerFormModel = {
              ...values,
              poster: {
                ...values.poster,
                postUrl:
                  postUrlMasked && values.poster.postUrl.trim() === ''
                    ? serverBaseline.poster.postUrl
                    : values.poster.postUrl.trim(),
              },
            }

            const patch = diffPatch(serverBaseline, normalized)
            if (!patch) {
              message.info('没有检测到变更')
              return
            }
            try {
              const hide = message.loading('正在应用设置并重启引擎...', 0)
              await updateServerSettings(patch)
              hide()
              message.success('保存成功')
              setServerBaseline(normalized) // 作为新的 baseline
            } catch (e: unknown) {
              message.error((e as Error)?.message || '保存失败')
            }
          }}
        >
          <Collapse
            defaultActiveKey={['http', 'ui']}
            items={[
              {
                key: 'acceleration',
                label: '硬件加速 (Acceleration)',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="OpenCL 加速" name={['acceleration', 'enableOpenCL']} valuePropName="checked">
                      <Switch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="RK MPP 硬件解码" name={['acceleration', 'enableMpp']} valuePropName="checked">
                      <Switch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item label="Qualcomm SNPE 推理加速" name={['acceleration', 'enableQualcomm']} valuePropName="checked">
                      <Switch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                  </Space>
                ),
              },
              {
                key: 'http',
                label: 'HTTP 服务',
                children: (
                  <Space direction="vertical" size={12} style={{ width: '100%' }}>
                    <Form.Item label="启用" name={['http', 'enable']} valuePropName="checked">
                      <Switch checkedChildren="启用" unCheckedChildren="禁用" />
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
                      <Switch checkedChildren="启用" unCheckedChildren="禁用" />
                    </Form.Item>
                    <Form.Item
                      label="POST URL"
                      name={['poster', 'postUrl']}
                      tooltip={
                        postUrlMasked
                          ? '出于安全考虑，后端 GET 会把该字段脱敏为 "***"；如需修改，请在此处重新填写完整 URL'
                          : '可留空表示不变更'
                      }
                    >
                      <Input placeholder={postUrlMasked ? '（已脱敏）如需修改请填写' : 'http://...'} />
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
                    <Form.Item label="日志目录" name={['log', 'logDir']}>
                      <Input placeholder="storage/win_logs" />
                    </Form.Item>
                    <Form.Item label="单文件最大字节数" name={['log', 'maxFileBytes']}>
                      <InputNumber min={1024} max={1024 * 1024 * 1024} style={{ width: '100%' }} />
                    </Form.Item>
                    <Form.Item label="滚动文件数" name={['log', 'maxRollFiles']}>
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
                      tooltip="数值由后端定义（例如 0=fit / 1=fill 等）。如果不知道含义，请保持默认值。"
                    >
                      <InputNumber min={0} max={10} style={{ width: '100%' }} />
                    </Form.Item>
                  </Space>
                ),
              },
            ]}
          />

          <div style={{ marginTop: 12 }}>
            <Space>
              <Tooltip title={!serverSettings.data ? '尚未加载后端设置，无法保存' : ''}>
                <span>
                  <Button
                    type="primary"
                    htmlType="submit"
                    loading={serverSettings.status === 'loading'}
                    disabled={!serverSettings.data}
                    style={{ pointerEvents: !serverSettings.data ? 'none' : 'auto' }}
                  >
                    保存到后端
                  </Button>
                </span>
              </Tooltip>
              <Tooltip title={!serverSettings.data ? '尚未加载后端设置，无法轮换密钥' : ''}>
                <span>
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
                        message.error((e as Error)?.message || '密钥轮换失败')
                      } finally {
                        setIsRotatingKey(false)
                      }
                    }}
                    disabled={!serverSettings.data}
                  >
                    <Button
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
                <span>
                  <Button
                    disabled={!serverBaseline}
                    style={{ pointerEvents: !serverBaseline ? 'none' : 'auto' }}
                    onClick={() => {
                      if (serverBaseline) serverForm.setFieldsValue(serverBaseline)
                      message.info('已恢复为上次拉取/保存的值')
                    }}
                  >
                    恢复
                  </Button>
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

  const items = [
    { key: 'local', label: '应用（本地）', children: localTab },
    { key: 'server', label: '后端（API）', children: serverTab },
  ]

  return (
    <Tabs
      items={items}
      defaultActiveKey="local"
      destroyInactiveTabPane={false}
    />
  )
}

