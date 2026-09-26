import { ReloadOutlined } from '@ant-design/icons'
import { Alert, Button, Card, Descriptions, Empty, Space, Spin, Table, Typography } from 'antd'
import type { TableColumnsType } from 'antd'
import { useEffect, useState } from 'react'

import { getFacesSnapshot } from '../api/faces'
import type { FaceDetection, FacesSnapshot } from '../api/types'
import { getErrorMessage } from '../api/error'
import { useAppStore } from '../state/AppStore'

type LoadState =
  | { status: 'loading' }
  | { status: 'ready'; data: FacesSnapshot }
  | { status: 'error'; message: string }

// 人脸检测结果表格：字段与后端 buildFacesJson 输出一一对应（帧坐标 / 预览坐标 / 置信度）。
const columns: TableColumnsType<FaceDetection> = [
  {
    title: '#',
    key: 'index',
    width: 56,
    render: (_v, _f, i) => i + 1,
  },
  {
    title: '帧坐标 (x, y, w, h)',
    key: 'bbox',
    render: (_v, f) => `${f.bbox.x}, ${f.bbox.y}, ${f.bbox.w}, ${f.bbox.h}`,
  },
  {
    title: '预览坐标 (x, y, w, h)',
    key: 'display_bbox',
    render: (_v, f) => `${f.display_bbox.x}, ${f.display_bbox.y}, ${f.display_bbox.w}, ${f.display_bbox.h}`,
  },
  {
    title: '置信度',
    key: 'confidence',
    width: 120,
    render: (_v, f) => (typeof f.confidence === 'number' ? f.confidence.toFixed(4) : '—'),
  },
]

function formatTimestamp(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '—'
  return new Date(ms).toLocaleString()
}

function formatNumber(v: number | null, digits = 2): string {
  return typeof v === 'number' && Number.isFinite(v) ? v.toFixed(digits) : '—'
}

// 人脸识别结果页：展示 GET /api/v1/faces 返回的**当前帧**人脸检测快照。
// ⚠️ 该端点不含 personId 等身份字段 —— 身份标签仅绘制在预览画面上（见 openapi.yaml）。
export function FacesPage() {
  const { prefs } = useAppStore()
  const [state, setState] = useState<LoadState>({ status: 'loading' })
  // 刷新/重试以自增计数器触发 effect 重跑（沿用 PreviewPage 的 camRetry 范式）。
  const [reloadKey, setReloadKey] = useState(0)

  // 说明：初始 state 即 'loading'，effect 内不做**同步** setState（只在 await 后的
  // .then/.catch 回调里 setState），避免触发 react-hooks/set-state-in-effect 级联渲染告警。
  useEffect(() => {
    let alive = true
    getFacesSnapshot(prefs)
      .then((snap) => {
        if (!alive) return
        setState({ status: 'ready', data: snap })
      })
      .catch((e: unknown) => {
        if (!alive) return
        setState({ status: 'error', message: getErrorMessage(e) || '加载人脸结果失败' })
      })
    return () => {
      alive = false
    }
  }, [prefs, reloadKey])

  // 手动刷新/重试：事件回调里同步置 loading 是安全的。
  const handleRefresh = () => {
    setState({ status: 'loading' })
    setReloadKey((v) => v + 1)
  }

  const data = state.status === 'ready' ? state.data : undefined

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card
        title="当前帧人脸快照"
        extra={
          <Button
            icon={<ReloadOutlined />}
            onClick={handleRefresh}
            loading={state.status === 'loading'}
          >
            刷新
          </Button>
        }
      >
        <Typography.Paragraph type="secondary">
          本页展示服务端当前帧检测到的人脸（非人脸库内容）。接口不返回身份字段，身份标签仅绘制在预览画面上。
        </Typography.Paragraph>

        {state.status === 'loading' ? (
          <div style={{ padding: '32px 0', textAlign: 'center' }}>
            <Spin />
          </div>
        ) : state.status === 'error' ? (
          <Alert
            type="error"
            showIcon
            message="人脸结果不可用"
            description={state.message}
            action={
              <Button size="small" icon={<ReloadOutlined />} onClick={handleRefresh}>
                重试
              </Button>
            }
          />
        ) : data ? (
          <Space direction="vertical" size={16} style={{ width: '100%' }}>
            <Descriptions column={2} size="small">
              <Descriptions.Item label="采集时间">{formatTimestamp(data.timestamp_ms)}</Descriptions.Item>
              <Descriptions.Item label="人脸数">{data.faces.length}</Descriptions.Item>
              <Descriptions.Item label="帧尺寸">
                {data.frame.w}×{data.frame.h}
              </Descriptions.Item>
              <Descriptions.Item label="预览尺寸">
                {data.preview.w}×{data.preview.h}（{data.preview.scale_mode}）
              </Descriptions.Item>
              <Descriptions.Item label="推理耗时(ms)">{formatNumber(data.perf.infer_ms)}</Descriptions.Item>
              <Descriptions.Item label="丢帧率">{formatNumber(data.perf.drop_rate, 3)}</Descriptions.Item>
              <Descriptions.Item label="步长(stride)">{data.perf.stride}</Descriptions.Item>
            </Descriptions>

            {data.faces.length === 0 ? (
              <Empty image={Empty.PRESENTED_IMAGE_SIMPLE} description="当前帧未检测到人脸" />
            ) : (
              <Table<FaceDetection>
                size="small"
                rowKey={(_f, i) => String(i)}
                columns={columns}
                dataSource={data.faces}
                pagination={false}
              />
            )}
          </Space>
        ) : null}
      </Card>
    </Space>
  )
}
