package android.os;

/**
 * JVM 单测测试替身：{@code android.os.SystemClock}。
 *
 * 背景：AGP 的 mockable android.jar 方法体默认抛 "not mocked" RuntimeException
 * （本仓未启用 {@code testOptions.unitTests.returnDefaultValues}）。相机帧回调路径
 * （Camera2CaptureController.onImageAvailable / CameraXCaptureController.analyze）
 * 会调用 elapsedRealtimeNanos()/elapsedRealtime() 采样耗时，真实 jar 下会直接抛异常、
 * 使帧路径无法在纯 JVM 中被驱动。本替身在测试运行时遮蔽 mockable jar，返回固定 0，
 * 令耗时统计退化为常量（不影响 FACE-CAM-008 代次复检行为）。
 *
 * 仅定义帧路径引用的最小 API 面。
 *
 * <p>⚠️ <b>本替身使时间相关逻辑在单测中不可观测</b>：所有方法恒返回 0，故任何依赖
 * 时间流逝的分支（耗时统计、超时判据等）在单测里都退化为常量，其时间语义**不被**本替身覆盖。
 *
 * <p>⚠️ <b>为何必须补齐 {@link #uptimeMillis()}</b>：主源 {@code LogDetailActivity} 使用
 * {@code SystemClock.uptimeMillis()}。javac 优先解析本源替身而非 mockable jar，故本类必须
 * 声明主源实际引用的**全部** {@code SystemClock} 方法 —— 否则任何触及 {@code LogDetailActivity}
 * 的单测会在编译期失败，且报错指向 {@code android.os.SystemClock}（迷惑性强）。
 */
public final class SystemClock {

    private SystemClock() {
    }

    public static long elapsedRealtime() {
        return 0L;
    }

    public static long elapsedRealtimeNanos() {
        return 0L;
    }

    public static long uptimeMillis() {
        return 0L;
    }
}
