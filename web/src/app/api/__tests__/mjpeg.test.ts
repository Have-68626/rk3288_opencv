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

  it('畸形流缓冲超限：onError 通知超限错误并重置缓冲，rest 不再无界累积', async () => {
    // 边界命中 + Content-Length 声明 100000 字节但数据永不补齐 → split 每块都整段保留 rest、从头重扫
    const head = te.encode(
      `--${MJPEG_BOUNDARY}\r\nContent-Type: image/jpeg\r\nContent-Length: 100000\r\n\r\n`,
    )
    const filler = Uint8Array.of(1, 2, 3)
    mockFetch.mockResolvedValue({
      ok: true,
      status: 200,
      body: new ReadableStream<Uint8Array>({
        start(controller) {
          controller.enqueue(cat(head, filler, filler, filler, filler, filler, filler))
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
      maxBufferBytes: 16, // 测试用小上限：超过即视为畸形流（生产默认 8MB）
      onFrame: (f) => frames.push(f),
      onError: (e) => errors.push(e),
    })

    expect(frames).toHaveLength(0)
    expect(errors).toHaveLength(1)
    expect((errors[0] as Error).message).toMatch(/超限/)
  })

  it('abort 后不触发 onError：AbortError 属调用方主动行为，不当作断流故障上报', async () => {
    const jpeg = Uint8Array.of(0xff, 0xd8, 0x11)
    const body = cat(partHeader(jpeg), jpeg, te.encode('\r\n'))
    const controller = new AbortController()
    let bodyCtrl: ReadableStreamDefaultController<Uint8Array> | undefined
    const stream = new ReadableStream<Uint8Array>({
      start(c) {
        bodyCtrl = c
        c.enqueue(body)
      },
    })
    // 模拟真实 fetch 中止语义：signal abort 时 body 流以 AbortError 终止，挂起的 read() 被拒绝
    controller.signal.addEventListener('abort', () => {
      bodyCtrl?.error(new DOMException('The operation was aborted.', 'AbortError'))
    })
    mockFetch.mockResolvedValue({ ok: true, status: 200, body: stream })

    const frames: Uint8Array[] = []
    const errors: unknown[] = []
    const p = fetchMjpegStream({
      url: '/api/v1/preview.mjpeg',
      token: 't',
      signal: controller.signal,
      onFrame: (f) => frames.push(f),
      onError: (e) => errors.push(e),
    })
    controller.abort()
    await p

    // 主动中止不应触发重连/故障上报（否则重连循环 + 闪烁）；中止发生在消费首块前，也不投递帧回调
    expect(errors).toHaveLength(0)
    expect(frames).toHaveLength(0)
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

  it('无 Content-Length：退化到“下一个边界之前”切帧（mjpeg.ts 退化分支）', () => {
    const jpeg = Uint8Array.of(0xff, 0xd8, 0xaa, 0xbb)
    const noLenHead = te.encode(`--${MJPEG_BOUNDARY}\r\nContent-Type: image/jpeg\r\n\r\n`)
    // 帧数据后紧跟 \r\n 与下一段 boundary：退化分支以“下一个 boundary”为帧尾，
    // 因此帧数据包含 jpeg 字节与帧后 \r\n 分隔符（无 Content-Length 无法精确裁剪）
    const body = cat(noLenHead, jpeg, te.encode('\r\n'), noLenHead)
    const r = splitMjpegFrames(body)
    expect(r.frames).toHaveLength(1)
    expect(Array.from(r.frames[0])).toEqual([0xff, 0xd8, 0xaa, 0xbb, 0x0d, 0x0a])
    // 第二段只有头部、无数据 → 剩余部分保留为 rest，等待其数据到齐
    expect(Array.from(r.rest)).toEqual(Array.from(noLenHead))
  })
})
