package com.example.rk3288_opencv;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

final class MonitoringCoordinator {
    enum RunState { STOPPED, STARTING, RUNNING, RESTARTING }
    enum EngineState { NOT_READY, INITIALIZING, READY }

    static final class UiState {
        final String startStopText;
        final boolean startStopEnabled;
        final int startStopBackgroundColorArgb;
        final String statusText;
        final String captureStrategyInfoText;
        final boolean captureSchemeGroupEnabled;
        final float captureSchemeGroupAlpha;
        final boolean overlayVisible;

        UiState(
                String startStopText,
                boolean startStopEnabled,
                int startStopBackgroundColorArgb,
                String statusText,
                String captureStrategyInfoText,
                boolean captureSchemeGroupEnabled,
                float captureSchemeGroupAlpha,
                boolean overlayVisible
        ) {
            this.startStopText = startStopText;
            this.startStopEnabled = startStopEnabled;
            this.startStopBackgroundColorArgb = startStopBackgroundColorArgb;
            this.statusText = statusText;
            this.captureStrategyInfoText = captureStrategyInfoText;
            this.captureSchemeGroupEnabled = captureSchemeGroupEnabled;
            this.captureSchemeGroupAlpha = captureSchemeGroupAlpha;
            this.overlayVisible = overlayVisible;
        }
    }

    interface Effect {}

    static final class ShowToast implements Effect {
        final boolean longDuration;
        final String text;

        ShowToast(boolean longDuration, String text) {
            this.longDuration = longDuration;
            this.text = text;
        }
    }

    static final class RequestRuntimePermission implements Effect {}
    static final class MaybeShowGoToSettingsDialog implements Effect {}

    static final class RunPreflight implements Effect {
        final boolean wantCamera;
        final boolean wantMock;

        RunPreflight(boolean wantCamera, boolean wantMock) {
            this.wantCamera = wantCamera;
            this.wantMock = wantMock;
        }
    }

    static final class InitEngine implements Effect {
        final boolean wantMock;

        InitEngine(boolean wantMock) {
            this.wantMock = wantMock;
        }
    }

    static final class ScheduleInitEngine implements Effect {
        final long delayMs;
        final boolean wantMock;

        ScheduleInitEngine(long delayMs, boolean wantMock) {
            this.delayMs = delayMs;
            this.wantMock = wantMock;
        }
    }

    static final class StartMonitoringFlow implements Effect {
        final boolean wantCamera;
        final boolean wantMock;
        final int selectedCameraId;
        final String mockFilePathOrNull;
        final CaptureScheme schemeOrNull;

        StartMonitoringFlow(
                boolean wantCamera,
                boolean wantMock,
                int selectedCameraId,
                String mockFilePathOrNull,
                CaptureScheme schemeOrNull
        ) {
            this.wantCamera = wantCamera;
            this.wantMock = wantMock;
            this.selectedCameraId = selectedCameraId;
            this.mockFilePathOrNull = mockFilePathOrNull;
            this.schemeOrNull = schemeOrNull;
        }
    }

    static final class StopMonitoringFlow implements Effect {}
    static final class ScheduleStart implements Effect {
        final long delayMs;

        ScheduleStart(long delayMs) {
            this.delayMs = delayMs;
        }
    }

    static final class Decision {
        final UiState uiState;
        final List<Effect> effects;

        Decision(UiState uiState, List<Effect> effects) {
            this.uiState = uiState;
            this.effects = effects == null ? Collections.emptyList() : effects;
        }
    }

    static final class Inputs {
        final int selectedCameraId;
        final String mockFilePathOrNull;
        final boolean runtimeGranted;

        Inputs(int selectedCameraId, String mockFilePathOrNull, boolean runtimeGranted) {
            this.selectedCameraId = selectedCameraId;
            this.mockFilePathOrNull = mockFilePathOrNull;
            this.runtimeGranted = runtimeGranted;
        }

        boolean wantCamera() {
            return selectedCameraId >= 0 && mockFilePathOrNull == null;
        }

        boolean wantMock() {
            return selectedCameraId == -1 && mockFilePathOrNull != null;
        }

        boolean allowMockEvenWithoutPermission() {
            return wantMock();
        }
    }

    private static final int CAPTURE_RECOVERY_MAX_RETRIES = 2;
    private static final long[] CAPTURE_RECOVERY_BACKOFF_MS = new long[]{800L, 1600L};

    private RunState runState = RunState.STOPPED;
    private EngineState engineState = EngineState.NOT_READY;
    private boolean firstFrameReceived = false;

    private boolean captureAutoEnabled = true;
    private CaptureScheme preferredCaptureScheme = CaptureScheme.CAMERA2;
    private CaptureScheme activeCaptureScheme = CaptureScheme.CAMERA2;
    private CaptureScheme forcedNextScheme = null;
    private boolean autoSwitchedThisRun = false;
    private int captureRecoveryRetries = 0;
    private String lastCaptureSchemeReason = "";

    private String lastActiveCaptureNameOrNull = null;
    private String lastPreflightFirstLineOrNull = null;
    private String lastStoppedReasonOrNull = null;
    private String lastRestartReasonOrNull = null;

    MonitoringCoordinator() {}

    synchronized Decision syncFromActivity(
            Inputs in,
            boolean engineInitialized,
            boolean isRunning,
            boolean firstFrameReceived,
            boolean captureAutoEnabled,
            CaptureScheme preferredCaptureScheme,
            CaptureScheme activeCaptureScheme,
            String activeCaptureNameOrNull,
            String lastCaptureSchemeReasonOrNull
    ) {
        this.engineState = engineInitialized ? EngineState.READY : EngineState.NOT_READY;
        this.runState = isRunning ? RunState.RUNNING : RunState.STOPPED;
        this.firstFrameReceived = firstFrameReceived;
        this.captureAutoEnabled = captureAutoEnabled;
        if (preferredCaptureScheme != null) {
            this.preferredCaptureScheme = preferredCaptureScheme;
        }
        if (activeCaptureScheme != null) {
            this.activeCaptureScheme = activeCaptureScheme;
        }
        this.lastActiveCaptureNameOrNull = activeCaptureNameOrNull;
        this.lastCaptureSchemeReason = lastCaptureSchemeReasonOrNull == null ? "" : lastCaptureSchemeReasonOrNull;
        return snapshot(in);
    }

    synchronized Decision snapshot(Inputs in) {
        return new Decision(buildUiState(in), Collections.emptyList());
    }

    synchronized Decision onToggleStartStopClicked(Inputs in) {
        boolean runningOrStarting = runState == RunState.RUNNING || runState == RunState.STARTING || runState == RunState.RESTARTING;
        if (runningOrStarting) {
            return stopInternal(in, null);
        }
        return startInternal(in, null);
    }

    synchronized Decision onStartRequested(Inputs in, String reasonOrNull) {
        return startInternal(in, reasonOrNull);
    }

    synchronized Decision onStopRequested(Inputs in, String reasonOrNull) {
        return stopInternal(in, reasonOrNull);
    }

    synchronized Decision onRestartRequested(Inputs in, String reasonOrNull, long delayMs) {
        if (runState != RunState.RUNNING) {
            return snapshot(in);
        }
        lastRestartReasonOrNull = reasonOrNull;
        lastCaptureSchemeReason = reasonOrNull == null ? "" : reasonOrNull;

        List<Effect> fx = new ArrayList<>();
        if (reasonOrNull != null && !reasonOrNull.isEmpty()) {
            fx.add(new ShowToast(false, reasonOrNull));
        }
        runState = RunState.RESTARTING;
        firstFrameReceived = false;
        lastStoppedReasonOrNull = null;
        fx.add(new StopMonitoringFlow());
        fx.add(new ScheduleStart(Math.max(0L, delayMs)));
        return new Decision(buildUiState(in), fx);
    }

    synchronized Decision onCapturePolicyChanged(Inputs in, boolean captureAutoEnabled, CaptureScheme preferredCaptureScheme, String reasonOrNull) {
        this.captureAutoEnabled = captureAutoEnabled;
        if (preferredCaptureScheme != null) {
            this.preferredCaptureScheme = preferredCaptureScheme;
        }
        lastCaptureSchemeReason = reasonOrNull == null ? "" : reasonOrNull;

        if (runState == RunState.RUNNING) {
            return onRestartRequested(in, reasonOrNull, 450L);
        }
        return snapshot(in);
    }

    synchronized Decision onCaptureFailure(Inputs in, String reason) {
        if (runState != RunState.RUNNING) {
            return snapshot(in);
        }

        if (captureRecoveryRetries < CAPTURE_RECOVERY_MAX_RETRIES) {
            int next = captureRecoveryRetries + 1;
            captureRecoveryRetries = next;
            long delay = CAPTURE_RECOVERY_BACKOFF_MS[Math.min(next - 1, CAPTURE_RECOVERY_BACKOFF_MS.length - 1)];
            forcedNextScheme = activeCaptureScheme;
            return onRestartRequested(in, "恢复重试" + next + "/" + CAPTURE_RECOVERY_MAX_RETRIES + "(" + reason + ")", delay);
        }

        if (captureAutoEnabled && !autoSwitchedThisRun) {
            autoSwitchedThisRun = true;
            forcedNextScheme = (activeCaptureScheme == CaptureScheme.CAMERA2) ? CaptureScheme.CAMERAX : CaptureScheme.CAMERA2;
            return onRestartRequested(in, "自动降级(" + reason + ")", 450L);
        }

        lastStoppedReasonOrNull = reason;
        runState = RunState.STOPPED;
        firstFrameReceived = false;
        List<Effect> fx = new ArrayList<>();
        fx.add(new ShowToast(true, "采集失败: " + reason));
        fx.add(new StopMonitoringFlow());
        return new Decision(buildUiState(in), fx);
    }

    synchronized Decision onPermissionStateChanged(Inputs in, boolean runtimeGranted, List<String> missingPermissionsOrNull, String stateNameOrNull) {
        boolean allowMock = in.allowMockEvenWithoutPermission();
        if (!runtimeGranted) {
            if (in.selectedCameraId >= 0) {
                engineState = EngineState.NOT_READY;
                firstFrameReceived = false;
            }
            if (runState == RunState.RUNNING && in.selectedCameraId >= 0) {
                runState = RunState.STOPPED;
                lastStoppedReasonOrNull = "安全模式: " + (stateNameOrNull == null ? "" : stateNameOrNull);
                List<Effect> fx = new ArrayList<>();
                fx.add(new StopMonitoringFlow());
                fx.add(new MaybeShowGoToSettingsDialog());
                return new Decision(buildUiState(in), fx);
            }
            List<Effect> fx = new ArrayList<>();
            fx.add(new MaybeShowGoToSettingsDialog());
            return new Decision(buildUiState(in), fx);
        }

        List<Effect> fx = new ArrayList<>();
        engineState = EngineState.INITIALIZING;
        fx.add(new ScheduleInitEngine(500L, in.wantMock()));
        return new Decision(buildUiState(in), fx);
    }

    synchronized Decision onPreflightFinished(Inputs in, String preflightTextOrNull) {
        if (preflightTextOrNull == null || preflightTextOrNull.trim().isEmpty()) {
            lastPreflightFirstLineOrNull = null;
            return snapshot(in);
        }
        String raw = preflightTextOrNull.trim();
        int idx = raw.indexOf('\n');
        lastPreflightFirstLineOrNull = idx >= 0 ? raw.substring(0, idx) : raw;
        return snapshot(in);
    }

    synchronized Decision onEngineInitResult(Inputs in, boolean ok, boolean wantMock) {
        engineState = ok ? EngineState.READY : EngineState.NOT_READY;
        if (!ok) {
            lastStoppedReasonOrNull = wantMock ? "引擎初始化失败 (Mock)" : "引擎初始化失败";
            runState = RunState.STOPPED;
            firstFrameReceived = false;
            List<Effect> fx = new ArrayList<>();
            fx.add(new ShowToast(true, wantMock ? "Mock 引擎初始化失败或已取消" : "引擎初始化失败，无法启动监控"));
            return new Decision(buildUiState(in), fx);
        }

        if (runState == RunState.STARTING) {
            return continueStartAfterEngineReady(in);
        }
        return snapshot(in);
    }

    synchronized Decision onMonitoringStarted(Inputs in, boolean wantCamera, String activeCaptureNameOrNull, CaptureScheme schemeOrNull) {
        runState = RunState.RUNNING;
        lastStoppedReasonOrNull = null;
        lastRestartReasonOrNull = null;
        lastActiveCaptureNameOrNull = activeCaptureNameOrNull;
        if (wantCamera && schemeOrNull != null) {
            activeCaptureScheme = schemeOrNull;
        }
        return snapshot(in);
    }

    synchronized Decision onMonitoringStartFailed(Inputs in, String reason) {
        runState = RunState.STOPPED;
        firstFrameReceived = false;
        lastStoppedReasonOrNull = reason;
        List<Effect> fx = new ArrayList<>();
        fx.add(new ShowToast(true, reason));
        return new Decision(buildUiState(in), fx);
    }

    synchronized Decision onFirstFrameArrived(Inputs in) {
        firstFrameReceived = true;
        return snapshot(in);
    }

    private Decision startInternal(Inputs in, String reasonOrNull) {
        boolean wantCamera = in.wantCamera();
        boolean wantMock = in.wantMock();
        List<Effect> fx = new ArrayList<>();

        if (!wantCamera && !wantMock) {
            fx.add(new ShowToast(false, "请先选择摄像头或 Mock 源"));
            return new Decision(buildUiState(in), fx);
        }

        if (wantCamera && !in.runtimeGranted) {
            fx.add(new ShowToast(true, "权限不足：已进入安全模式"));
            fx.add(new RequestRuntimePermission());
            return new Decision(buildUiState(in), fx);
        }

        runState = RunState.STARTING;
        firstFrameReceived = false;
        lastStoppedReasonOrNull = null;
        if (reasonOrNull != null && !reasonOrNull.isEmpty()) {
            lastCaptureSchemeReason = reasonOrNull;
        }

        fx.add(new RunPreflight(wantCamera, wantMock));

        if (engineState != EngineState.READY) {
            engineState = EngineState.INITIALIZING;
            fx.add(new InitEngine(wantMock));
            if (wantMock) {
                fx.add(new ShowToast(false, "正在初始化 Mock 引擎，请稍候…"));
            }
            return new Decision(buildUiState(in), fx);
        }

        CaptureScheme scheme = null;
        if (wantCamera) {
            scheme = chooseStartScheme();
        }
        fx.add(new StartMonitoringFlow(wantCamera, wantMock, in.selectedCameraId, in.mockFilePathOrNull, scheme));
        return new Decision(buildUiState(in), fx);
    }

    private Decision stopInternal(Inputs in, String reasonOrNull) {
        runState = RunState.STOPPED;
        firstFrameReceived = false;
        lastStoppedReasonOrNull = reasonOrNull;
        lastRestartReasonOrNull = null;
        List<Effect> fx = new ArrayList<>();
        fx.add(new StopMonitoringFlow());
        return new Decision(buildUiState(in), fx);
    }

    private Decision continueStartAfterEngineReady(Inputs in) {
        boolean wantCamera = in.wantCamera();
        boolean wantMock = in.wantMock();
        if (!wantCamera && !wantMock) {
            runState = RunState.STOPPED;
            List<Effect> fx = new ArrayList<>();
            fx.add(new ShowToast(false, "请先选择摄像头或 Mock 源"));
            return new Decision(buildUiState(in), fx);
        }

        List<Effect> fx = new ArrayList<>();
        CaptureScheme scheme = null;
        if (wantCamera) {
            scheme = chooseStartScheme();
        }
        fx.add(new StartMonitoringFlow(wantCamera, wantMock, in.selectedCameraId, in.mockFilePathOrNull, scheme));
        return new Decision(buildUiState(in), fx);
    }

    private CaptureScheme chooseStartScheme() {
        boolean comingFromAutoSwitch = forcedNextScheme != null && captureAutoEnabled;
        CaptureScheme scheme = forcedNextScheme != null
                ? forcedNextScheme
                : (captureAutoEnabled ? CaptureScheme.CAMERA2 : preferredCaptureScheme);
        forcedNextScheme = null;
        activeCaptureScheme = scheme;
        autoSwitchedThisRun = comingFromAutoSwitch;
        if (captureAutoEnabled) {
            lastCaptureSchemeReason = comingFromAutoSwitch ? ("自动切换到 " + scheme.name()) : ("自动默认 " + scheme.name());
        } else {
            lastCaptureSchemeReason = "手动使用 " + scheme.name();
        }
        captureRecoveryRetries = 0;
        return scheme;
    }

    private UiState buildUiState(Inputs in) {
        boolean allowStart = in.runtimeGranted || in.allowMockEvenWithoutPermission();
        boolean runningOrStarting = runState == RunState.RUNNING || runState == RunState.STARTING || runState == RunState.RESTARTING;
        String btnText = runningOrStarting ? "STOP MONITORING" : "START MONITORING";
        int btnColor = runningOrStarting ? 0xFFFF0000 : 0xFF00FF00;

        boolean schemeEnabled = !captureAutoEnabled;
        float schemeAlpha = schemeEnabled ? 1.0f : 0.55f;

        String base = captureAutoEnabled
                ? "自动：默认 Camera2，失败切换到 CameraX"
                : "手动：固定使用所选方案";

        String preferred = preferredCaptureScheme == null ? "--" : preferredCaptureScheme.name();
        String active = lastActiveCaptureNameOrNull != null ? lastActiveCaptureNameOrNull : (activeCaptureScheme == null ? "--" : activeCaptureScheme.name());
        String stateLine = runningOrStarting
                ? ("当前生效: " + active)
                : (captureAutoEnabled ? "待生效: 自动" : ("待生效: " + preferred));

        String reason = lastCaptureSchemeReason == null ? "" : lastCaptureSchemeReason.trim();
        if (!reason.isEmpty()) {
            reason = "最近切换: " + reason;
        }
        String captureInfo = base + "\n" + stateLine + (reason.isEmpty() ? "" : ("\n" + reason));

        boolean permissionOk = in.runtimeGranted || in.allowMockEvenWithoutPermission();
        boolean engineOk = engineState == EngineState.READY;
        boolean overlayVisible = permissionOk && engineOk && (runState == RunState.RUNNING) && firstFrameReceived;

        String statusText;
        if (!permissionOk) {
            statusText = "Status: SAFE MODE (Missing Permissions)";
        } else if (lastPreflightFirstLineOrNull != null && runState == RunState.STARTING) {
            statusText = "Preflight: " + lastPreflightFirstLineOrNull;
        } else if (runState == RunState.RESTARTING) {
            String r = lastRestartReasonOrNull;
            statusText = (r != null && !r.isEmpty()) ? ("状态: 正在热重启... (" + r + ")") : "状态: 正在热重启...";
        } else if (runState == RunState.RUNNING) {
            if (in.wantCamera()) {
                String name = lastActiveCaptureNameOrNull == null ? "--" : lastActiveCaptureNameOrNull;
                statusText = "Status: Running (" + name + " / Cam " + in.selectedCameraId + ")";
            } else {
                statusText = "Status: Running";
            }
        } else if (engineState == EngineState.INITIALIZING) {
            statusText = "Status: Engine Initializing";
        } else if (engineState == EngineState.READY) {
            statusText = "Status: Engine Initialized";
        } else if (lastStoppedReasonOrNull != null && !lastStoppedReasonOrNull.trim().isEmpty()) {
            statusText = "Status: Stopped (" + lastStoppedReasonOrNull + ")";
        } else {
            statusText = "Status: Stopped";
        }

        return new UiState(
                btnText,
                allowStart,
                btnColor,
                statusText,
                captureInfo,
                schemeEnabled,
                schemeAlpha,
                overlayVisible
        );
    }
}

