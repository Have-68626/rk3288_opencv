import { ReloadOutlined, WarningOutlined } from '@ant-design/icons'
import { Alert, Button, Space, Typography, message } from 'antd'
import { useCallback, useEffect, useRef, useState } from 'react'

import { describeReasonCode, getHealth } from '../api/health'
import { getModels, reloadModel } from '../api/models'
import { getErrorMessage } from '../api/error'
import type { HealthData } from '../api/types'
import { useAppStore } from '../state/AppStore'

// health 轮询周期：只读轻量端点，降级状态仅在装配/重载后变化，15s 足以贴合而不过度轮询。
const HEALTH_POLL_MS = 15000

// 未取到（含后端不可达/超时）时按“未知”处理 —— 不渲染任何东西（安静降级，不刷屏报错）。
type HealthState = { data?: HealthData }

// 运行期降级横幅（FACE-ARCWIN-003 前端展示）：
// 当 health 的**运行时装配**状态为 degraded（识别器整体初始化失败）时，在布局顶部显示
// 一条警告，说明“引擎处于降级运行模式：模型未就绪，识别不可用”，并给出可读的原因与
// 重试入口（复用既有 /api/v1/models/reload 端点）。未降级时不渲染、不占位。
//
// 注意：只对 `status === 'degraded'`（运行时装配）报警 —— `configStatus` 的 degraded/rejected
// 属**配置装载层**的另一个量，语义不同，且 config.json 的启动期告警可能长期存在，若一并
// 在此报警会误导成“识别不可用”。
export function DegradedBanner() {
  const { prefs } = useAppStore()
  const [health, setHealth] = useState<HealthState>({})
  const [retrying, setRetrying] = useState(false)
  const aliveRef = useRef(true)

  const loadHealth = useCallback(async () => {
    try {
      const env = await getHealth(prefs)
      if (!aliveRef.current) return
      setHealth({ data: env.ok ? env.data : undefined })
    } catch {
      // 后端不可达/超时：安静降级 —— 不渲染、不弹通知（避免轮询刷屏）
      if (aliveRef.current) setHealth({})
    }
  }, [prefs])

  useEffect(() => {
    aliveRef.current = true
    void loadHealth()
    const timer = window.setInterval(() => {
      void loadHealth()
    }, HEALTH_POLL_MS)
    return () => {
      aliveRef.current = false
      window.clearInterval(timer)
    }
  }, [loadHealth])

  const handleRetry = async () => {
    setRetrying(true)
    try {
      // reload 端点以 id 作**存在性门槛**（id 不决定重载对象；校验通过后整体重建运行时），
      // 故取当前 activeModels 的首个 id 即可，无需新造端点。
      const models = await getModels(prefs)
      if (!models.ok) throw new Error(models.error.message)
      const first = models.data.activeModels[0]
      if (!first) throw new Error('当前没有可重载的模型')
      const env = await reloadModel(prefs, first.id)
      if (!env.ok) throw new Error(env.error.message)
      // data.ok 是**装配成败**（与 Envelope 根部的 ok 语义不同）：false = 重载后识别器仍未初始化。
      if (env.data.ok === false) {
        message.error(`重试后仍处于降级：${env.data.warning || '运行时装配失败'}`)
      } else {
        message.success('已请求重载运行时')
      }
    } catch (e: unknown) {
      message.error(getErrorMessage(e) || '重试失败')
    } finally {
      setRetrying(false)
      // 无论成败都重取 health：成功则横幅消失，失败则刷新降级信息。
      void loadHealth()
    }
  }

  const data = health.data
  if (!data || data.status !== 'degraded') return null

  const reason = describeReasonCode(data.reasonCode)

  return (
    <Alert
      banner
      type="warning"
      showIcon
      icon={<WarningOutlined />}
      style={{ borderRadius: 0 }}
      message="引擎处于降级运行模式：模型未就绪，识别不可用"
      description={
        <Space direction="vertical" size={2}>
          {reason ? (
            <Typography.Text>
              降级原因：{reason}
              {data.reasonCode ? `（${data.reasonCode}）` : ''}
            </Typography.Text>
          ) : null}
          {data.warning ? (
            <Typography.Text type="secondary">详情：{data.warning}</Typography.Text>
          ) : null}
          {data.degradedSince ? (
            <Typography.Text type="secondary">自 {data.degradedSince} 起处于降级</Typography.Text>
          ) : null}
        </Space>
      }
      action={
        <Button
          size="small"
          danger
          icon={<ReloadOutlined />}
          loading={retrying}
          disabled={retrying}
          onClick={handleRetry}
        >
          重试
        </Button>
      }
    />
  )
}
