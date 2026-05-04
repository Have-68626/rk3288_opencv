import {
  Alert,
  Button,
  Card,
  Divider,
  Form,
  Input,
  Popconfirm,
  Select,
  Space,
  Switch,
  Tooltip,
  Typography,
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
  const [isEnrolling, setIsEnrolling] = useState(false)
  const [isClearing, setIsClearing] = useState(false)
  const [isOpeningPrivacy, setIsOpeningPrivacy] = useState(false)

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
      .catch((e: unknown) => {
        if (!alive) return
        setCams({ status: 'error', message: (e as Error)?.message || '加载摄像头列表失败' })
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
          <Form.Item
            htmlFor="preview-device-select"
            label={
              <Space>
                <span>摄像头设备</span>
                {cams.status === 'ready' && cams.devices.length === 0 ? (
                  <Tooltip title="未检测到设备。请检查摄像头是否连接、驱动是否正常，或本地服务是否已启动并拥有访问权限。">
                    <Typography.Text type="secondary" style={{ cursor: 'help' }}>
                      (帮助)
                    </Typography.Text>
                  </Tooltip>
                ) : null}
              </Space>
            }
          >
            <Select
              id="preview-device-select"
              loading={cams.status === 'loading'}
              placeholder={cams.status === 'loading' ? '正在加载摄像头...' : '请选择摄像头'}
              notFoundContent={
                cams.status === 'loading'
                  ? '加载中...'
                  : '未发现摄像头，请检查权限或本地服务'
              }
              options={deviceOptions}
              value={selectedDevice?.deviceId}
              onChange={(deviceId) => {
                updateServerSettings({ camera: { preferredDeviceId: deviceId } })
              }}
              disabled={cams.status !== 'ready'}
            />
          </Form.Item>

          <Form.Item label="分辨率 / FPS" htmlFor="preview-format-select">
            <Select
              id="preview-format-select"
              placeholder="请先选择摄像头"
              notFoundContent="该设备无可用分辨率"
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
                id="preview-flip-x"
                checked={flipX}
                aria-label="翻转 X"
                onChange={(v) => {
                  setFlipX(v)
                  setFlip(prefs, { flipX: v, flipY })
                }}
              />
              <label htmlFor="preview-flip-x" style={{ cursor: 'pointer' }}>
                <Typography.Text>翻转 X</Typography.Text>
              </label>
            </Space>
            <Space>
              <Switch
                id="preview-flip-y"
                checked={flipY}
                aria-label="翻转 Y"
                onChange={(v) => {
                  setFlipY(v)
                  setFlip(prefs, { flipX, flipY: v })
                }}
              />
              <label htmlFor="preview-flip-y" style={{ cursor: 'pointer' }}>
                <Typography.Text>翻转 Y</Typography.Text>
              </label>
            </Space>
          </Space>

          <Divider style={{ margin: '12px 0' }} />

          <Form.Item
            rules={[{ required: true }]}
            label="注册 personId"
            htmlFor="preview-person-id"
            extra="人脸特征将与此 ID 绑定。请确保上方预览画面中人脸清晰可见。"
          >
            <Input
              id="preview-person-id"
              value={personId}
              maxLength={32}
              onChange={(e) => setPersonId(e.target.value)}
              placeholder="例如：alice"
            />
          </Form.Item>

          <Space wrap>
            <Tooltip title={!personId.trim() ? '请输入要注册的 personId' : ''}>
              <span style={{ display: 'inline-block' }} tabIndex={!personId.trim() ? 0 : undefined} role={!personId.trim() ? 'button' : undefined} aria-disabled={!personId.trim() ? true : undefined}>
                <Button
                  type="primary"
                  onClick={async () => {
                    try {
                      setIsEnrolling(true)
                      await enroll(prefs, { personId })
                      message.success('注册指令已发送')
                      setPersonId('')
                    } catch (e: unknown) {
                      message.error((e as Error)?.message || '注册失败')
                    } finally {
                      setIsEnrolling(false)
                    }
                  }}
                  disabled={!personId.trim()}
                  loading={isEnrolling}
                  style={{ pointerEvents: !personId.trim() ? 'none' : undefined }}
                >
                  注册
                </Button>
              </span>
            </Tooltip>
            <Popconfirm
              title="警告：此操作不可恢复"
              description="确认清空所有人脸库数据？"
              onConfirm={async () => {
                try {
                  setIsClearing(true)
                  await clearDb(prefs)
                  message.success('清空指令已发送')
                } catch (e: unknown) {
                  message.error((e as Error)?.message || '清空失败')
                } finally {
                  setIsClearing(false)
                }
              }}
              okText="确认清空"
              cancelText="取消"
              okButtonProps={{ danger: true, loading: isClearing }}
            >
              <Button danger loading={isClearing}>
                清空库
              </Button>
            </Popconfirm>
            <Button
              onClick={async () => {
                try {
                  setIsOpeningPrivacy(true)
                  await openPrivacySettings(prefs)
                  message.success('已尝试打开隐私设置窗口')
                } catch (e: unknown) {
                  message.error((e as Error)?.message || '打开失败')
                } finally {
                  setIsOpeningPrivacy(false)
                }
              }}
              loading={isOpeningPrivacy}
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
