package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * CaptureSourceEpochGate 统一代次门（任务 2 归一）单元测试。
 *
 * <p>覆盖计划要求的三类既有回归基线：
 * 1. 旧控制器延迟回调必须被丢弃（stop/切换源后 old epoch 不再放行）；
 * 2. 新旧源交错回调不会改变当前源状态（只有最新 enter 的 epoch 为 current）；
 * 3. epoch 只在配置变化（enter/advance）时递增，同一会话内保持稳定。
 */
public class CaptureSourceEpochGateTest {

    // ① 旧控制器延迟回调被丢弃：stop/启动新源后，旧 epoch 回调一律不通过门禁
    @Test
    public void oldSessionDelayedCallbackIsDropped() {
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();
        long oldSession = gate.enter();
        assertTrue(gate.isCurrent(oldSession));

        gate.advance(); // 模拟 stop
        assertFalse("stop 后旧会话回调应被丢弃", gate.isCurrent(oldSession));

        long newSession = gate.enter(); // 模拟再次 start
        assertTrue("当前新会话应放行", gate.isCurrent(newSession));
        assertFalse("旧会话不得复活", gate.isCurrent(oldSession));
    }

    // ② 新旧源交错：仅最新 enter 的源为 current，交错回调不改变状态
    @Test
    public void interleavedSourcesOnlyLatestIsCurrent() {
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();
        long srcA = gate.enter();
        long srcB = gate.enter();      // 切到 B
        assertFalse(gate.isCurrent(srcA));
        assertTrue(gate.isCurrent(srcB));

        gate.advance();                // B 被停
        assertFalse(gate.isCurrent(srcB));

        long srcC = gate.enter();      // 再切到 C
        assertTrue(gate.isCurrent(srcC));
        assertFalse("旧源 A/B 不得在当前影响渲染/翻转", gate.isCurrent(srcA));
        assertFalse(gate.isCurrent(srcB));
        assertEquals("epoch 应单调递增", srcC, gate.current());
    }

    // ③ 同一会话（无配置变化）期间保持稳定：回调多次校验均放行
    @Test
    public void sameSessionStableUntilConfigurationChange() {
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();
        long session = gate.enter();
        for (int i = 0; i < 100; i++) {
            assertTrue("同一会话期间回调应持续有效", gate.isCurrent(session));
            assertEquals("无配置变化时 epoch 不得改变", session, gate.current());
        }
    }

    // ④ epoch 仅在配置变化（enter/advance）时递增，不由其他事件改变
    @Test
    public void epochAdvancesOnlyOnConfigurationChange() {
        CaptureSourceEpochGate gate = new CaptureSourceEpochGate();
        long start = gate.current();
        // 无操作：保持不变
        assertEquals(start, gate.current());
        // enter / advance 各推进一次
        long e1 = gate.enter();
        assertEquals(start + 1, e1);
        assertEquals(e1, gate.current());
        gate.advance();
        assertEquals(e1 + 1, gate.current());
    }
}