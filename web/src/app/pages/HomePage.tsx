import { Alert, Card, Descriptions, Space, Typography } from 'antd'

import { useAppStore } from '../state/AppStore'

export function HomePage() {
  const { prefs, serverSettings } = useAppStore()

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card>
        <Typography.Title level={4} style={{ marginTop: 0 }}>
          概览
        </Typography.Title>
        <Typography.Paragraph style={{ marginBottom: 0 }}>
          本页面用于快速确认：前端偏好与后端 settings 是否可读写。
        </Typography.Paragraph>
      </Card>

      {serverSettings.status === 'error' ? (
        <Alert
          type="error"
          showIcon
          message="后端 /api/v1/settings 不可用"
          description={
            <>
              <div>{serverSettings.error.message}</div>
              {serverSettings.error.details?.length ? (
                <div style={{ marginTop: 8 }}>
                  <Typography.Text type="secondary">details：</Typography.Text>
                  <ul style={{ margin: '4px 0 0 18px' }}>
                    {serverSettings.error.details.map((d, i) => (
                      <li key={i}>{d}</li>
                    ))}
                  </ul>
                </div>
              ) : null}
            </>
          }
        />
      ) : null}

      <Card title="应用偏好（本地）">
        <Descriptions column={1} size="small">
          <Descriptions.Item label="主题">{prefs.theme}</Descriptions.Item>
          <Descriptions.Item label="语言">{prefs.language}</Descriptions.Item>
          <Descriptions.Item label="启动页">{prefs.startPage}</Descriptions.Item>
          <Descriptions.Item label="API 超时(ms)">{prefs.apiTimeoutMs}</Descriptions.Item>
          <Descriptions.Item label="日志级别">{prefs.logLevel}</Descriptions.Item>
          <Descriptions.Item label="缓存策略">{prefs.cacheStrategy}</Descriptions.Item>
        </Descriptions>
      </Card>

      <Card title="后端 settings（只读摘要）">
        {serverSettings.data ? (
          <Descriptions column={1} size="small">
            <Descriptions.Item label="schemaVersion">
              {serverSettings.data.schemaVersion}
            </Descriptions.Item>
            <Descriptions.Item label="HTTP 端口">
              {serverSettings.data.http?.port}
            </Descriptions.Item>
            <Descriptions.Item label="相机分辨率">
              {serverSettings.data.camera?.width}×{serverSettings.data.camera?.height} @
              {serverSettings.data.camera?.fps}fps
            </Descriptions.Item>
            <Descriptions.Item label="日志目录">
              {serverSettings.data.log?.logDir}
            </Descriptions.Item>
          </Descriptions>
        ) : (
          <Typography.Text type="secondary">
            尚未加载。可点击右上角“刷新后端设置”。
          </Typography.Text>
        )}
      </Card>
    </Space>
  )
}

