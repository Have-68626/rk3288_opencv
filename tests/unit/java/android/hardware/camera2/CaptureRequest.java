package android.hardware.camera2;

import android.util.Range;
import android.view.Surface;

/**
 * JVM 单测测试替身（Q4-7 成功路径）。
 *
 * 背景：真实 {@code CaptureRequest.Builder} 的构造器为包私有（android.hardware.camera2），
 * 且 AGP mockable android.jar 的所有方法会抛 "not mocked" 异常，无法在
 * com.example.rk3288_opencv 下构造真实对象驱动 handleSessionConfigured 的成功分支。
 * 本测试源集内的同名类在测试运行时遮蔽 mockable jar（测试类优先于 android.jar 进入
 * 类路径），提供可构造、方法为空实现的 Builder，使生产 setRepeatingRequest 成功路径
 * 可被驱动；生产类在编译期仍链接真实 SDK，运行时仅本测试使用该替身。
 *
 * 仅定义生产代码引用的最小 API 面：CONTROL_AF_MODE / CONTROL_AE_TARGET_FPS_RANGE /
 * Builder.set/addTarget/build。缺省分支（fps 为 null）不触碰 Range 方法，故仅作类型使用。
 */
public final class CaptureRequest {

    public static final Key<Integer> CONTROL_AF_MODE = new Key<>("control_af_mode");
    public static final int CONTROL_AF_MODE_CONTINUOUS_VIDEO = 3;
    public static final Key<Range<Integer>> CONTROL_AE_TARGET_FPS_RANGE = new Key<>("control_ae_target_fps_range");

    private CaptureRequest() {
    }

    public static final class Key<T> {
        private final String name;

        public Key(String name) {
            this.name = name;
        }

        public String getName() {
            return name;
        }
    }

    public static final class Builder {
        public Builder() {
        }

        public <T> void set(Key<T> key, T value) {
        }

        public void addTarget(Surface surface) {
        }

        public CaptureRequest build() {
            return new CaptureRequest();
        }
    }
}
