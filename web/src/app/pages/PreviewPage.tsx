import { DeleteOutlined, ReloadOutlined, SafetyCertificateOutlined, UserAddOutlined } from '@ant-design/icons'
import {
  Alert,
  Button,
  Card,
  Divider,
  Empty,
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
import type { ServerSettingsDoc } from '../api/types'
import { clearDb, enroll, openPrivacySettings, setFlip } from '../api/actions'
import { getErrorMessage } from '../api/error'
import { useAppStore } from '../state/AppStore'

type LoadState =
  | { status: 'idle' }
  | { status: 'loading' }
  | { status: 'ready'; devices: CameraDeviceInfo[] }
  | { status: 'error'; message: string }

export function PreviewPage() {
  const { prefs, serverSettings, updateServerSettings } = useAppStore()
  const [cams, setCams] = useState<LoadState>({ status: 'idle' })
  const [camRetry, setCamRetry] = useState(0)
  const [previewKey, setPreviewKey] = useState(0)
  const [imgError, setImgError] = useState(false)

  const [flipX, setFlipX] = useState(false)
  const [flipY, setFlipY] = useState(false)
  const [isFlippingX, setIsFlippingX] = useState(false)
  const [isFlippingY, setIsFlippingY] = useState(false)
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
        setCams({ status: 'error', message: getErrorMessage(e) || '加载摄像头列表失败' })
      })
    return () => {
      alive = false
    }
  }, [prefs, camRetry])

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

  const handleEnroll = async () => {
    if (!personId.trim() || isEnrolling) return
    try {
      setIsEnrolling(true)
      await enroll(prefs, { personId })
      message.success('注册指令已发送')
      setPersonId('')
    } catch (e: unknown) {
      message.error(getErrorMessage(e) || '注册失败')
    } finally {
      setIsEnrolling(false)
    }
  }

  return (
    <Space direction="vertical" size={16} style={{ width: '100%' }}>
      <Card
        title="预览"
        extra={
          <Button
            icon={<ReloadOutlined />}
            onClick={() => {
              setImgError(false)
              setPreviewKey((v) => v + 1)
            }}
          >
            刷新预览
          </Button>
        }
      >
        <div
          style={{
            maxWidth: 1100,
            minHeight: 240,
            display: 'flex',
            flexDirection: 'column',
            justifyContent: 'center',
            alignItems: 'center',
            borderRadius: 8,
            border: '1px solid #f0f0f0',
            background: '#fafafa',
            overflow: 'hidden',
          }}
        >
          {imgError ? (
            <Empty
              image={Empty.PRESENTED_IMAGE_SIMPLE}
              description={
                <Typography.Text type="secondary">
                  无法连接到摄像头流，请检查服务状态
                </Typography.Text>
              }
              style={{ margin: '40px 0' }}
            >
              <Button
                type="primary"
                onClick={() => {
                  setImgError(false)
                  setPreviewKey((v) => v + 1)
                }}
              >
                重试
              </Button>
            </Empty>
          ) : (
            <img
              key={previewKey}
              src={`${import.meta.env.VITE_API_BASE || ''}/api/v1/preview.mjpeg`}
              style={{
                width: '100%',
                display: 'block',
              }}
              alt="摄像头实时预览"
              onError={() => setImgError(true)}
            />
          )}
        </div>
        <Typography.Paragraph type="secondary" style={{ marginBottom: 0, marginTop: 8 }}>
          若看不到画面：确认本地服务已启动，并检查摄像头权限是否允许。
        </Typography.Paragraph>
      </Card>

      <Card title="相机与动作">
        {cams.status === 'error' ? (
          <Alert
            type="error"
            showIcon
            message="摄像头列表不可用"
            description={cams.message}
            action={
              <Button size="small" icon={<ReloadOutlined />} onClick={() => setCamRetry((v) => v + 1)}>
                重试
              </Button>
            }
          />
        ) : null}

        <Form layout="vertical">
          <Form.Item
            htmlFor="preview-device-select"
            label={
              <Space>
                <span>摄像头设备</span>
                <Tooltip title="重新扫描设备">
                  <span
                    style={{ display: 'inline-block' }}
                    tabIndex={cams.status === 'loading' ? 0 : undefined}
                    role={cams.status === 'loading' ? 'button' : undefined}
                    aria-disabled={cams.status === 'loading' ? true : undefined}
                    aria-label={cams.status === 'loading' ? '正在重新扫描设备' : undefined}
                  >
                    <Button
                      type="text"
                      size="small"
                      icon={<ReloadOutlined />}
                      aria-label="重新扫描摄像头列表"
                      onClick={(e) => {
                        e.stopPropagation();
                        setCamRetry((v) => v + 1);
                      }}
                      loading={cams.status === 'loading'}
                      style={cams.status === 'loading' ? { pointerEvents: 'none' } : undefined}
                    />
                  </span>
                </Tooltip>
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
              onChange={async (deviceId) => {
                const hide = message.loading('正在切换摄像头...', 0)
                try {
                  await updateServerSettings({ camera: { preferredDeviceId: deviceId } } as Partial<ServerSettingsDoc>)
                  message.success('切换摄像头成功')
                } catch (e: unknown) {
                  message.error(getErrorMessage(e) || '切换摄像头失败')
                } finally {
                  hide()
                }
              }}
              disabled={cams.status !== 'ready' || serverSettings.status === 'loading'}
            />
          </Form.Item>

          <Form.Item label="分辨率 / FPS" htmlFor="preview-format-select">
            <Select
              id="preview-format-select"
              placeholder="请先选择摄像头"
              notFoundContent="该设备无可用分辨率"
              options={formatOptions}
              value={currentFormatKey}
              onChange={async (k) => {
                const m = /^(\d+)x(\d+)@(\d+)$/.exec(k)
                if (!m) return
                const hide = message.loading('正在更改分辨率...', 0)
                try {
                  await updateServerSettings({
                    camera: {
                      width: Number(m[1]),
                      height: Number(m[2]),
                      fps: Number(m[3]),
                    },
                  } as Partial<ServerSettingsDoc>)
                  message.success('更改分辨率成功')
                } catch (e: unknown) {
                  message.error(getErrorMessage(e) || '更改分辨率失败')
                } finally {
                  hide()
                }
              }}
              disabled={!selectedDevice || serverSettings.status === 'loading'}
            />
          </Form.Item>

          <Divider style={{ margin: '12px 0' }} />

          <Space wrap>
            <Space>
              <Switch
                id="preview-flip-x"
                checked={flipX}
                loading={isFlippingX}
                disabled={isFlippingY}
                aria-label="翻转 X"
                onChange={async (v) => {
                  const original = flipX
                  setFlipX(v)
                  setIsFlippingX(true)
                  try {
                    await setFlip(prefs, { flipX: v, flipY })
                    message.success(`画面已${v ? '开启' : '关闭'} X 轴翻转`)
                  } catch (e: unknown) {
                    setFlipX(original)
                    message.error(getErrorMessage(e) || '设置 X 轴翻转失败')
                  } finally {
                    setIsFlippingX(false)
                  }
                }}
              />
              <label
                htmlFor="preview-flip-x"
                style={{ cursor: isFlippingY ? 'not-allowed' : 'pointer' }}
                onClick={(e) => {
                  e.preventDefault();
                  if (!isFlippingY) {
                    document.getElementById('preview-flip-x')?.click();
                  }
                }}
              >
                <Typography.Text disabled={isFlippingY}>翻转 X</Typography.Text>
              </label>
            </Space>
            <Space>
              <Switch
                id="preview-flip-y"
                checked={flipY}
                loading={isFlippingY}
                disabled={isFlippingX}
                aria-label="翻转 Y"
                onChange={async (v) => {
                  const original = flipY
                  setFlipY(v)
                  setIsFlippingY(true)
                  try {
                    await setFlip(prefs, { flipX, flipY: v })
                    message.success(`画面已${v ? '开启' : '关闭'} Y 轴翻转`)
                  } catch (e: unknown) {
                    setFlipY(original)
                    message.error(getErrorMessage(e) || '设置 Y 轴翻转失败')
                  } finally {
                    setIsFlippingY(false)
                  }
                }}
              />
              <label
                htmlFor="preview-flip-y"
                style={{ cursor: isFlippingX ? 'not-allowed' : 'pointer' }}
                onClick={(e) => {
                  e.preventDefault();
                  if (!isFlippingX) {
                    document.getElementById('preview-flip-y')?.click();
                  }
                }}
              >
                <Typography.Text disabled={isFlippingX}>翻转 Y</Typography.Text>
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
              showCount
              allowClear
              onChange={(e) => setPersonId(e.target.value)}
              onPressEnter={handleEnroll}
              placeholder="例如：alice"
            />
          </Form.Item>

          <Space wrap>
            <Tooltip title={!personId.trim() ? '请输入要注册的 personId' : ''}>
              <span style={{ display: 'inline-block' }} tabIndex={!personId.trim() ? 0 : undefined} role={!personId.trim() ? 'button' : undefined} aria-disabled={!personId.trim() ? true : undefined} aria-label="注册">
                <Button
                  type="primary"
                  icon={<UserAddOutlined />}
                  onClick={handleEnroll}
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
                  message.error(getErrorMessage(e) || '清空失败')
                } finally {
                  setIsClearing(false)
                }
              }}
              okText="确认清空"
              cancelText="取消"
              okButtonProps={{ danger: true, loading: isClearing }}
            >
              <Button danger icon={<DeleteOutlined />} loading={isClearing}>
                清空库
              </Button>
            </Popconfirm>
            <Button
              icon={<SafetyCertificateOutlined />}
              onClick={async () => {
                try {
                  setIsOpeningPrivacy(true)
                  await openPrivacySettings(prefs)
                  message.success('已尝试打开隐私设置窗口')
                } catch (e: unknown) {
                  message.error(getErrorMessage(e) || '打开失败')
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
