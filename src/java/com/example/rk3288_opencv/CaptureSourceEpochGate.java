package com.example.rk3288_opencv;

/**
 * 相机源代次门（CaptureSourceEpochGate）。
 *
 * <p>统一旧的两套独立实现：Camera2 的 {@code int generation} + {@code listenerGeneration}
 * 与 CameraX 的 {@code StartSessionGate}。切换相机/控制器/预览源或 stop/start 时调用
 * {@link #advance()} 或 {@link #enter()} 使代次单调递增；异步绑定、帧回调、状态回调在
 * 捕获进入时的 epoch（{@link #enter()} 的返回值），执行时用 {@link #isCurrent(long)}
 * 校验仍为当前源，否则整体丢弃（不 bind/不覆盖/不推送）。</p>
 *
 * <p>语义约束：epoch 仅由源切换/启停等配置变化推进，不由帧内容或检测结果改变；
 * 可见性由 {@code volatile} 保证。写来源：主线程 start/stop 与 Camera2 错误路径
 * 回调线程的 advance——两者交错仅微秒级窗口且与旧 {@code generation++} 实现同构的
 * 既有债（非本次迁移引入），故不额外加锁；维护时应保持与旧语义逐字节等价。</p>
 */
final class CaptureSourceEpochGate {
    private volatile long current;

    /** 进入新会话：返回本次 epoch，并使之前的源过期。 */
    long enter() {
        return ++current;
    }

    /** 使当前及之前的源全部过期（stop/重启/切换源）。 */
    void advance() {
        ++current;
    }

    /** epoch 是否仍为当前源；false 表示旧源回调，应丢弃。 */
    boolean isCurrent(long epoch) {
        return epoch == current;
    }

    /** 当前 source epoch（仅供状态展示/诊断，任务 3 使用）。 */
    long current() {
        return current;
    }
}