import { DeleteOutlined, SaveOutlined } from '@ant-design/icons'
import {
  Alert,
  Button,
  Card,
  Collapse,
  Form,
  Input,
  InputNumber,
  Select,
  Space,
  message,
} from 'antd'
import { useEffect, useState } from 'react'

import { useAppStore } from '../state/AppStore'
import { clearApiToken, getApiToken, setApiToken } from '../state/prefs'

type LocalFormModel = {
  theme: 'system' | 'light' | 'dark'
  language: 'zh-CN' | 'en-US'
  startPage: 'home' | 'preview' | 'settings'
  apiTimeoutMs: number
  logLevel: 'silent' | 'error' | 'warn' | 'info' | 'debug'
  cacheStrategy: 'no-cache' | 'memory-30s' | 'local-5m'
}

export function LocalSettingsTab() {
  const { prefs, setPrefs } = useAppStore()
  const [localForm] = Form.useForm<LocalFormModel>()

  // 后端 Bearer Token 独立于 LocalPrefs：凭据用显式“保存/清除”，
  // 避免像其他偏好那样击键实时写入——那会存下输入到一半的 token。
  // useState(getApiToken)：getApiToken 作为惰性初始化器，挂载时读取 localStorage。
  const [apiTokenDraft, setApiTokenDraft] = useState(getApiToken)
  const [tokenSaved, setTokenSaved] = useState(() => getApiToken() !== '')

  const handleSaveToken = () => {
    const t = apiTokenDraft.trim()
    if (!t) {
      // 空值等价于清除：回到兼容模式（后端未启用鉴权时无感）
      clearApiToken()
      setTokenSaved(false)
      message.info('Token 已清除，请求不再附加 Authorization（兼容模式）')
      return
    }
    setApiToken(t)
    setTokenSaved(true)
    message.success('已保存：后续请求自动附加 Authorization: Bearer ***')
  }

  const handleClearToken = () => {
    clearApiToken()
    setApiTokenDraft('')
    setTokenSaved(false)
    message.info('Token 已清除（兼容模式）')
  }

  // 本地设置：用 onValuesChange 实时生效（减少"点保存但忘了"的坑）
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

  return (
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
                    {/* ★ 2026-09-28（外部审查 [P3]「Web 的 English 选项只切换组件库语言」）：
                        明确该偏好的**实际范围**——它只切换 **antd 组件库的 locale**，业务页面与导航
                        文案仍为中文。没有这句提示时，选 English 会得到一个中英混合界面且**无任何说明**。
                        （全站双语不在当前范围。`prefs.language` 的消费点只有两处：`App.tsx`
                        **按它切换 antd locale**；`HomePage.tsx` 的「应用偏好」卡片**原样显示它的值**。
                        ⇒ 除组件库外，全仓没有任何按它切换语言的渲染逻辑 —— 但「唯一消费点」的说法
                        不成立（验收代理指出），此处已按实际收窄。） */}
                    <Form.Item
                      label="语言"
                      name="language"
                      extra="English 目前仅切换组件库语言；业务文案仍为中文（全站双语不在当前范围）"
                    >
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

      <Card title="后端鉴权（API Token，独立存储）">
        <Space direction="vertical" size={12} style={{ width: '100%' }}>
          <Form layout="vertical">
            <Form.Item
              label="Bearer Token"
              extra="服务端设置 RK_WCFR_HTTP_AUTH_TOKEN 后，所有 /api 请求需携带该 token；留空 = 兼容模式（不附加 Authorization）"
            >
              <Input.Password
                value={apiTokenDraft}
                onChange={(e) => setApiTokenDraft(e.target.value)}
                placeholder="留空表示不启用鉴权"
                autoComplete="off"
              />
            </Form.Item>
            <Space>
              <Button type="primary" icon={<SaveOutlined />} onClick={handleSaveToken}>
                保存 Token
              </Button>
              <Button danger icon={<DeleteOutlined />} onClick={handleClearToken} disabled={!tokenSaved}>
                清除 Token
              </Button>
            </Space>
          </Form>
          {tokenSaved && (
            <Alert type="success" showIcon message="已启用 Bearer 鉴权，请求将自动携带 Authorization 头" />
          )}
        </Space>
      </Card>

      <Alert
        type="info"
        showIcon
        message="说明"
        description={'“应用设置”不写入后端 /api/v1/settings（后端 schema 禁止多余字段），仅用于控制前端行为。'}
      />
    </Space>
  )
}
