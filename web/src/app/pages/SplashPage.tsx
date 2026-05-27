import { ArrowRightOutlined, ReloadOutlined } from '@ant-design/icons'
import { Button, Result, Space, Spin, Tooltip, Typography } from 'antd'
import { useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'

import { useAppStore } from '../state/AppStore'

function startPageToRoute(startPage: 'home' | 'preview' | 'settings') {
  if (startPage === 'settings') return '/settings'
  if (startPage === 'preview') return '/preview'
  return '/home'
}

export function SplashPage() {
  const nav = useNavigate()
  const { prefs, serverSettings, refreshServerSettings } = useAppStore()
  const did = useRef(false)

  useEffect(() => {
    if (did.current) return
    did.current = true

    refreshServerSettings({ silent: true }).then((ok) => {
      if (ok) nav(startPageToRoute(prefs.startPage), { replace: true })
    })
  }, [refreshServerSettings, nav, prefs.startPage])

  return (
    <div
      style={{
        height: '100vh',
        display: 'grid',
        placeItems: 'center',
        padding: 16,
        background:
          'radial-gradient(1200px 500px at 40% 0%, rgba(24,144,255,0.25), transparent 60%), #0b1220',
      }}
    >
      <div style={{ width: 720, maxWidth: '100%' }}>
        <Typography.Title style={{ color: 'rgba(255,255,255,0.92)', marginTop: 0 }}>
          RK3288 本地控制台
        </Typography.Title>
        <Typography.Paragraph style={{ color: 'rgba(255,255,255,0.72)' }}>
          正在初始化 UI 与后端设置…
        </Typography.Paragraph>

        {serverSettings.status === 'error' ? (
          <Result
            status="warning"
            title="后端设置拉取失败"
            subTitle={serverSettings.error.message}
            extra={
              <Space>
                <Button
                  type="primary"
                  icon={<ReloadOutlined />}
                  onClick={() => refreshServerSettings()}
                  loading={(serverSettings.status as string) === 'loading'}
                >
                  重试
                </Button>
                <Tooltip title="在未连接后端的情况下进入，部分功能将受限或处于只读状态">
                  <Button icon={<ArrowRightOutlined />} onClick={() => nav(startPageToRoute(prefs.startPage), { replace: true })}>
                    继续进入 (离线模式)
                  </Button>
                </Tooltip>
              </Space>
            }
          />
        ) : (
          <div style={{ padding: '18px 0' }}>
            <Space>
              <Spin />
              <Typography.Text style={{ color: 'rgba(255,255,255,0.72)' }}>
                连接状态：{serverSettings.status}
              </Typography.Text>
            </Space>
          </div>
        )}
      </div>
    </div>
  )
}

