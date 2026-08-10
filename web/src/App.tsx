import { lazy, Suspense } from 'react'
import { ConfigProvider, message, Spin, theme } from 'antd'
import enUS from 'antd/locale/en_US'
import zhCN from 'antd/locale/zh_CN'
import { HashRouter, Navigate, Route, Routes } from 'react-router-dom'

import { SplashPage } from './app/pages/SplashPage'
import { AppStoreProvider, setNotify, useAppStore } from './app/state/AppStore'
import { ErrorBoundary } from './app/ui/ErrorBoundary'
import { MainLayout } from './app/ui/MainLayout'

// 路由懒加载（Q-CQ-B08-011 拆包）：
// 非首屏页面按路由拆成独立 chunk，主 chunk 控制在批准预算 600KB raw 内。
// 页面文件为 named export，经 then 映射为 default 以满足 React.lazy 要求。
// SplashPage 保持静态导入：首屏立即渲染，不享受懒加载收益。
const HomePage = lazy(() =>
  import('./app/pages/HomePage').then((m) => ({ default: m.HomePage })),
)
const PreviewPage = lazy(() =>
  import('./app/pages/PreviewPage').then((m) => ({ default: m.PreviewPage })),
)
const SettingsPage = lazy(() =>
  import('./app/pages/SettingsPage').then((m) => ({ default: m.SettingsPage })),
)

// 懒加载 fallback：全屏居中 antd Spin（避免白屏闪烁）
function RouteFallback() {
  return (
    <div
      style={{
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        height: '100vh',
      }}
    >
      <Spin size="large" />
    </div>
  )
}

function AntdAndRouteRoot() {
  return (
    <ErrorBoundary>
      <AntdAndRouteInner />
    </ErrorBoundary>
  )
}

function AntdAndRouteInner() {
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
        <Suspense fallback={<RouteFallback />}>
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
        </Suspense>
      </HashRouter>
    </ConfigProvider>
  )
}

export default function App() {
  setNotify({
    loading: (msg: string) => message.loading(msg, 0),
    success: (msg: string) => message.success(msg),
    error: (msg: string) => message.error(msg),
  })
  return (
    <AppStoreProvider>
      <AntdAndRouteRoot />
    </AppStoreProvider>
  )
}
