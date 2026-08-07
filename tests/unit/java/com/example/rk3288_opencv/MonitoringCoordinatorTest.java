package com.example.rk3288_opencv;

import org.junit.Test;

import android.Manifest;

import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class MonitoringCoordinatorTest {

    @Test
    public void startWithoutInputShowsToastAndDisablesStart() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs(null, null, true);

        MonitoringCoordinator.Decision d = c.onStartRequested(in, null);

        assertNotNull(d);
        assertNotNull(d.uiState);
        assertTrue(d.uiState.startStopEnabled);
        assertEquals("START MONITORING", d.uiState.startStopText);
        assertTrue(d.effects.size() >= 1);
        assertTrue(d.effects.get(0) instanceof MonitoringCoordinator.ShowToast);
        MonitoringCoordinator.ShowToast t = (MonitoringCoordinator.ShowToast) d.effects.get(0);
        assertEquals("请先选择摄像头或 Mock 源", t.text);
    }

    @Test
    public void startCameraWithoutPermissionRequestsPermission() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, false);

        MonitoringCoordinator.Decision d = c.onStartRequested(in, null);

        assertNotNull(d);
        assertTrue(d.effects.size() >= 2);
        assertTrue(d.effects.get(0) instanceof MonitoringCoordinator.ShowToast);
        assertTrue(d.effects.get(1) instanceof MonitoringCoordinator.RequestRuntimePermission);
        assertEquals("START MONITORING", d.uiState.startStopText);
        assertFalse(d.uiState.startStopEnabled);
    }

    @Test
    public void startMockWithoutPermissionStillInitsEngine() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs(null, "/tmp/mock.mp4", false);

        MonitoringCoordinator.Decision d = c.onStartRequested(in, null);

        assertNotNull(d);
        assertTrue(d.uiState.startStopEnabled);
        assertEquals("STOP MONITORING", d.uiState.startStopText);

        boolean hasPreflight = false;
        boolean hasInitEngine = false;
        boolean hasToast = false;
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.RunPreflight) hasPreflight = true;
            if (e instanceof MonitoringCoordinator.InitEngine) hasInitEngine = true;
            if (e instanceof MonitoringCoordinator.ShowToast) hasToast = true;
        }
        assertTrue(hasPreflight);
        assertTrue(hasInitEngine);
        assertTrue(hasToast);
    }

    @Test
    public void engineReadyContinuesStartFlow() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs(null, "/tmp/mock.mp4", false);

        c.onStartRequested(in, null);
        MonitoringCoordinator.Decision d = c.onEngineInitResult(in, true, true);

        boolean hasStart = false;
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.StartMonitoringFlow) {
                hasStart = true;
                MonitoringCoordinator.StartMonitoringFlow s = (MonitoringCoordinator.StartMonitoringFlow) e;
                assertTrue(s.wantMock);
                assertFalse(s.wantCamera);
            }
        }
        assertTrue(hasStart);
    }

    @Test
    public void captureFailureTriggersRestartWithBackoff() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);

        MonitoringCoordinator.Decision d = c.onCaptureFailure(in, "首帧超时");

        boolean hasStop = false;
        boolean hasSchedule = false;
        long delay = -1L;
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.StopMonitoringFlow) hasStop = true;
            if (e instanceof MonitoringCoordinator.ScheduleStart) {
                hasSchedule = true;
                delay = ((MonitoringCoordinator.ScheduleStart) e).delayMs;
            }
        }
        assertTrue(hasStop);
        assertTrue(hasSchedule);
        assertEquals(800L, delay);
        assertEquals("STOP MONITORING", d.uiState.startStopText);
    }

    @Test
    public void permissionLostStopsMonitoringAndShowsSettingsDialog() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);

        MonitoringCoordinator.Inputs noPerm = new MonitoringCoordinator.Inputs("0", null, false);
        MonitoringCoordinator.Decision d = c.onPermissionStateChanged(noPerm, false, List.of(Manifest.permission.CAMERA), "DENIED");

        boolean hasStop = false;
        boolean hasDialog = false;
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.StopMonitoringFlow) hasStop = true;
            if (e instanceof MonitoringCoordinator.MaybeShowGoToSettingsDialog) hasDialog = true;
        }
        assertTrue(hasStop);
        assertTrue(hasDialog);
    }

    @Test
    public void overlayVisibleOnlyWhenReadyRunningAndFirstFrame() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);
        MonitoringCoordinator.Decision d = c.onFirstFrameArrived(in);

        assertTrue(d.uiState.overlayVisible);
    }

    // ── 代次/取消门：生命周期后陈旧排程任务被取消（可控调度器场景） ──

    private static int findScheduleStartGeneration(MonitoringCoordinator.Decision d) {
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.ScheduleStart) {
                return ((MonitoringCoordinator.ScheduleStart) e).generation;
            }
        }
        return -1;
    }

    private static int findScheduleInitGeneration(MonitoringCoordinator.Decision d) {
        for (MonitoringCoordinator.Effect e : d.effects) {
            if (e instanceof MonitoringCoordinator.ScheduleInitEngine) {
                return ((MonitoringCoordinator.ScheduleInitEngine) e).generation;
            }
        }
        return -1;
    }

    @Test
    public void staleScheduleStartAfterStopIsCancelled() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);
        MonitoringCoordinator.Decision d = c.onCaptureFailure(in, "首帧超时");  // 排程 ScheduleStart
        int scheduledGen = findScheduleStartGeneration(d);
        assertTrue(scheduledGen >= 0);
        assertTrue(c.isCurrentGeneration(scheduledGen));  // 排程时代次有效

        c.onStopRequested(in, "用户停止");  // stop → 陈旧任务取消
        assertFalse(c.isCurrentGeneration(scheduledGen));
    }

    @Test
    public void staleScheduleInitAfterPermissionRevokeIsCancelled() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);
        MonitoringCoordinator.Inputs noPerm = new MonitoringCoordinator.Inputs("0", null, false);

        MonitoringCoordinator.Decision d = c.onPermissionStateChanged(in, true, null, "GRANTED");  // 排程 ScheduleInitEngine
        int scheduledGen = findScheduleInitGeneration(d);
        assertTrue(scheduledGen >= 0);
        assertTrue(c.isCurrentGeneration(scheduledGen));

        c.onPermissionStateChanged(noPerm, false, List.of(Manifest.permission.CAMERA), "DENIED");  // 撤权 → 陈旧任务取消
        assertFalse(c.isCurrentGeneration(scheduledGen));
    }

    @Test
    public void staleScheduleAfterDestroyIsCancelled() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);
        MonitoringCoordinator.Decision d = c.onCaptureFailure(in, "首帧超时");
        int scheduledGen = findScheduleStartGeneration(d);
        assertTrue(c.isCurrentGeneration(scheduledGen));

        c.onActivityDestroyed(in);  // 销毁 → 陈旧任务取消
        assertFalse(c.isCurrentGeneration(scheduledGen));
    }

    @Test
    public void currentGenerationScheduleStillRuns() {
        MonitoringCoordinator c = new MonitoringCoordinator();
        MonitoringCoordinator.Inputs in = new MonitoringCoordinator.Inputs("0", null, true);

        c.onEngineInitResult(in, true, false);
        c.onMonitoringStarted(in, true, "Camera2", CaptureScheme.CAMERA2);
        MonitoringCoordinator.Decision d = c.onCaptureFailure(in, "首帧超时");
        int scheduledGen = findScheduleStartGeneration(d);
        // 无生命周期事件 → 排程代次仍当前 → 任务可执行
        assertTrue(c.isCurrentGeneration(scheduledGen));
    }
}

