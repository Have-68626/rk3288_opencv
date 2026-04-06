import {
  Alert,
  Button,
  Card,
  Divider,
  Form,
  Input,
  Select,
  Space,
  Switch,
  Typography,
  Popconfirm,
  message,
} from 'antd'
import { useEffect, useMemo, useState } from 'react'

import type { CameraDeviceInfo } from '../api/cameras'
import { getCameras } from '../api/cameras'
import { clearDb, enroll, openPrivacySettings, setFlip } from '../api/actions'
import { useAppStore } from '../state/AppStore'

type LoadState =
  | { status: 'idle' }
  | { status: 'loading' }
  | { status: 'ready'; devices: CameraDeviceInfo[] }
  | { status: 'error'; message: string }

export function PreviewPage() {
  const { prefs, serverSettings, updateServerSettings } = useAppStore()
  const [cams, setCams] = useState<LoadState>({ status: 'idle' })
  const [previewKey, setPreviewKey] = useState(0)
  const [flipX, setFlipX] = useState(false)
  const [flipY, setFlipY] = useState(false)
  const [personId, setPersonId] = useState('')

  const currentDeviceId = serverSettings.data?.camera?.preferredDeviceId ?? ''
  const currentW = serverSettings.data?.camera?.width ?? 640
  const currentH = serverSettings.data?.camera?.height ?? 480
  const currentFps = serverSettings.data?.camera?.fps ?? 30

  useEffect(() => {
    let alive = true
    setCams({ status: 'loading' })
    getCameras(prefs)
      .then((env) => {
        if (!alive) return
        if (!env.ok) {
          setCams({ status: 'error', message: env.error.message })
          return
        }
        setCams({ status: 'ready', devices: env.data.devices })
      })
      .catch((e: any) => {
        if (!alive) return
        setCams({ status: 'error', message: e?.message || '加载摄像头列表失败' })
      })
    return () => {
      alive = false
    }
  }, [prefs])

  const deviceOptions = useMemo(() => {
    if (cams.status !== 'ready') return []
    return cams.devices.map((d) => ({
      value: d.deviceId,
      label: d.name || d.deviceId || `device-${d.index}`,
    }))
  }, [cams])

  const selectedDevice = useMemo(() => {
    if (cams.status !== 'ready') return undefined
    return cams.devices.find((d) => d.deviceId === currentDeviceId) ?? cams.devices[0]
  }, [cams, currentDeviceId])

  const formatOptions = useMemo(() => {
    const dev = selectedDevice
    if (!dev) return []
    return dev.formats.map((f) => ({
      value: `${f.w}x${f.h}@${f.fps}`,
      label: `${f.w}×${f.h} @ ${f.fps}fps`,
    }))
  }, [selectedDevice])

  const currentFormatKey = `${currentW}x${currentH}@${currentFps}`

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card
        title="预览"
        extra={
          <Button onClick={() => setPreviewKey((v) => v + 1)}>刷新预览</Button>
        }
      >
        <div style={{ maxWidth: 1100 }}>
          <img
            key={previewKey}
            src="/api/v1/preview.mjpeg"
            style={{ width: '100%', borderRadius: 8, border: '1px solid #f0f0f0' }}
            alt="preview"
          />
        </div>
        <Typography.Paragraph type="secondary" style={{ marginBottom: 0, marginTop: 8 }}>
          若看不到画面：确认本地服务已启动，并检查摄像头权限是否允许。
        </Typography.Paragraph>
      </Card>

      <Card title="相机与动作">
        {cams.status === 'error' ? (
          <Alert type="error" showIcon message="摄像头列表不可用" description={cams.message} />
        ) : null}

        <Form layout="vertical">
          <Form.Item label="摄像头设备">
            <Select
              options={deviceOptions}
              value={selectedDevice?.deviceId}
              onChange={(deviceId) => {
                updateServerSettings({ camera: { preferredDeviceId: deviceId } })
              }}
              disabled={cams.status !== 'ready'}
            />
          </Form.Item>

          <Form.Item label="分辨率 / FPS">
            <Select
              options={formatOptions}
              value={currentFormatKey}
              onChange={(k) => {
                const m = /^(\d+)x(\d+)@(\d+)$/.exec(k)
                if (!m) return
                updateServerSettings({
                  camera: {
                    width: Number(m[1]),
                    height: Number(m[2]),
                    fps: Number(m[3]),
                  },
                })
              }}
              disabled={!selectedDevice}
            />
          </Form.Item>

          <Divider style={{ margin: '12px 0' }} />

          <Space wrap>
            <Space>
              <Switch
                checked={flipX}
                onChange={(v) => {
                  setFlipX(v)
                  setFlip(prefs, { flipX: v, flipY })
                }}
              />
              <Typography.Text>翻转 X</Typography.Text>
            </Space>
            <Space>
              <Switch
                checked={flipY}
                onChange={(v) => {
                  setFlipY(v)
                  setFlip(prefs, { flipX, flipY: v })
                }}
              />
              <Typography.Text>翻转 Y</Typography.Text>
            </Space>
          </Space>

          <Divider style={{ margin: '12px 0' }} />

          <Form.Item label="注册 personId">
            <Input
              value={personId}
              onChange={(e) => setPersonId(e.target.value)}
              placeholder="例如：alice"
            />
          </Form.Item>

          <Space wrap>
            <Button
              type="primary"
              onClick={async () => {
                try {
                  await enroll(prefs, { personId })
                  message.success('注册指令已发送')
                  setPersonId('')
                } catch (e: any) {
                  message.error(e?.message || '注册失败')
                }
              }}
              disabled={!personId.trim()}
            >
              注册
            </Button>
            <Popconfirm
              title="警告：此操作不可恢复"
              description="确认清空所有人脸库数据？"
              onConfirm={async () => {
                try {
                  await clearDb(prefs)
                  message.success('清空指令已发送')
                } catch (e: any) {
                  message.error(e?.message || '清空失败')
                }
              }}
              okText="确认清空"
              cancelText="取消"
              okButtonProps={{ danger: true }}
            >
              <Button danger>
                清空库
              </Button>
            </Popconfirm>
            <Button
              onClick={async () => {
                try {
                  await openPrivacySettings(prefs)
                  message.success('已尝试打开隐私设置窗口')
                } catch (e: any) {
                  message.error(e?.message || '打开失败')
                }
              }}
            >
              打开隐私设置
            </Button>
          </Space>
        </Form>

        {serverSettings.status === 'error' ? (
          <Alert
            style={{ marginTop: 12 }}
            type="error"
            showIcon
            message="后端 settings 保存/加载异常"
            description={serverSettings.error.message}
          />
        ) : null}
      </Card>
    </Space>
  )
}

