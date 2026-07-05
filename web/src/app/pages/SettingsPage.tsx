import { Tabs } from 'antd'

import { LocalSettingsTab } from './LocalSettingsTab'
import { ServerSettingsTab } from './ServerSettingsTab'

export function SettingsPage() {
  const items = [
    { key: 'local', label: '应用（本地）', children: <LocalSettingsTab /> },
    { key: 'server', label: '后端（API）', children: <ServerSettingsTab /> },
  ]

  return (
    <Tabs
      items={items}
      defaultActiveKey=”local”
      destroyInactiveTabPane={false}
    />
  )
}


