import { Button, Result, Space, Spin, Typography } from 'antd'
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

    // 启动时先尝试拉取后端 settings：
    // - 好处：用户进入设置页时已有数据；
    // - 失败也不阻断：允许“离线/后端未启动”进入页面查看提示。
    refreshServerSettings({ silent: true }).then((ok) => {
      if (ok) nav(startPageToRoute(prefs.startPage), { replace: true })
      // 失败则停留在启动页，让用户决定“重试/继续”
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

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
                <Button type="primary" onClick={() => refreshServerSettings()}>
                  重试
                </Button>
                <Button onClick={() => nav(startPageToRoute(prefs.startPage), { replace: true })}>
                  继续进入
                </Button>
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

