import { AppstoreOutlined, SettingOutlined, VideoCameraOutlined } from '@ant-design/icons'
import { Badge, Button, Layout, Menu, Space, Typography } from 'antd'
import type { MenuProps } from 'antd'
import { Outlet, useLocation, useNavigate } from 'react-router-dom'

import { useAppStore } from '../state/AppStore'

const { Header, Sider, Content } = Layout

function statusToBadge(status: 'idle' | 'loading' | 'ready' | 'error') {
  if (status === 'ready') return { status: 'success' as const, text: '已连接' }
  if (status === 'loading') return { status: 'processing' as const, text: '请求中' }
  if (status === 'error') return { status: 'error' as const, text: '异常' }
  return { status: 'default' as const, text: '未加载' }
}

export function MainLayout() {
  const nav = useNavigate()
  const loc = useLocation()
  const { serverSettings, refreshServerSettings } = useAppStore()

  const selectedKey =
    loc.pathname.startsWith('/settings')
      ? 'settings'
      : loc.pathname.startsWith('/preview')
        ? 'preview'
      : loc.pathname.startsWith('/home')
        ? 'home'
        : 'home'

  const badge = statusToBadge(serverSettings.status)

  return (
    <Layout style={{ minHeight: '100vh' }}>
      <Sider collapsible width={220} theme="dark">
        <div style={{ padding: 16 }}>
          <Typography.Title level={5} style={{ margin: 0, color: 'rgba(255,255,255,0.88)' }}>
            RK3288 控制台
          </Typography.Title>
          <Typography.Text style={{ color: 'rgba(255,255,255,0.65)' }}>
            本地 SPA（v1）
          </Typography.Text>
        </div>
        <Menu
          theme="dark"
          mode="inline"
          selectedKeys={[selectedKey]}
          onClick={(e: Parameters<NonNullable<MenuProps['onClick']>>[0]) => {
            if (e.key === 'home') nav('/home')
            if (e.key === 'preview') nav('/preview')
            if (e.key === 'settings') nav('/settings')
          }}
          items={[
            { key: 'home', icon: <AppstoreOutlined />, label: '概览' },
            { key: 'preview', icon: <VideoCameraOutlined />, label: '预览' },
            { key: 'settings', icon: <SettingOutlined />, label: '设置' },
          ]}
        />
      </Sider>

      <Layout>
        <Header style={{ background: 'transparent', padding: '0 16px' }}>
          <Space style={{ width: '100%', justifyContent: 'space-between' }}>
            <Space>
              <Badge status={badge.status} text={badge.text} />
              {serverSettings.status === 'error' ? (
                <Typography.Text type="danger">
                  {serverSettings.error.message}
                </Typography.Text>
              ) : null}
            </Space>
            <Button
              onClick={() => refreshServerSettings()}
              loading={serverSettings.status === 'loading'}
            >
              刷新后端设置
            </Button>
          </Space>
        </Header>
        <Content style={{ padding: 16 }}>
          <Outlet />
        </Content>
      </Layout>
    </Layout>
  )
}

