import {
  Alert,
  Card,
  Collapse,
  Form,
  InputNumber,
  Select,
  Space,
} from 'antd'
import { useEffect } from 'react'

import { useAppStore } from '../state/AppStore'

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
        description={'“应用设置”不写入后端 /api/v1/settings（后端 schema 禁止多余字段），仅用于控制前端行为。'}
      />
    </Space>
  )
}
