package com.example.rk3288_opencv;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;

/** 将处理后的 RGBA 帧交给本进程 FFmpeg；网络背压不得阻塞相机与 UI 线程。 */
final class RtmpCameraInput implements AutoCloseable {
    interface FrameSource extends AutoCloseable {
        boolean readFrame(byte[] rgba);
        @Override void close();
    }

    private final ServerSocket listener;
    private final Thread worker;
    private final FrameSource source;
    private final Runnable onFailure;
    private final int frameBytes;
    private final int frameIntervalMs;
    private volatile boolean closed;
    private volatile boolean failed;
    private Socket connection;

    RtmpCameraInput(FrameSource source, int width, int height, int fps,
                    Runnable onFailure) throws IOException {
        this.source = source;
        this.onFailure = onFailure;
        if (width <= 0 || height <= 0 || fps <= 0 || fps > 60
                || (long) width * height > Integer.MAX_VALUE / 4) {
            source.close();
            throw new IllegalArgumentException("相机推流尺寸或帧率无效");
        }
        frameBytes = width * height * 4;
        frameIntervalMs = 1000 / fps;
        try {
            // 不绑定通配地址，防止处理后的相机画面被局域网读取。
            listener = new ServerSocket(0, 1, InetAddress.getByAddress(new byte[]{127, 0, 0, 1}));
            listener.setSoTimeout(5000);
        } catch (IOException e) {
            source.close();
            throw e;
        }
        worker = new Thread(this::pump, "RtmpCameraFrames");
        worker.setDaemon(true);
        worker.start();
    }

    String inputUrl() {
        return "tcp://127.0.0.1:" + listener.getLocalPort();
    }

    boolean hasFailed() { return failed; }

    private void pump() {
        try {
            Socket accepted = listener.accept();
            synchronized (this) {
                if (closed) {
                    accepted.close();
                    return;
                }
                connection = accepted;
            }
            listener.close();
            OutputStream output = accepted.getOutputStream();
            byte[] rgba = new byte[frameBytes];
            long lastFrameNs = System.nanoTime();
            boolean hasFrame = false;
            while (!closed) {
                if (source.readFrame(rgba)) {
                    hasFrame = true;
                    lastFrameNs = System.nanoTime();
                } else if (System.nanoTime() - lastFrameNs >= 5_000_000_000L) {
                    throw new IOException("相机推流帧超时");
                }
                // 低帧率识别时重复最近一帧，维持声明的时基；持续无新帧仍须失败退出。
                if (hasFrame) output.write(rgba);
                Thread.sleep(frameIntervalMs);
            }
        } catch (IOException | RuntimeException | LinkageError e) {
            if (!closed) {
                failed = true;
                onFailure.run();
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            close();
            source.close();
        }
    }

    @Override public synchronized void close() {
        if (closed) return;
        closed = true;
        // close 可解除 accept/write 阻塞；Bitmap 由 worker 的 finally 释放，避免采集期间回收。
        try { listener.close(); } catch (IOException ignored) { }
        if (connection != null) {
            try { connection.close(); } catch (IOException ignored) { }
        }
        worker.interrupt();
    }
}
