import { ConfigProvider, theme } from 'antd'
import enUS from 'antd/locale/en_US'
import zhCN from 'antd/locale/zh_CN'
import { HashRouter, Navigate, Route, Routes } from 'react-router-dom'

import { HomePage } from './app/pages/HomePage'
import { PreviewPage } from './app/pages/PreviewPage'
import { SettingsPage } from './app/pages/SettingsPage'
import { SplashPage } from './app/pages/SplashPage'
import { AppStoreProvider, useAppStore } from './app/state/AppStore'
import { MainLayout } from './app/ui/MainLayout'

function AntdAndRouteRoot() {
  const { prefs } = useAppStore()

  const resolvedThemeMode =
    prefs.theme === 'system'
      ? window.matchMedia?.('(prefers-color-scheme: dark)')?.matches
        ? 'dark'
        : 'light'
      : prefs.theme

  return (
    <ConfigProvider
      locale={prefs.language === 'en-US' ? enUS : zhCN}
      theme={{
        algorithm:
          resolvedThemeMode === 'dark'
            ? theme.darkAlgorithm
            : theme.defaultAlgorithm,
      }}
    >
      <HashRouter>
        <Routes>
          {/* 启动页：负责加载偏好/拉取后端 settings，并按“启动页”跳转 */}
          <Route path="/" element={<SplashPage />} />

          {/* 主框架：侧边栏/顶部栏/内容区 */}
          <Route element={<MainLayout />}>
            <Route path="/home" element={<HomePage />} />
            <Route path="/preview" element={<PreviewPage />} />
            <Route path="/settings" element={<SettingsPage />} />
          </Route>

          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </HashRouter>
    </ConfigProvider>
  )
}

export default function App() {
  return (
    <AppStoreProvider>
      <AntdAndRouteRoot />
    </AppStoreProvider>
  )
}
