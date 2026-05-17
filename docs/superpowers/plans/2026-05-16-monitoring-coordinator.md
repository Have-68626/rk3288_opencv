# MonitoringCoordinator Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 引入纯 Java 的 MonitoringCoordinator 收敛监控状态与决策，MainActivity 仅负责事件转发、执行一次性动作（Effects）与渲染 UiState，并保持现有行为一致。

**Architecture:** MonitoringCoordinator 内部维护有限状态与策略字段，所有“是否启动/停止/重启/失败恢复/权限安全模式/采集方案切换”等决策在 Coordinator 内完成；Coordinator 输出 UiState（Activity 渲染）与 Effects（Activity 执行并回传执行结果），从而把 UI/Android 副作用与业务决策分离。

**Tech Stack:** Java（Android 工程现有代码风格），不新增第三方依赖；构建验证使用 Gradle Wrapper。

---

## 文件结构与职责

- Create: `src/java/com/example/rk3288_opencv/MonitoringCoordinator.java`
  - 纯 Java 编排器：状态机 + 决策 + UiState/Effect 输出
- Modify: `src/java/com/example/rk3288_opencv/MainActivity.java`
  - 仅转发事件、执行 Effects（调用现有 native/capture/permission 逻辑）、渲染 UiState
- Modify: `.trae/specs/refactor-android-mainactivity-monitoring/tasks.md`
  - 勾选 Task 2

---

### Task 1: 新增 MonitoringCoordinator（有限状态 + UiState + Effects）

**Files:**
- Create: `src/java/com/example/rk3288_opencv/MonitoringCoordinator.java`

- [ ] **Step 1: 新建文件并定义数据结构（UiState / Effect / Inputs）**

在 `MonitoringCoordinator.java` 写入以下骨架（保持纯 Java，不引用 Android 类）：

```java
package com.example.rk3288_opencv;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

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
        ShowToast(boolean longDuration, String text) { this.longDuration = longDuration; this.text = text; }
    }

    static final class RequestRuntimePermission implements Effect {}
    static final class MaybeShowGoToSettingsDialog implements Effect {}

    static final class InitEngine implements Effect {
        final boolean wantMock;
        final String mockFilePathOrNull;
        InitEngine(boolean wantMock, String mockFilePathOrNull) { this.wantMock = wantMock; this.mockFilePathOrNull = mockFilePathOrNull; }
    }

    static final class StartMonitoringFlow implements Effect {
        final boolean wantCamera;
        final boolean wantMock;
        final int selectedCameraId;
        final String mockFilePathOrNull;
        final CaptureScheme schemeOrNull;
        StartMonitoringFlow(boolean wantCamera, boolean wantMock, int selectedCameraId, String mockFilePathOrNull, CaptureScheme schemeOrNull) {
            this.wantCamera = wantCamera;
            this.wantMock = wantMock;
            this.selectedCameraId = selectedCameraId;
            this.mockFilePathOrNull = mockFilePathOrNull;
            this.schemeOrNull = schemeOrNull;
        }
    }

    static final class StopMonitoringFlow implements Effect {}
    static final class ScheduleStart implements Effect { final long delayMs; ScheduleStart(long delayMs) { this.delayMs = delayMs; } }

    static final class PersistPreflight implements Effect { final String text; PersistPreflight(String text) { this.text = text; } }

    static final class Inputs {
        final int selectedCameraId;
        final String mockFilePathOrNull;
        final boolean runtimeGranted;
        Inputs(int selectedCameraId, String mockFilePathOrNull, boolean runtimeGranted) {
            this.selectedCameraId = selectedCameraId;
            this.mockFilePathOrNull = mockFilePathOrNull;
            this.runtimeGranted = runtimeGranted;
        }
        boolean wantCamera() { return selectedCameraId >= 0 && mockFilePathOrNull == null; }
        boolean wantMock() { return selectedCameraId == -1 && mockFilePathOrNull != null; }
        boolean allowMockEvenWithoutPermission() { return wantMock(); }
    }

    static final class Decision {
        final UiState uiState;
        final List<Effect> effects;
        Decision(UiState uiState, List<Effect> effects) { this.uiState = uiState; this.effects = effects; }
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

    private boolean lastRuntimeGranted = false;
    private List<String> lastMissingPermissions = Collections.emptyList();

    private boolean lastOverlayVisible = false;

    MonitoringCoordinator() {}

    Decision onStartStopClicked(Inputs in) { return decideStartStop(in); }
    Decision onCapturePolicyChanged(Inputs in, boolean captureAutoEnabled, CaptureScheme preferredCaptureScheme, String reason) { return decidePolicyChanged(in, captureAutoEnabled, preferredCaptureScheme, reason); }
    Decision onCaptureFailure(Inputs in, String reason) { return decideCaptureFailure(in, reason); }
    Decision onPermissionStateChanged(Inputs in, boolean runtimeGranted, List<String> missingPermissions, String stateName) { return decidePermissionChanged(in, runtimeGranted, missingPermissions, stateName); }
    Decision onFirstFrameArrived(Inputs in) { this.firstFrameReceived = true; return snapshot(in, Collections.emptyList()); }
    Decision onEngineInitResult(Inputs in, boolean ok, String modeName) { return decideEngineInitResult(in, ok, modeName); }

    Decision onMonitoringFlowStarted(Inputs in, boolean wantCamera, CaptureScheme schemeOrNull) { return decideMonitoringFlowStarted(in, wantCamera, schemeOrNull); }
    Decision onMonitoringFlowStartFailed(Inputs in, String reason) { return decideMonitoringFlowStartFailed(in, reason); }
    Decision onStopped(Inputs in, String reason) { return decideStopped(in, reason); }

    private Decision decideStartStop(Inputs in) { return snapshot(in, Collections.emptyList()); }
    private Decision decidePolicyChanged(Inputs in, boolean captureAutoEnabled, CaptureScheme preferredCaptureScheme, String reason) { return snapshot(in, Collections.emptyList()); }
    private Decision decideCaptureFailure(Inputs in, String reason) { return snapshot(in, Collections.emptyList()); }
    private Decision decidePermissionChanged(Inputs in, boolean runtimeGranted, List<String> missingPermissions, String stateName) { return snapshot(in, Collections.emptyList()); }
    private Decision decideEngineInitResult(Inputs in, boolean ok, String modeName) { return snapshot(in, Collections.emptyList()); }
    private Decision decideMonitoringFlowStarted(Inputs in, boolean wantCamera, CaptureScheme schemeOrNull) { return snapshot(in, Collections.emptyList()); }
    private Decision decideMonitoringFlowStartFailed(Inputs in, String reason) { return snapshot(in, Collections.emptyList()); }
    private Decision decideStopped(Inputs in, String reason) { return snapshot(in, Collections.emptyList()); }

    private Decision snapshot(Inputs in, List<Effect> effects) {
        UiState ui = buildUiState(in);
        return new Decision(ui, effects == null ? Collections.emptyList() : effects);
    }

    private UiState buildUiState(Inputs in) {
        boolean allowStart = in.runtimeGranted || in.allowMockEvenWithoutPermission();

        boolean running = runState == RunState.RUNNING || runState == RunState.STARTING || runState == RunState.RESTARTING;
        String btnText = running ? "STOP MONITORING" : "START MONITORING";
        int btnColor = running ? 0xFFFF0000 : 0xFF00FF00;

        boolean schemeEnabled = !captureAutoEnabled;
        float schemeAlpha = schemeEnabled ? 1.0f : 0.55f;

        String base = captureAutoEnabled ? "自动：默认 Camera2，失败切换到 CameraX" : "手动：固定使用所选方案";
        String preferred = preferredCaptureScheme == null ? "--" : preferredCaptureScheme.name();
        String active = activeCaptureScheme == null ? "--" : activeCaptureScheme.name();
        String state = running ? ("当前生效: " + active) : (captureAutoEnabled ? "待生效: 自动" : ("待生效: " + preferred));
        String reason = (lastCaptureSchemeReason == null ? "" : lastCaptureSchemeReason.trim());
        if (!reason.isEmpty()) reason = "最近切换: " + reason;
        String captureInfo = base + "\n" + state + (reason.isEmpty() ? "" : ("\n" + reason));

        boolean permissionOk = in.runtimeGranted || in.allowMockEvenWithoutPermission();
        boolean engineOk = engineState == EngineState.READY;
        boolean overlayVisible = permissionOk && engineOk && (runState == RunState.RUNNING) && firstFrameReceived;
        lastOverlayVisible = overlayVisible;

        String statusText;
        if (!permissionOk) {
            statusText = "Status: SAFE MODE (Missing Permissions)";
        } else if (engineState == EngineState.INITIALIZING) {
            statusText = "Status: Engine Initializing";
        } else if (!engineOk) {
            statusText = "Status: Engine Not Ready";
        } else if (runState == RunState.RUNNING) {
            statusText = "Status: Running";
        } else if (runState == RunState.RESTARTING) {
            statusText = "状态: 正在热重启...";
        } else {
            statusText = "Status: Stopped";
        }

        return new UiState(btnText, allowStart, btnColor, statusText, captureInfo, schemeEnabled, schemeAlpha, overlayVisible);
    }
}
```

- [ ] **Step 2: 填充决策逻辑（保持与现有 MainActivity 行为一致）**

把下面逻辑落到 `decideStartStop/decidePolicyChanged/decideCaptureFailure/decidePermissionChanged/decideEngineInitResult` 中：

1) Start/Stop 点击（对应原 `startMonitoring/stopMonitoring` 的决策部分）：

```java
private Decision decideStartStop(Inputs in) {
    List<Effect> fx = new ArrayList<>();
    boolean running = runState == RunState.RUNNING || runState == RunState.STARTING || runState == RunState.RESTARTING;
    if (running) {
        runState = RunState.STOPPED;
        firstFrameReceived = false;
        fx.add(new StopMonitoringFlow());
        return snapshot(in, fx);
    }

    boolean wantCamera = in.wantCamera();
    boolean wantMock = in.wantMock();
    if (!wantCamera && !wantMock) {
        fx.add(new ShowToast(false, "请先选择摄像头或 Mock 源"));
        return snapshot(in, fx);
    }

    if (wantCamera && !in.runtimeGranted) {
        fx.add(new ShowToast(true, "权限不足：已进入安全模式"));
        fx.add(new RequestRuntimePermission());
        return snapshot(in, fx);
    }

    runState = RunState.STARTING;
    firstFrameReceived = false;

    fx.add(new PersistPreflight("__REQUEST__"));

    if (engineState != EngineState.READY) {
        engineState = EngineState.INITIALIZING;
        fx.add(new InitEngine(wantMock, in.mockFilePathOrNull));
        if (wantMock) {
            fx.add(new ShowToast(false, "正在初始化 Mock 引擎，请稍候…"));
            return snapshot(in, fx);
        }
        return snapshot(in, fx);
    }

    CaptureScheme scheme = null;
    if (wantCamera) {
        boolean comingFromAutoSwitch = (forcedNextScheme != null && captureAutoEnabled);
        scheme = forcedNextScheme != null ? forcedNextScheme : (captureAutoEnabled ? CaptureScheme.CAMERA2 : preferredCaptureScheme);
        forcedNextScheme = null;
        activeCaptureScheme = scheme;
        autoSwitchedThisRun = comingFromAutoSwitch;
        if (captureAutoEnabled) {
            lastCaptureSchemeReason = comingFromAutoSwitch ? ("自动切换到 " + scheme.name()) : ("自动默认 " + scheme.name());
        } else {
            lastCaptureSchemeReason = "手动使用 " + scheme.name();
        }
        captureRecoveryRetries = 0;
    }

    fx.add(new StartMonitoringFlow(wantCamera, wantMock, in.selectedCameraId, in.mockFilePathOrNull, scheme));
    return snapshot(in, fx);
}
```

2) 策略变化（对应原 “切换自动模式/切换采集方案 -> restartMonitoring”）：

```java
private Decision decidePolicyChanged(Inputs in, boolean newAuto, CaptureScheme newPreferred, String reason) {
    this.captureAutoEnabled = newAuto;
    this.preferredCaptureScheme = newPreferred == null ? this.preferredCaptureScheme : newPreferred;
    this.lastCaptureSchemeReason = reason == null ? "" : reason;
    boolean running = runState == RunState.RUNNING;
    if (running) {
        return decideRestart(in, 450L, reason);
    }
    return snapshot(in, Collections.emptyList());
}

private Decision decideRestart(Inputs in, long delayMs, String reason) {
    List<Effect> fx = new ArrayList<>();
    runState = RunState.RESTARTING;
    firstFrameReceived = false;
    if (reason != null && !reason.isEmpty()) fx.add(new ShowToast(false, reason));
    fx.add(new StopMonitoringFlow());
    fx.add(new ScheduleStart(Math.max(0L, delayMs)));
    return snapshot(in, fx);
}
```

3) 采集失败（对应原 `handleCaptureFailure`：重试/backoff + 自动降级 + 停止）：

```java
private Decision decideCaptureFailure(Inputs in, String reason) {
    if (runState != RunState.RUNNING) return snapshot(in, Collections.emptyList());
    List<Effect> fx = new ArrayList<>();
    if (captureRecoveryRetries < CAPTURE_RECOVERY_MAX_RETRIES) {
        int next = captureRecoveryRetries + 1;
        captureRecoveryRetries = next;
        long delay = CAPTURE_RECOVERY_BACKOFF_MS[Math.min(next - 1, CAPTURE_RECOVERY_BACKOFF_MS.length - 1)];
        forcedNextScheme = activeCaptureScheme;
        return decideRestart(in, delay, "恢复重试" + next + "/" + CAPTURE_RECOVERY_MAX_RETRIES + "(" + reason + ")");
    }
    if (captureAutoEnabled && !autoSwitchedThisRun) {
        autoSwitchedThisRun = true;
        forcedNextScheme = (activeCaptureScheme == CaptureScheme.CAMERA2) ? CaptureScheme.CAMERAX : CaptureScheme.CAMERA2;
        return decideRestart(in, 450L, "自动降级(" + reason + ")");
    }
    fx.add(new ShowToast(true, "采集失败: " + reason));
    runState = RunState.STOPPED;
    firstFrameReceived = false;
    fx.add(new StopMonitoringFlow());
    return snapshot(in, fx);
}
```

4) 权限状态变化（对应原 `onPermissionStateChanged`：安全模式/停止/延迟 init）：

```java
private Decision decidePermissionChanged(Inputs in, boolean runtimeGranted, List<String> missingPermissions, String stateName) {
    this.lastRuntimeGranted = runtimeGranted;
    this.lastMissingPermissions = (missingPermissions == null) ? Collections.emptyList() : new ArrayList<>(missingPermissions);

    List<Effect> fx = new ArrayList<>();
    boolean allowMock = in.allowMockEvenWithoutPermission();
    if (!runtimeGranted) {
        if (in.selectedCameraId >= 0) {
            engineState = EngineState.NOT_READY;
            firstFrameReceived = false;
        }
        if (runState == RunState.RUNNING && in.selectedCameraId >= 0) {
            runState = RunState.STOPPED;
            fx.add(new StopMonitoringFlow());
        }
        fx.add(new MaybeShowGoToSettingsDialog());
        return snapshot(in, fx);
    }

    fx.add(new ScheduleStart(500L));
    engineState = EngineState.INITIALIZING;
    fx.add(new InitEngine(in.wantMock(), in.mockFilePathOrNull));
    return snapshot(in, fx);
}
```

5) 引擎初始化结果（用于 Mock 模式完成后 “pendingStartAfterInit” 行为）：

```java
private Decision decideEngineInitResult(Inputs in, boolean ok, String modeName) {
    engineState = ok ? EngineState.READY : EngineState.NOT_READY;
    List<Effect> fx = new ArrayList<>();
    if (!ok) {
        runState = RunState.STOPPED;
        firstFrameReceived = false;
        fx.add(new ShowToast(true, (modeName == null ? "" : modeName) + " 引擎初始化失败或已取消"));
        return snapshot(in, fx);
    }

    boolean wantMock = in.wantMock();
    if (wantMock && runState == RunState.STARTING) {
        CaptureScheme scheme = null;
        fx.add(new StartMonitoringFlow(false, true, in.selectedCameraId, in.mockFilePathOrNull, scheme));
    } else if (runState == RunState.STARTING) {
        boolean wantCamera = in.wantCamera();
        CaptureScheme scheme = wantCamera ? (captureAutoEnabled ? CaptureScheme.CAMERA2 : preferredCaptureScheme) : null;
        fx.add(new StartMonitoringFlow(wantCamera, in.wantMock(), in.selectedCameraId, in.mockFilePathOrNull, scheme));
    }
    return snapshot(in, fx);
}
```

- [ ] **Step 3: 实现运行流“开始成功/失败/停止”回调入口**

```java
private Decision decideMonitoringFlowStarted(Inputs in, boolean wantCamera, CaptureScheme schemeOrNull) {
    runState = RunState.RUNNING;
    if (wantCamera && schemeOrNull != null) {
        activeCaptureScheme = schemeOrNull;
    }
    return snapshot(in, Collections.emptyList());
}

private Decision decideMonitoringFlowStartFailed(Inputs in, String reason) {
    runState = RunState.STOPPED;
    firstFrameReceived = false;
    return snapshot(in, Collections.singletonList(new ShowToast(true, reason)));
}

private Decision decideStopped(Inputs in, String reason) {
    runState = RunState.STOPPED;
    firstFrameReceived = false;
    return snapshot(in, Collections.emptyList());
}
```

---

### Task 2: 改造 MainActivity：转发事件、执行 Effects、渲染 UiState

**Files:**
- Modify: `src/java/com/example/rk3288_opencv/MainActivity.java`

- [ ] **Step 1: 增加 coordinator 字段与 render/applyEffects 基础设施**

在 `MainActivity` 增加字段：

```java
private final MonitoringCoordinator monitoringCoordinator = new MonitoringCoordinator();
```

新增输入快照方法（从 Activity 当前字段拼出 Inputs）：

```java
private MonitoringCoordinator.Inputs snapshotInputs() {
    boolean runtimeGranted = permissionStateMachine != null && permissionStateMachine.isRuntimeGranted();
    return new MonitoringCoordinator.Inputs(selectedCameraId, mockFilePath, runtimeGranted);
}
```

新增 render 方法（唯一 UI 渲染入口）：

```java
private void renderMonitoringUi(@NonNull MonitoringCoordinator.UiState s) {
    if (btnStartStop != null) {
        btnStartStop.setText(s.startStopText);
        btnStartStop.setEnabled(s.startStopEnabled);
        btnStartStop.setBackgroundColor(s.startStopBackgroundColorArgb);
    }
    if (tvStatus != null) tvStatus.setText(s.statusText);
    if (tvCaptureStrategyInfo != null) tvCaptureStrategyInfo.setText(s.captureStrategyInfoText);
    if (tvOverlayStatus != null) tvOverlayStatus.setVisibility(s.overlayVisible ? View.VISIBLE : View.GONE);
    if (rgCaptureScheme != null) {
        rgCaptureScheme.setEnabled(s.captureSchemeGroupEnabled);
        rgCaptureScheme.setAlpha(s.captureSchemeGroupAlpha);
        for (int i = 0; i < rgCaptureScheme.getChildCount(); i++) {
            View c = rgCaptureScheme.getChildAt(i);
            if (c != null) {
                c.setEnabled(s.captureSchemeGroupEnabled);
                c.setAlpha(s.captureSchemeGroupAlpha);
            }
        }
    }
}
```

新增统一的“推进编排器 -> 执行 effects -> 渲染 ui”入口：

```java
private void dispatchDecision(@NonNull MonitoringCoordinator.Decision d) {
    applyMonitoringEffects(d.effects);
    renderMonitoringUi(d.uiState);
}
```

- [ ] **Step 2: 实现 applyMonitoringEffects（把旧 start/stop/restart/permission/native 行为挪到这里）**

新增：

```java
private void applyMonitoringEffects(@NonNull List<MonitoringCoordinator.Effect> effects) {
    if (effects.isEmpty()) return;
    MonitoringCoordinator.Inputs in = snapshotInputs();
    for (MonitoringCoordinator.Effect e : effects) {
        if (e instanceof MonitoringCoordinator.ShowToast) {
            MonitoringCoordinator.ShowToast t = (MonitoringCoordinator.ShowToast) e;
            Toast.makeText(this, t.text, t.longDuration ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT).show();
        } else if (e instanceof MonitoringCoordinator.RequestRuntimePermission) {
            if (permissionStateMachine != null) permissionStateMachine.requestWithUserConfirmation();
        } else if (e instanceof MonitoringCoordinator.MaybeShowGoToSettingsDialog) {
            if (permissionStateMachine != null) permissionStateMachine.showGoToSettingsDialogIfNeeded();
        } else if (e instanceof MonitoringCoordinator.PersistPreflight) {
            String preflight = runInputPreflight(in.wantCamera(), in.wantMock());
            if (preflight != null && !preflight.isEmpty()) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(PREF_LAST_PREFLIGHT, preflight).apply();
                AppLog.i("MainActivity", "preflight", preflight.replace('\n', ' '));
            }
        } else if (e instanceof MonitoringCoordinator.InitEngine) {
            MonitoringCoordinator.InitEngine ie = (MonitoringCoordinator.InitEngine) e;
            performInitEngineAndReportResult(ie.wantMock);
        } else if (e instanceof MonitoringCoordinator.StartMonitoringFlow) {
            MonitoringCoordinator.StartMonitoringFlow s = (MonitoringCoordinator.StartMonitoringFlow) e;
            performStartFlowAndReportResult(s);
        } else if (e instanceof MonitoringCoordinator.StopMonitoringFlow) {
            performStopFlow();
            dispatchDecision(monitoringCoordinator.onStopped(snapshotInputs(), "stop"));
        } else if (e instanceof MonitoringCoordinator.ScheduleStart) {
            MonitoringCoordinator.ScheduleStart ss = (MonitoringCoordinator.ScheduleStart) e;
            handler.postDelayed(() -> dispatchDecision(monitoringCoordinator.onStartStopClicked(snapshotInputs())), Math.max(0L, ss.delayMs));
        }
    }
}
```

其中 `performInitEngineAndReportResult/performStartFlowAndReportResult/performStopFlow` 为“执行层函数”，只保留动作不包含决策分支：

```java
private void performStopFlow() {
    isRunning = false;
    stopCaptureWatchdog();
    stopPreviewWatchdog();
    if (activeCapture != null) {
        try { activeCapture.stop(); } catch (Exception ignored) {}
        activeCapture = null;
    }
    if (engineInitialized) {
        try { nativeConfigureExternalInput(false, 0, 1); } catch (Exception ignored) {}
    }
    if (frameHandler != null) frameHandler.removeCallbacks(frameUpdater);
    nativeStop();
    rtmpPusher.stop();
    updateSystemReadyUi("停止监控");
}

private void performInitEngineAndReportResult(boolean wantMock) {
    initEngine();
    dispatchDecision(monitoringCoordinator.onEngineInitResult(snapshotInputs(), engineInitialized, wantMock ? "Mock" : "Camera"));
}

private void performStartFlowAndReportResult(@NonNull MonitoringCoordinator.StartMonitoringFlow s) {
    boolean wantCamera = s.wantCamera;
    boolean wantMock = s.wantMock;

    if (wantCamera) {
        nativeConfigureExternalInput(true, 0, 1);
        activeCaptureScheme = (s.schemeOrNull == null) ? CaptureScheme.CAMERA2 : s.schemeOrNull;
        activeCapture = (activeCaptureScheme == CaptureScheme.CAMERAX) ? cameraXCapture : camera2Capture;
        boolean started = activeCapture != null && activeCapture.start(String.valueOf(selectedCameraId));
        if (!started) {
            nativeConfigureExternalInput(false, 0, 1);
            activeCapture = null;
            dispatchDecision(monitoringCoordinator.onMonitoringFlowStartFailed(snapshotInputs(), "启动采集失败: " + (activeCapture == null ? "N/A" : activeCapture.name())));
            return;
        }
    }

    firstFrameReceived = false;
    isRunning = true;
    nativeStart();
    if (frameHandler != null) frameHandler.post(frameUpdater);
    if (wantCamera) startCaptureWatchdog();
    startPreviewWatchdog();
    dispatchDecision(monitoringCoordinator.onMonitoringFlowStarted(snapshotInputs(), wantCamera, activeCaptureScheme));
}
```

- [ ] **Step 3: 将旧决策入口替换为 coordinator 调用**

1) Start/Stop 点击：

把 `MainScreenBinder.Callbacks#onStartStopClicked` 内逻辑替换为：

```java
dispatchDecision(monitoringCoordinator.onStartStopClicked(snapshotInputs()));
```

2) `handleCaptureFailure`：保留方法壳但只转发：

```java
private void handleCaptureFailure(String reason) {
    dispatchDecision(monitoringCoordinator.onCaptureFailure(snapshotInputs(), reason));
}
```

3) `onPermissionStateChanged`：仅转发并渲染：

```java
private void onPermissionStateChanged(@NonNull PermissionStateMachine.State state, @NonNull List<String> missingRuntimePermissions) {
    boolean granted = (state == PermissionStateMachine.State.GRANTED);
    dispatchDecision(monitoringCoordinator.onPermissionStateChanged(snapshotInputs(), granted, missingRuntimePermissions, state.name()));
}
```

4) 首帧到达处（原 `updateSystemReadyUi("首帧到达")` 附近）追加：

```java
dispatchDecision(monitoringCoordinator.onFirstFrameArrived(snapshotInputs()));
```

5) 策略切换（自动开关、方案选择）处，把原 `restartMonitoring("切换...")` 改为：

```java
dispatchDecision(monitoringCoordinator.onCapturePolicyChanged(
        snapshotInputs(),
        captureAutoEnabled,
        preferredCaptureScheme,
        "切换自动模式"
));
```

以及：

```java
dispatchDecision(monitoringCoordinator.onCapturePolicyChanged(
        snapshotInputs(),
        captureAutoEnabled,
        preferredCaptureScheme,
        "切换采集方案"
));
```

- [ ] **Step 4: 确保 UiState 全量覆盖后，删除/旁路旧 UI 写入路径**

把以下直接写 UI 的路径调整为由 `renderMonitoringUi` 统一处理（避免状态不一致）：
- `startMonitoring/stopMonitoring/restartMonitoring/onPermissionStateChanged/updateCaptureStrategyInfo/applyCaptureUiState/updateSystemReadyUi` 内对按钮/tvStatus/tvCaptureStrategyInfo/tvOverlayStatus/rgCaptureScheme 的直接写入

---

### Task 3: 构建验证 + 勾选 Task2

**Files:**
- Modify: `.trae/specs/refactor-android-mainactivity-monitoring/tasks.md`

- [ ] **Step 1: 构建验证**

在仓库根目录运行：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
.\gradlew.bat --no-daemon :app:assembleDebug
```

预期：`BUILD SUCCESSFUL`。

- [ ] **Step 2: 更新 tasks.md 勾选 Task2**

把 `.trae/specs/refactor-android-mainactivity-monitoring/tasks.md` 的 Task2 子项全部改为 `[x]`，并把 Task2 也改为 `[x]`。

---

## 自检清单（落地前后都要过一遍）

- UiState 覆盖点齐全：Start/Stop、tvStatus、tvCaptureStrategyInfo、采集方案区 enable/alpha、overlayVisible
- 决策迁移齐全：start/stop/restart/handleCaptureFailure/onPermissionStateChanged/策略切换
- Mock 行为保持：Mock 初始化完成后能按旧逻辑自动启动（原 pendingStartAfterInit）
- 权限安全模式保持：权限丢失时停止外部采集并禁止初始化
- Watchdog 行为保持：仍通过 `handleCaptureFailure` 触发恢复/降级/停止

