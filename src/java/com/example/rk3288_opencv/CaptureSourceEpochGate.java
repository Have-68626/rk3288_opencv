package com.example.rk3288_opencv;

import java.util.concurrent.atomic.AtomicLong;

/**
 * 相机源代次门（CaptureSourceEpochGate）。
 *
 * <p>统一旧的两套独立实现：Camera2 的 {@code int generation} + {@code listenerGeneration}
 * 与 CameraX 的 {@code StartSessionGate}。切换相机/控制器/预览源或 stop/start 时调用
 * {@link #advance()} 或 {@link #enter()} 使代次单调递增；异步绑定、帧回调、状态回调在
 * 捕获进入时的 epoch（{@link #enter()} 的返回值），执行时用 {@link #isCurrent(long)}
 * 校验仍为当前源，否则整体丢弃（不 bind/不覆盖/不推送）。</p>
 *
 * <p>语义约束：epoch 仅由源切换/启停等配置变化推进，不由帧内容或检测结果改变。
 * 写来源为主线程 start/stop 与 Camera2 错误路径回调线程，二者可能并发；代次状态由
 * {@link AtomicLong} 承载，{@link #enter()} / {@link #advance()} 通过原子自增保证
 * 递增不丢更新，读写之间建立 happens-before，故无需外部加锁。</p>
 */
final class CaptureSourceEpochGate {
    private final AtomicLong current = new AtomicLong();

    /** 进入新会话：返回本次 epoch，并使之前的源过期。 */
    long enter() {
        return current.incrementAndGet();
    }

    /** 使当前及之前的源全部过期（stop/重启/切换源）。 */
    void advance() {
        current.incrementAndGet();
    }

    /** epoch 是否仍为当前源；false 表示旧源回调，应丢弃。 */
    boolean isCurrent(long epoch) {
        return epoch == current.get();
    }

    /** 当前 source epoch（仅供状态展示/诊断，任务 3 使用）。 */
    long current() {
        return current.get();
    }
}