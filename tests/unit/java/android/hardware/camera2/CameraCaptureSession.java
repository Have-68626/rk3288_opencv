package android.hardware.camera2;

import android.os.Handler;

/**
 * JVM 单测测试替身（Q4-7 成功路径）。
 *
 * 背景：真实 {@code CameraCaptureSession} 为抽象类且构造器为包私有
 * （android.hardware.camera2），AGP mockable android.jar 的方法还会抛 "not mocked"，
 * 无法在 com.example.rk3288_opencv 下构造。本测试源集内的同名类在测试运行时遮蔽
 * mockable jar（测试类优先于 android.jar 进入类路径），提供可构造、setRepeatingRequest
 * 为空实现的替身，使生产 handleSessionConfigured 的 setRepeatingRequest 成功分支可被驱动。
 *
 * 仅定义生产代码引用的最小 API 面：setRepeatingRequest / stopRepeating / close /
 * StateCallback / CaptureCallback。生产类在编译期仍链接真实 SDK。
 */
public class CameraCaptureSession implements AutoCloseable {

    public CameraCaptureSession() {
    }

    public int setRepeatingRequest(CaptureRequest request, CaptureCallback callback, Handler handler) {
        return 0;
    }

    public void stopRepeating() {
    }

    @Override
    public void close() {
    }

    public static abstract class StateCallback {
        public StateCallback() {
        }

        public abstract void onConfigured(CameraCaptureSession session);

        public abstract void onConfigureFailed(CameraCaptureSession session);
    }

    public static abstract class CaptureCallback {
        public CaptureCallback() {
        }
    }
}
