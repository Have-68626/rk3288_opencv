import { ReloadOutlined, WarningOutlined } from '@ant-design/icons'
import { Alert, Button, Space, Typography, message } from 'antd'
import { useEffect, useRef, useState } from 'react'

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
  const { prefs, setHealthProbe } = useAppStore()
  const [health, setHealth] = useState<HealthState>({})
  const [retrying, setRetrying] = useState(false)
  const aliveRef = useRef(true)
  // 生命周期代次使偏好变化前的响应无法覆盖新状态；不能按轮询计时递增，否则长超时永远被淘汰。
  const genRef = useRef(0)
  const retryRef = useRef(false)
  const refreshHealthRef = useRef<() => void>(() => {})

  useEffect(() => {
    aliveRef.current = true
    const gen = ++genRef.current
    const isCurrent = () => aliveRef.current && gen === genRef.current
    let timer: number | undefined
    let pending: Promise<void> | undefined
    let refreshQueued = false

    const loadHealth = () => {
      if (pending || !isCurrent()) return
      window.clearTimeout(timer)
      pending = (async () => {
        try {
          const env = await getHealth(prefs)
          if (!isCurrent() || refreshQueued) return
          setHealth({ data: env.ok ? env.data : undefined })
          setHealthProbe(!!env.ok)
        } catch {
          if (isCurrent() && !refreshQueued) {
            setHealth({})
            setHealthProbe(false)
          }
        } finally {
          pending = undefined
          if (isCurrent()) {
            // 重载前已发出的探针结果可能过时；合并手动刷新，并在旧探针结束后立即再取一次。
            if (refreshQueued) {
              refreshQueued = false
              loadHealth()
            } else {
              timer = window.setTimeout(loadHealth, HEALTH_POLL_MS)
            }
          }
        }
      })()
    }

    refreshHealthRef.current = () => {
      if (!isCurrent()) return
      if (pending) refreshQueued = true
      else loadHealth()
    }
    // 完成后再计时，避免 API 超时大于轮询周期时堆叠探针、永久丢弃超时结果。
    void loadHealth()
    return () => {
      aliveRef.current = false
      genRef.current = gen + 1
      window.clearTimeout(timer)
    }
  }, [prefs, setHealthProbe])

  const handleRetry = async () => {
    if (retryRef.current || !aliveRef.current) return
    retryRef.current = true
    const gen = genRef.current
    const isCurrent = () => aliveRef.current && gen === genRef.current
    setRetrying(true)
    try {
      // reload 端点以 id 作**存在性门槛**（id 不决定重载对象；校验通过后整体重建运行时），
      // 故取当前 activeModels 的首个 id 即可，无需新造端点。
      const models = await getModels(prefs)
      if (!isCurrent()) return
      if (!models.ok) throw new Error(models.error.message)
      const first = models.data.activeModels[0]
      if (!first) throw new Error('当前没有可重载的模型')
      const env = await reloadModel(prefs, first.id)
      if (!isCurrent()) return
      if (!env.ok) throw new Error(env.error.message)
      // data.ok 是**装配成败**（与 Envelope 根部的 ok 语义不同）：false = 重载后识别器仍未初始化。
      if (env.data.ok === false) {
        message.error(`重试后仍处于降级：${env.data.warning || '运行时装配失败'}`)
      } else {
        message.success('已请求重载运行时')
      }
    } catch (e: unknown) {
      if (isCurrent()) message.error(getErrorMessage(e) || '重试失败')
    } finally {
      retryRef.current = false
      if (aliveRef.current) setRetrying(false)
      // 无论成败都重取 health：成功则横幅消失，失败则刷新降级信息。
      if (isCurrent()) refreshHealthRef.current()
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
