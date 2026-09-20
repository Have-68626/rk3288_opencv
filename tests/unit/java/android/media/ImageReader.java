package android.media;

/**
 * JVM 单测测试替身：{@code android.media.ImageReader}（FACE-CAM-008 帧路径判别用例）。
 *
 * 背景：真实 ImageReader 构造器私有，且 mockable android.jar 方法体抛 "not mocked"，
 * 无法在纯 JVM 中构造。本替身在测试运行时遮蔽 mockable jar，仅供生产
 * Camera2CaptureController.onImageAvailable 调用 acquireLatestImage()/close()。
 *
 * 仅定义帧路径引用的最小 API 面。
 */
public class ImageReader implements AutoCloseable {

    private Image nextImage;

    /** 测试注入：下一次 acquireLatestImage() 返回的帧。 */
    public void setNextImage(Image image) {
        this.nextImage = image;
    }

    public Image acquireLatestImage() {
        return nextImage;
    }

    public android.view.Surface getSurface() {
        return null;
    }

    @Override
    public void close() {
    }
}
