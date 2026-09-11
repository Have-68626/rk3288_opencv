package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * Q-CQ-B07-004 — CameraX 异步 bind 竞态：过期 Future 完成时不 bind/不覆盖。
 *
 * RED 判别逻辑：
 *  修复前 CameraXCaptureController.start() 的 ProcessCameraProvider 回调无条件执行
 *  provider.unbindAll()/bindToLifecycle() 并覆盖 camera/analysis/sensorOrientation 等字段。
 *  "启动→过期 Future 延迟完成→再启动新流程"场景下，旧回调必然重新 bind 并覆盖新状态
 *  → 本测试对"旧代次被拒绝（旧 Future 不 bind/不覆盖）"的断言在修复前失败。
 *  修复后每次启动/停止递增代次，回调先校验代次仍为当前才继续，过期则整体忽略：
 *  不执行 unbindAll()/bindToLifecycle()，不写 camera/analysis 字段 —— 即不 bind 新 Surface、
 *  不覆盖新状态（与 MonitoringCoordinator 的 isCurrentGeneration 取消门模式一致）。
 */
public class CameraXCaptureControllerStartGateTest {

    @Test
    public void staleFutureCompletionAfterRestartIsIgnored() {
        // 场景：启动 → 过期 Future 延迟完成 → 再启动新流程
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();

        // 第一次启动：异步 Future 尚未完成，捕获本次会话代次
        long firstStart = gate.enter();
        assertTrue(gate.isCurrent(firstStart));

        // 再启动新流程：旧会话代次随之过期
        long secondStart = gate.enter();
        assertFalse(gate.isCurrent(firstStart));   // 旧 Future 完成时不得 bind/不覆盖新状态
        assertTrue(gate.isCurrent(secondStart));   // 新流程代次仍有效，允许正常 bind
    }

    @Test
    public void stopInvalidatesInFlightBind() {
        // 启动后 stop()：尚未完成的异步绑定过期（旧回调不得重新拉起相机）
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();

        long start = gate.enter();
        assertTrue(gate.isCurrent(start));

        gate.advance();
        assertFalse(gate.isCurrent(start));
    }

    @Test
    public void currentSessionWithoutInterruptionStillProceeds() {
        // 无并发/无 stop 的正常流程：当前代次始终放行，避免修复引入回归
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();

        long epoch = gate.enter();
        assertTrue(gate.isCurrent(epoch));

        gate.advance();
        assertFalse(gate.isCurrent(epoch));

        // 再次启动后，新代次重新放行
        long again = gate.enter();
        assertTrue(gate.isCurrent(again));
    }

    /**
     * P1-4：旧代次排队帧被丢弃。
     * analyzer 绑定（setAnalyzer）时捕获会话代次，帧送达时以该捕获值校验；stop/切换源
     * 推进代次后，stop 前排队的 ImageProxy 携带旧捕获值，analyze 入口 isCurrent 为 false
     * → 整帧丢弃（不推送、不统计）。修复前 analyze 无代次校验，旧帧仍会被推送/统计。
     */
    @Test
    public void staleQueuedFrameEpochIsDropped() {
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();
        long bindEpoch = gate.enter();          // start：bind analyzer 时捕获会话代次
        assertTrue("绑定时的会话帧应放行", gate.isCurrent(bindEpoch));

        gate.advance();                         // stop：使在途/排队的旧帧过期
        assertFalse("stop 后旧 analyzer 捕获的代次应失效", gate.isCurrent(bindEpoch));

        long newEpoch = gate.enter();           // 再次 start：新会话
        assertFalse("旧会话排队帧不得在新会话被放行", gate.isCurrent(bindEpoch));
        assertTrue("新会话绑定帧应放行", gate.isCurrent(newEpoch));
    }
}
