// MJPEG 预览流：Bearer 认证 + multipart 帧切分。
// 背景：服务端已强制 preview.mjpeg 必须携带 Bearer 鉴权（S-SEC-B02），而 <img src>
// 无法携带 Authorization 头，因此有 token 时改为 fetch + ReadableStream 按边界切帧。
// 帧格式与 StreamSessionRunner.cpp runMjpeg() 输出对齐：
//   --rk_boundary\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<JPEG bytes>\r\n

export const MJPEG_BOUNDARY = 'rk_boundary'

// TS 5.7+ 泛型 TypedArray：subarray() 产出的 view 是 ArrayBufferLike 缓冲，
// 与 Blob 要求的 ArrayBuffer 视图不同源，统一用该别名表达"任意缓冲上的字节视图"。
export type Bytes = Uint8Array<ArrayBufferLike>

export interface MjpegStreamOptions {
  url: string
  token: string
  signal: AbortSignal
  /** 每收到一帧完整 JPEG 字节时回调（字节为独立快照，可安全转 Blob） */
  onFrame: (jpegBytes: Bytes) => void
  /** 流错误/断流时回调，恰好一次；前端据此触发重连（复用 previewKey 刷新机制） */
  onError: (err: unknown) => void
}

// 无 token（空/纯空白）时不发起任何请求，返回 null（兼容无鉴权部署的 <img src> 路径）；
// 有 token 时返回一个 Promise，流正常关闭时 resolve（onError 已在期间回调）。
export function fetchMjpegStream(opts: MjpegStreamOptions): Promise<void> | null {
  const token = opts.token.trim()
  if (!token) return null

  return (async () => {
    // 保证 onError 恰好一次：中止、网络错误、非 200、断流只通知一次
    let notified = false
    const notify = (err: unknown) => {
      if (notified) return
      notified = true
      opts.onError(err)
    }

    let res: Response
    try {
      res = await fetch(opts.url, {
        headers: { Authorization: `Bearer ${token}` },
        signal: opts.signal,
      })
    } catch (e) {
      notify(e)
      return
    }

    if (!res.ok || !res.body) {
      notify(new Error(`MJPEG 流请求失败：HTTP ${res.status}`))
      return
    }

    const reader = res.body.getReader()
    let buffer: Bytes = new Uint8Array(0)
    try {
      for (;;) {
        const { done, value } = await reader.read()
        if (done) break
        buffer = appendBytes(buffer, value)
        const { frames, rest } = splitMjpegFrames(buffer)
        buffer = rest
        for (const f of frames) opts.onFrame(f)
      }
    } catch (e) {
      // 网络中断/主动中止（AbortError）也走 onError；前端用 aborted 标志忽略中止
      notify(e)
      return
    }

    // 流被服务端正常关闭（Connection: close）视为断流，触发前端重连
    notify(new Error('MJPEG 流已断开'))
  })()
}

/**
 * 把一个累积的 multipart 字节缓冲切成完整 JPEG 帧。
 * 返回 { frames, rest }：frames 为已完整的帧；rest 为未消费尾部（半帧/半边界），
 * 由调用方累积到下一块继续切分。边界、头部、帧数据都可能跨块到达，因此以
 * Content-Length 提前量判定“帧数据是否到齐”，绝不臆造半帧。
 */
export function splitMjpegFrames(buffer: Bytes): {
  frames: Bytes[]
  rest: Bytes
} {
  const frames: Bytes[] = []
  let pos = 0
  for (;;) {
    // 找到当前帧的起始边界
    const start = findSubarray(buffer, kBoundary, pos)
    if (start < 0) break
    // 帧头结束（\r\n\r\n）：Content-Type/Content-Length 可能跨块，未完整则等待
    const headEnd = findSubarray(buffer, kCrLfCrLf, start + kBoundary.length)
    if (headEnd < 0) break
    const header = ascii(buffer.subarray(start + kBoundary.length, headEnd))
    const lenMatch = /Content-Length:\s*(\d+)/i.exec(header)
    const dataStart = headEnd + kCrLfCrLf.length
    let len: number
    if (lenMatch) {
      len = Number(lenMatch[1])
    } else {
      // 服务端未给 Content-Length：退化到“下一个边界之前”切帧
      const next = findSubarray(buffer, kBoundary, dataStart)
      if (next < 0) break
      len = next - dataStart
    }
    const dataEnd = dataStart + len
    if (dataEnd > buffer.length) break // Content-Length 提前量：帧数据未到齐
    frames.push(buffer.subarray(dataStart, dataEnd))
    pos = dataEnd
  }
  return { frames, rest: buffer.subarray(pos) }
}

const te = new TextEncoder()
const kBoundary = te.encode(`--${MJPEG_BOUNDARY}`)
const kCrLfCrLf = te.encode('\r\n\r\n')

function findSubarray(haystack: Bytes, needle: Uint8Array<ArrayBuffer>, from: number): number {
  if (needle.length === 0) return -1
  const last = haystack.length - needle.length
  outer: for (let i = from; i <= last; i++) {
    for (let j = 0; j < needle.length; j++) {
      if (haystack[i + j] !== needle[j]) continue outer
    }
    return i
  }
  return -1
}

function ascii(bytes: Bytes): string {
  let out = ''
  for (let i = 0; i < bytes.length; i++) out += String.fromCharCode(bytes[i])
  return out
}

function appendBytes(a: Bytes, b: Bytes): Bytes {
  if (a.length === 0) return b
  if (b.length === 0) return a
  const out = new Uint8Array(a.length + b.length)
  out.set(a, 0)
  out.set(b, a.length)
  return out
}
