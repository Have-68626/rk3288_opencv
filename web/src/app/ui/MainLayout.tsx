import { AppstoreOutlined, ReloadOutlined, SettingOutlined, TeamOutlined, VideoCameraOutlined } from '@ant-design/icons'
import { Badge, Button, Layout, Menu, Space, Typography } from 'antd'
import type { MenuProps } from 'antd'
import { Outlet, useLocation, useNavigate } from 'react-router-dom'

import { useAppStore } from '../state/AppStore'
import { badgeFor } from './badge'
import { DegradedBanner } from './DegradedBanner'

const { Header, Sider, Content } = Layout

// 徽标判据已抽到 ./badge（**不依赖 antd**，便于纯函数单测）；见该文件的说明。

export function MainLayout() {
  const nav = useNavigate()
  const loc = useLocation()
  const { serverSettings, refreshServerSettings, healthProbe } = useAppStore()

  const selectedKey =
    loc.pathname.startsWith('/settings')
      ? 'settings'
      : loc.pathname.startsWith('/preview')
        ? 'preview'
        : loc.pathname.startsWith('/faces')
          ? 'faces'
      : loc.pathname.startsWith('/home')
        ? 'home'
        : 'home'

  // ★ 2026-09-28 修（外部审查 [P2]）：徽标此前**只**由 `serverSettings.status` 派生，而该状态
  //   只在「挂载时一次 + 用户手动刷新」时更新 ⇒ 后端断线后徽标会**无限期**保持「已连接」，
  //   与 `docs/windows-web-spa/feature_parity.md` 声明的「顶栏状态区 = 健康检查入口」不符。
  //   判据已抽到 `badgeFor()`（见其注释，含单测）。
  const badge = badgeFor(serverSettings.status, healthProbe)

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
            if (e.key === 'faces') nav('/faces')
            if (e.key === 'settings') nav('/settings')
          }}
          items={[
            { key: 'home', icon: <AppstoreOutlined />, label: '概览' },
            { key: 'preview', icon: <VideoCameraOutlined />, label: '预览' },
            { key: 'faces', icon: <TeamOutlined />, label: '人脸结果' },
            { key: 'settings', icon: <SettingOutlined />, label: '设置' },
          ]}
        />
      </Sider>

      <Layout>
        {/* 运行期降级横幅：仅在 health.status === 'degraded' 时渲染，否则不占位。 */}
        <DegradedBanner />
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
              icon={<ReloadOutlined />}
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

