package android.content;

/**
 * JVM 单测测试替身：{@code android.content.Context}（最小面）。
 *
 * <p>背景：真实/mockable {@code Context} 是**抽象类**（compileSdk 34 下有 100+ 抽象方法），
 * mockable android.jar 的方法体默认抛 "not mocked" RuntimeException。要在纯 JVM 中构造
 * {@code CameraXCaptureController}（其构造器调用 {@code context.getApplicationContext()}）
 * 就需要一个非空 Context 实例；而子类化 Context 需实现全部抽象方法，成本不可接受。
 *
 * <p>故按本仓既有手法（同 {@code android.os.SystemClock} / {@code android.media.Image}
 * 替身）以同名类遮蔽 mockable jar。仅定义帧路径构造所需的最小 API 面：
 * {@link #getApplicationContext()} 返回自身。CameraX 帧路径（{@code analyze}）不触达
 * Context 的其它方法，故无需更多成员。
 *
 * <p>⚠️ 本替身遮蔽的是**整个** {@code android.content.Context} 类型：若将来某个单测驱动到
 * 生产代码里 Context 的其它方法（如 {@code getSystemService}），会在此处报
 * {@code NoSuchMethodError} —— 届时按需补充对应方法即可，勿据此认为 Context 不可测。
 */
public class Context {

    public Context getApplicationContext() {
        return this;
    }
}
