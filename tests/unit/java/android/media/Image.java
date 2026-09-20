package android.media;

import java.nio.ByteBuffer;

/**
 * JVM 单测测试替身：{@code android.media.Image}（含嵌套 {@code Plane}）。
 *
 * 背景：真实 Image 为抽象类、Plane 亦为抽象类，且 mockable android.jar 方法体抛
 * "not mocked"，无法在纯 JVM 中构造。本替身在测试运行时遮蔽 mockable jar，供生产
 * Camera2CaptureController.onImageAvailable 读取平面/尺寸/时间戳并关闭帧。
 *
 * 仅定义帧路径引用的最小 API 面。
 */
public class Image implements AutoCloseable {

    /** 关闭计数：finally 释放路径的观测点。 */
    public int closeCalls;

    private Plane[] planes;
    private int width;
    private int height;
    private long timestamp;

    public void setPlanes(Plane[] planes) {
        this.planes = planes;
    }

    public void setDimensions(int width, int height) {
        this.width = width;
        this.height = height;
    }

    public void setTimestamp(long timestamp) {
        this.timestamp = timestamp;
    }

    public Plane[] getPlanes() {
        return planes;
    }

    public int getWidth() {
        return width;
    }

    public int getHeight() {
        return height;
    }

    public long getTimestamp() {
        return timestamp;
    }

    @Override
    public void close() {
        closeCalls++;
    }

    /** 与 android.media.Image.Plane 同构的最小替身（生产仅读 buffer/stride）。 */
    public static class Plane {

        private final ByteBuffer buffer;
        private final int rowStride;
        private final int pixelStride;

        public Plane(ByteBuffer buffer, int rowStride, int pixelStride) {
            this.buffer = buffer;
            this.rowStride = rowStride;
            this.pixelStride = pixelStride;
        }

        public ByteBuffer getBuffer() {
            return buffer;
        }

        public int getRowStride() {
            return rowStride;
        }

        public int getPixelStride() {
            return pixelStride;
        }
    }
}
