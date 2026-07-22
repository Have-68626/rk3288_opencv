import { KeyOutlined, ReloadOutlined, SaveOutlined, UndoOutlined } from '@ant-design/icons'
import {
  Alert,
  Button,
  Card,
  Collapse,
  Descriptions,
  Empty,
  Form,
  Input,
  InputNumber,
  Popconfirm,
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
import { useAppStore } from '../state/AppStore'
import { diffPatch } from '../utils/diffPatch'

type ServerFormModel = {
  http: { enable: boolean; port: number }
  log: { logDir: string; maxFileBytes: number; maxRollFiles: number }
  poster: {
    enable: boolean
    postUrl: string // GET 可能返回 "***"（脱敏）；表单这里用空串表示"未填写/不变更"
    throttleMs: number
    backoffMinMs: number
    backoffMaxMs: number
  }
  ui: { windowWidth: number; windowHeight: number; previewScaleMode: number }
  acceleration: {
    enableOpenCL: boolean
    enableLibyuv: boolean
    enableMpp: boolean
    enableQualcomm: boolean
  }
}

function buildServerFormModelFromDoc(doc: ServerSettingsDoc): { model: ServerFormModel; postUrlMasked: boolean } {
  const masked = doc.poster?.postUrl === '***'
  return {
    postUrlMasked: masked,
    model: {
      http: { enable: !!doc.http?.enable, port: Number(doc.http?.port ?? 8080) },
      log: {
        logDir: String(doc.log?.logDir ?? ''),
        maxFileBytes: Number(doc.log?.maxFileBytes ?? 10 * 1024 * 1024),
        maxRollFiles: Number(doc.log?.maxRollFiles ?? 5),
      },
      poster: {
        enable: !!doc.poster?.enable,
        postUrl: masked ? '' : String(doc.poster?.postUrl ?? ''),
        throttleMs: Number(doc.poster?.throttleMs ?? 100),
        backoffMinMs: Number(doc.poster?.backoffMinMs ?? 200),
        backoffMaxMs: Number(doc.poster?.backoffMaxMs ?? 5000),
      },
      ui: {
        windowWidth: Number(doc.ui?.windowWidth ?? 1280),
        windowHeight: Number(doc.ui?.windowHeight ?? 800),
        previewScaleMode: Number(doc.ui?.previewScaleMode ?? 0),
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

            // 若 postUrl 是脱敏字段：空串表示"用户未填写，不变更"
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
              message.error(getErrorMessage(e) || '保存失败')
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
                                  color={m.status === 'loaded' ? 'green' : m.status === 'failed' ? 'red' : 'orange'}
                                  style={{ marginRight: 0 }}
                                >
                                  {m.status === 'loaded' ? '已加载' : m.status === 'failed' ? '失败' : '缺失'}
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
                          <Empty
                            image={Empty.PRESENTED_IMAGE_SIMPLE}
                            description="暂无活跃模型（管线未启动或无配置）"
                          />
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
                <span style={{ display: 'inline-block' }} tabIndex={!serverSettings.data ? 0 : undefined} role={!serverSettings.data ? 'button' : undefined} aria-disabled={!serverSettings.data ? true : undefined}>
                  <Button
                    type="primary"
                    htmlType="submit"
                    icon={<SaveOutlined />}
                    loading={serverSettings.status === 'loading'}
                    disabled={!serverSettings.data}
                    style={{ pointerEvents: !serverSettings.data ? 'none' : 'auto' }}
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
