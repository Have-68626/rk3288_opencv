import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { fetchMjpegStream, splitMjpegFrames, MJPEG_BOUNDARY } from '../mjpeg'

// 单测环境为 node（vitest 默认）：ReadableStream/TextEncoder/AbortController 均为全局可用。
// MJPEG multipart 帧格式与 StreamSessionRunner.cpp runMjpeg() 输出对齐：
//   --rk_boundary\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<JPEG bytes>\r\n
const te = new TextEncoder()

function partHeader(jpeg: Uint8Array): Uint8Array {
  return te.encode(
    `--${MJPEG_BOUNDARY}\r\n` +
      'Content-Type: image/jpeg\r\n' +
      `Content-Length: ${jpeg.length}\r\n` +
      '\r\n',
  )
}

function cat(...chunks: Uint8Array[]): Uint8Array {
  const total = chunks.reduce((n, c) => n + c.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const c of chunks) {
    out.set(c, off)
    off += c.length
  }
  return out
}

const mockFetch = vi.fn()
beforeEach(() => {
  mockFetch.mockReset()
  ;(globalThis as unknown as { fetch: unknown }).fetch = mockFetch
})
afterEach(() => {
  delete (globalThis as unknown as { fetch?: unknown }).fetch
})

describe('fetchMjpegStream（Bearer 认证 MJPEG 流）', () => {
  it('请求带 Authorization: Bearer，并从 multipart 切出 2 帧 JPEG 字节', async () => {
    const jpeg1 = Uint8Array.of(0xff, 0xd8, 0x11, 0x22)
    const jpeg2 = Uint8Array.of(0xff, 0xd8, 0x33, 0x44, 0x55)
    const body = cat(
      partHeader(jpeg1), jpeg1, te.encode('\r\n'),
      partHeader(jpeg2), jpeg2, te.encode('\r\n'),
    )
    mockFetch.mockResolvedValue({
      ok: true,
      status: 200,
      body: new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(body)
          controller.close()
        },
      }),
    })

    const frames: Uint8Array[] = []
    await fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 'secret-token',
      signal: new AbortController().signal,
      onFrame: (f) => frames.push(f),
      onError: () => {},
    })

    expect(mockFetch).toHaveBeenCalledWith(
      '/api/v1/preview.mjpeg',
      expect.objectContaining({
        headers: { Authorization: 'Bearer secret-token' },
      }),
    )
    expect(frames).toHaveLength(2)
    expect(Array.from(frames[0])).toEqual([0xff, 0xd8, 0x11, 0x22])
    expect(Array.from(frames[1])).toEqual([0xff, 0xd8, 0x33, 0x44, 0x55])
  })

  it('流式：帧跨块到达仍能切出完整帧', async () => {
    const jpeg1 = Uint8Array.of(0xff, 0xd8, 0x11, 0x22)
    const jpeg2 = Uint8Array.of(0xff, 0xd8, 0x33, 0x44, 0x55)
    const p1 = cat(partHeader(jpeg1), jpeg1, te.encode('\r\n'))
    const p2 = cat(partHeader(jpeg2), jpeg2, te.encode('\r\n'))
    // 把 p1 的最后 3 字节（帧尾 + 下一帧头部开头）挪到第二个 chunk，模拟跨块
    const cut = p1.length - 3
    mockFetch.mockResolvedValue({
      ok: true,
      status: 200,
      body: new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(p1.subarray(0, cut))
          controller.enqueue(cat(p1.subarray(cut), p2))
          controller.close()
        },
      }),
    })

    const frames: Uint8Array[] = []
    await fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 't',
      signal: new AbortController().signal,
      onFrame: (f) => frames.push(f),
      onError: () => {},
    })

    expect(frames).toHaveLength(2)
    expect(Array.from(frames[0])).toEqual([0xff, 0xd8, 0x11, 0x22])
    expect(Array.from(frames[1])).toEqual([0xff, 0xd8, 0x33, 0x44, 0x55])
  })

  it('无 token（空串）：不发起 fetch，返回 null', async () => {
    const result = fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: '',
      signal: new AbortController().signal,
      onFrame: () => {},
      onError: () => {},
    })
    expect(result).toBeNull()
    expect(mockFetch).not.toHaveBeenCalled()
  })

  it('无 token（纯空白）：同样不发起 fetch', async () => {
    const result = fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: '   ',
      signal: new AbortController().signal,
      onFrame: () => {},
      onError: () => {},
    })
    expect(result).toBeNull()
    expect(mockFetch).not.toHaveBeenCalled()
  })

  it('HTTP 非 200（如 401）：onError 收到失败信息且无帧', async () => {
    mockFetch.mockResolvedValue({ ok: false, status: 401, body: null })
    const frames: Uint8Array[] = []
    const errors: unknown[] = []
    await fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 'secret-token',
      signal: new AbortController().signal,
      onFrame: (f) => frames.push(f),
      onError: (e) => errors.push(e),
    })
    expect(frames).toHaveLength(0)
    expect(errors).toHaveLength(1)
  })

  it('网络错误：onError 收到异常且无帧', async () => {
    mockFetch.mockRejectedValue(new Error('boom'))
    const frames: Uint8Array[] = []
    const errors: unknown[] = []
    await fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 'secret-token',
      signal: new AbortController().signal,
      onFrame: (f) => frames.push(f),
      onError: (e) => errors.push(e),
    })
    expect(frames).toHaveLength(0)
    expect(errors).toHaveLength(1)
    expect((errors[0] as Error).message).toBe('boom')
  })

  it('流正常关闭（服务端断流）：onError 恰好一次，触发前端重连路径', async () => {
    const jpeg = Uint8Array.of(0xaa, 0xbb)
    const body = cat(partHeader(jpeg), jpeg, te.encode('\r\n'))
    mockFetch.mockResolvedValue({
      ok: true,
      status: 200,
      body: new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(body)
          controller.close()
        },
      }),
    })

    const frames: Uint8Array[] = []
    const errors: unknown[] = []
    await fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 't',
      signal: new AbortController().signal,
      onFrame: (f) => frames.push(f),
      onError: (e) => errors.push(e),
    })

    expect(frames).toHaveLength(1)
    expect(Array.from(frames[0])).toEqual([0xaa, 0xbb])
    // 断流属于“需要重连”的事件，走 onError 恰好一次（不重复通知）
    expect(errors).toHaveLength(1)
  })
})

describe('splitMjpegFrames（纯函数：帧切分健壮性）', () => {
  it('Content-Length 提前量：帧数据未到齐时整体保留为 rest，补齐后成帧', () => {
    const jpeg = Uint8Array.of(0xff, 0xd8, 0xaa, 0xbb)
    const head = partHeader(jpeg) // 声明 Content-Length: 4
    const partial = cat(head, jpeg.subarray(0, 2)) // 只到 2/4 字节
    const r1 = splitMjpegFrames(partial)
    expect(r1.frames).toHaveLength(0)
    expect(r1.rest.length).toBe(partial.length)

    const complete = cat(partial, jpeg.subarray(2), te.encode('\r\n'))
    const r2 = splitMjpegFrames(complete)
    expect(r2.frames).toHaveLength(1)
    expect(Array.from(r2.frames[0])).toEqual([0xff, 0xd8, 0xaa, 0xbb])
    // 帧后的 \r\n 分隔符保留为 rest（等待下一帧边界到来时被消费）
    expect(Array.from(r2.rest)).toEqual([0x0d, 0x0a])
  })

  it('边界标记跨块：先返回 rest，补齐后成帧', () => {
    const jpeg = Uint8Array.of(1, 2, 3)
    const body = cat(partHeader(jpeg), jpeg, te.encode('\r\n'))
    // 切在 "--rk_b" 之后，让 boundary 前缀不完整
    const chunk1 = body.subarray(0, 5)
    const chunk2 = body.subarray(5)
    const r1 = splitMjpegFrames(chunk1)
    expect(r1.frames).toHaveLength(0)
    expect(r1.rest.length).toBe(5)

    const r2 = splitMjpegFrames(cat(r1.rest, chunk2))
    expect(r2.frames).toHaveLength(1)
    expect(Array.from(r2.frames[0])).toEqual([1, 2, 3])
  })

  it('头部跨块：Content-Type/Content-Length 被切到两个 chunk 仍可解析', () => {
    const jpeg = Uint8Array.of(7, 8, 9)
    const head = partHeader(jpeg)
    const body = cat(head, jpeg, te.encode('\r\n'))
    // 切在头部中间（Content-Type 未完成）
    const cut = 30
    const chunk1 = body.subarray(0, cut)
    const chunk2 = body.subarray(cut)
    const r1 = splitMjpegFrames(chunk1)
    expect(r1.frames).toHaveLength(0)

    const r2 = splitMjpegFrames(cat(r1.rest, chunk2))
    expect(r2.frames).toHaveLength(1)
    expect(Array.from(r2.frames[0])).toEqual([7, 8, 9])
  })

  it('空输入：无帧且 rest 为空', () => {
    const r = splitMjpegFrames(new Uint8Array(0))
    expect(r.frames).toHaveLength(0)
    expect(r.rest.length).toBe(0)
  })
})
