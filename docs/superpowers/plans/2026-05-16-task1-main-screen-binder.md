# Task1 MainScreenBinder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 抽出独立 `MainScreenBinder.java`，让 `MainActivity` 的 `onCreate` 与配置变化重绑（`rebindViewsAfterConfigChange`）共用同一套 `findViewById + 监听注册` 入口，并修复 `btnHotRestart` 重复绑定导致“保存偏好 + 热重启”被覆盖的问题。

**Architecture:** `MainScreenBinder` 负责两件事：1) 一次性找齐所有需要的 View 并以 `Binding` 返回；2) 一次性注册所有 UI 监听，监听通过 `Callbacks` 回调到 `MainActivity` 的现有方法/状态。`MainActivity` 只做状态字段赋值与业务实现，避免在多个生命周期里复制 UI 绑定代码。

**Tech Stack:** Android + Java（现有工程风格），不引入新依赖。

---

## 文件结构

**Create**
- `D:\19842\Documents\GitHub\rk3288_opencv\src\java\com\example\rk3288_opencv\MainScreenBinder.java`

**Modify**
- `D:\19842\Documents\GitHub\rk3288_opencv\src\java\com\example\rk3288_opencv\MainActivity.java`
- `D:\19842\Documents\GitHub\rk3288_opencv\.trae\specs\refactor-android-mainactivity-monitoring\tasks.md`

---

## Task 1.1: 新增 MainScreenBinder（Binding + Callbacks）

**Files:**
- Create: `src\java\com\example\rk3288_opencv\MainScreenBinder.java`

- [ ] **Step 1: 新建 Binder 文件并定义对外 API**

目标 API（用于 `MainActivity` 复用）：

```java
package com.example.rk3288_opencv;

import android.app.Activity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.floatingactionbutton.FloatingActionButton;

final class MainScreenBinder {
    interface Callbacks {
        void onStartStopClicked();
        void onViewLogsClicked();
        void onNavPlaybackClicked();
        void onNavSettingsClicked();
        void onNavAboutClicked();
        void onExitClicked();
        void onSetMockUrlClicked();
        void onPushRtmpClicked();
        void onStopRtmpClicked();
        void onHotRestartClicked();
        void onModeChanged(int checkedId);
        void onToggleRecognitionPanelClicked();
    }

    static final class Binding {
        ImageView monitorView;
        View previewSurface;
        View videoWrapper;
        TextView tvFps;
        TextView tvCpu;
        TextView tvMemory;
        TextView tvLatency;
        TextView tvCaptureStrategyInfo;
        TextView tvStatus;
        TextView tvOverlayStatus;
        View recognitionTitle;

        Button btnStartStop;
        Button btnViewLogs;
        Button btnNavPlayback;
        Button btnNavSettings;
        Button btnNavAbout;
        Button btnExit;
        Button btnHotRestart;

        RadioGroup rgMode;
        RadioGroup rgCaptureScheme;
        Switch switchCaptureAuto;
        Spinner spinnerCameras;
        Switch switchFlipX;
        Switch switchFlipY;
        Switch switchAccelOpenCL;
        Switch switchAccelMpp;
        Switch switchAccelQualcomm;
        Switch switchOverlay;

        View panelSettings;

        EditText etMockUrl;
        Button btnSetMockUrl;

        EditText etRtmpUrl;
        Button btnPushRtmp;
        Button btnStopRtmp;

        View panelRecognitionEvents;
        RecyclerView rvRecognitionEvents;
        FloatingActionButton fabToggleEvents;
    }

    @NonNull
    static Binding bindViews(@NonNull Activity activity) {
        Binding b = new Binding();
        b.monitorView = activity.findViewById(R.id.monitor_view);
        b.previewSurface = activity.findViewById(R.id.preview_surface);
        b.videoWrapper = activity.findViewById(R.id.video_wrapper);

        b.tvFps = activity.findViewById(R.id.tv_fps);
        b.tvCpu = activity.findViewById(R.id.tv_cpu);
        b.tvMemory = activity.findViewById(R.id.tv_memory);
        b.tvLatency = activity.findViewById(R.id.tv_storage);
        b.tvCaptureStrategyInfo = activity.findViewById(R.id.tv_capture_strategy_info);
        b.tvStatus = activity.findViewById(R.id.tv_app_status);
        b.tvOverlayStatus = activity.findViewById(R.id.tv_overlay_status);

        b.recognitionTitle = activity.findViewById(R.id.recognition_title);
        b.btnStartStop = activity.findViewById(R.id.btn_start_stop);
        b.btnViewLogs = activity.findViewById(R.id.btn_view_logs);
        b.btnNavPlayback = activity.findViewById(R.id.btn_nav_playback);
        b.btnNavSettings = activity.findViewById(R.id.btn_nav_settings);
        b.btnNavAbout = activity.findViewById(R.id.btn_nav_about);
        b.btnExit = activity.findViewById(R.id.btn_exit);
        b.btnHotRestart = activity.findViewById(R.id.btn_hot_restart);

        b.rgMode = activity.findViewById(R.id.rg_mode);
        b.rgCaptureScheme = activity.findViewById(R.id.rg_capture_scheme);
        b.switchCaptureAuto = activity.findViewById(R.id.switch_capture_auto);
        b.spinnerCameras = activity.findViewById(R.id.spinner_cameras);
        b.switchFlipX = activity.findViewById(R.id.switch_flip_x);
        b.switchFlipY = activity.findViewById(R.id.switch_flip_y);
        b.switchAccelOpenCL = activity.findViewById(R.id.switch_accel_opencl);
        b.switchAccelMpp = activity.findViewById(R.id.switch_accel_mpp);
        b.switchAccelQualcomm = activity.findViewById(R.id.switch_accel_qualcomm);
        b.switchOverlay = activity.findViewById(R.id.switch_overlay);

        b.panelSettings = activity.findViewById(R.id.panel_settings);

        b.etMockUrl = activity.findViewById(R.id.et_mock_url);
        b.btnSetMockUrl = activity.findViewById(R.id.btn_set_mock_url);

        b.etRtmpUrl = activity.findViewById(R.id.et_rtmp_url);
        b.btnPushRtmp = activity.findViewById(R.id.btn_push_rtmp);
        b.btnStopRtmp = activity.findViewById(R.id.btn_stop_rtmp);

        b.panelRecognitionEvents = activity.findViewById(R.id.panel_recognition_events);
        b.rvRecognitionEvents = activity.findViewById(R.id.rv_recognition_events);
        b.fabToggleEvents = activity.findViewById(R.id.fab_toggle_events);
        return b;
    }

    static void bindListeners(@NonNull Binding b, @NonNull Callbacks cb) {
        if (b.btnStartStop != null) b.btnStartStop.setOnClickListener(v -> cb.onStartStopClicked());
        if (b.btnViewLogs != null) b.btnViewLogs.setOnClickListener(v -> cb.onViewLogsClicked());
        if (b.btnNavPlayback != null) b.btnNavPlayback.setOnClickListener(v -> cb.onNavPlaybackClicked());
        if (b.btnNavSettings != null) b.btnNavSettings.setOnClickListener(v -> cb.onNavSettingsClicked());
        if (b.btnNavAbout != null) b.btnNavAbout.setOnClickListener(v -> cb.onNavAboutClicked());
        if (b.btnExit != null) b.btnExit.setOnClickListener(v -> cb.onExitClicked());
        if (b.btnSetMockUrl != null) b.btnSetMockUrl.setOnClickListener(v -> cb.onSetMockUrlClicked());
        if (b.btnPushRtmp != null) b.btnPushRtmp.setOnClickListener(v -> cb.onPushRtmpClicked());
        if (b.btnStopRtmp != null) b.btnStopRtmp.setOnClickListener(v -> cb.onStopRtmpClicked());
        if (b.btnHotRestart != null) b.btnHotRestart.setOnClickListener(v -> cb.onHotRestartClicked());

        if (b.rgMode != null) {
            b.rgMode.setOnCheckedChangeListener((group, checkedId) -> cb.onModeChanged(checkedId));
        }
        if (b.fabToggleEvents != null) {
            b.fabToggleEvents.setOnClickListener(v -> cb.onToggleRecognitionPanelClicked());
        }
    }
}
```

- [ ] **Step 2: 本地构建验证（先不修改 MainActivity）**

运行（可复制）：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
.\gradlew.bat --no-daemon :app:assembleDebug
```

期望：编译通过（若失败，优先检查 import/包名/类名/引用的 R.id 是否存在）。

---

## Task 1.2: MainActivity 接入 Binder（onCreate 与 rebind 复用）

**Files:**
- Modify: `src\java\com\example\rk3288_opencv\MainActivity.java`

- [ ] **Step 1: 新增一个统一入口方法，替代两段重复 findViewById/监听代码**

目标：`onCreate()` 与 `rebindViewsAfterConfigChange()` 都调用同一个方法，例如：

```java
private void bindMainScreen(boolean isConfigChangeRebind) {
    MainScreenBinder.Binding b = MainScreenBinder.bindViews(this);

    monitorView = b.monitorView;
    previewSurface = (android.view.SurfaceView) b.previewSurface;
    videoWrapper = b.videoWrapper;
    tvFps = b.tvFps;
    tvCpu = b.tvCpu;
    tvMemory = b.tvMemory;
    tvLatency = b.tvLatency;
    tvCaptureStrategyInfo = b.tvCaptureStrategyInfo;
    tvStatus = b.tvStatus;
    tvOverlayStatus = b.tvOverlayStatus;
    recognitionTitle = b.recognitionTitle;

    btnStartStop = b.btnStartStop;
    btnViewLogs = b.btnViewLogs;
    btnNavPlayback = b.btnNavPlayback;
    btnNavSettings = b.btnNavSettings;
    btnNavAbout = b.btnNavAbout;
    btnExit = b.btnExit;
    btnHotRestart = b.btnHotRestart;

    rgMode = b.rgMode;
    rgCaptureScheme = b.rgCaptureScheme;
    switchCaptureAuto = b.switchCaptureAuto;
    spinnerCameras = b.spinnerCameras;
    switchFlipX = b.switchFlipX;
    switchFlipY = b.switchFlipY;
    switchAccelOpenCL = b.switchAccelOpenCL;
    switchAccelMpp = b.switchAccelMpp;
    switchAccelQualcomm = b.switchAccelQualcomm;
    switchOverlay = b.switchOverlay;

    panelSettings = b.panelSettings;
    etMockUrl = b.etMockUrl;
    btnSetMockUrl = b.btnSetMockUrl;
    etRtmpUrl = b.etRtmpUrl;
    btnPushRtmp = b.btnPushRtmp;
    btnStopRtmp = b.btnStopRtmp;

    panelRecognitionEvents = b.panelRecognitionEvents;
    rvRecognitionEvents = b.rvRecognitionEvents;
    fabToggleEvents = b.fabToggleEvents;

    if (tvOverlayStatus != null) tvOverlayStatus.setVisibility(View.GONE);

    MainScreenBinder.bindListeners(b, new MainScreenBinder.Callbacks() {
        @Override public void onStartStopClicked() { if (isRunning) stopMonitoring(); else startMonitoring(); }
        @Override public void onViewLogsClicked() { startActivity(new Intent(MainActivity.this, LogViewerActivity.class)); }
        @Override public void onNavPlaybackClicked() { pickMediaFile(); }
        @Override public void onNavSettingsClicked() { toggleSettingsPanel(); }
        @Override public void onNavAboutClicked() { showAboutDialog(); }
        @Override public void onExitClicked() { showExitDialog(); }
        @Override public void onSetMockUrlClicked() { applyMockUrl(); }
        @Override public void onPushRtmpClicked() { startRtmpPush(); }
        @Override public void onStopRtmpClicked() { stopRtmpPush(); }
        @Override public void onHotRestartClicked() { performHotRestartWithPrefsPersist(); }
        @Override public void onModeChanged(int checkedId) {
            int mode = (checkedId == R.id.rb_continuous) ? 0 : 1;
            nativeSetMode(mode);
        }
        @Override public void onToggleRecognitionPanelClicked() { toggleRecognitionPanel(); }
    });
}
```

其中 `performHotRestartWithPrefsPersist()` 是下一步要加的包装方法，用于保证“保存偏好 + 热重启”一次性完成。

- [ ] **Step 2: onCreate 与 rebindViewsAfterConfigChange 改为调用 bindMainScreen**

目标改造点：

1) `onCreate()`：保留 `setContentView(...)`、线程/Handler 初始化、状态初始化（prefs 读取、controller 创建等），但移除重复的 `findViewById` + 按钮监听注册片段，改为调用 `bindMainScreen(false)`。

2) `rebindViewsAfterConfigChange()`：移除重复的 `findViewById` + 各种 `setOnClickListener` 注册片段，改为调用 `bindMainScreen(true)`，并在其后继续执行与“重绑后需要恢复的 UI/状态”相关逻辑（例如：surface callback 重新注册、RecyclerView adapter 重挂、Spinner adapter 复位、全屏状态恢复、`discoverCameras()` 等）。

- [ ] **Step 3: 修复 btnHotRestart 重复绑定覆盖问题**

将 “保存偏好 + 热重启” 固化为一个方法，避免未来在别处再次被覆盖：

```java
private void performHotRestartWithPrefsPersist() {
    SharedPreferences.Editor ed = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit();
    if (switchAccelOpenCL != null) ed.putBoolean(PREF_ACCEL_OPENCL, switchAccelOpenCL.isChecked());
    if (switchAccelMpp != null) ed.putBoolean(PREF_ACCEL_MPP, switchAccelMpp.isChecked());
    if (switchAccelQualcomm != null) ed.putBoolean(PREF_ACCEL_QUALCOMM, switchAccelQualcomm.isChecked());
    ed.apply();
    performHotRestart();
}
```

并确保 `btnHotRestart` 只绑定一次（通过 binder），删除原先 `onCreate()` 里多个 `setOnClickListener` 重复赋值段落。

- [ ] **Step 4: 仓库内搜索验证（去除重复绑定残留模式）**

运行（可复制）：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
rg -n \"btnHotRestart\\.setOnClickListener\" -S
rg -n \"rebindViewsAfterConfigChange\\(\" -S
```

期望：
- `btnHotRestart.setOnClickListener` 只在 `MainScreenBinder.bindListeners` 的一处出现
- `rebindViewsAfterConfigChange()` 内不再包含大量 findViewById/监听注册复制段

- [ ] **Step 5: 本地构建验证**

按仓库约定跑一次（可复制）：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
.\gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

期望：全部通过。若 `lintDebug` 出错，优先修正空指针风险（view 可能为 null 的场景）与重复 import（本文件里已经有 `import android.widget.Button;` 重复一次）。

---

## Task 1.3: 更新 tasks.md 勾选 Task1

**Files:**
- Modify: `.trae\specs\refactor-android-mainactivity-monitoring\tasks.md`

- [ ] **Step 1: 将 Task 1 及其子项勾选为完成**

将以下项从 `- [ ]` 更新为 `- [x]`：
- Task 1 标题行
- 其下 4 个子项（提取绑定层、入口复用、HotRestart 重复绑定移除、代码层验证）

---

## 回滚方式（防止改动过大不好定位）

- 若出现 UI 行为异常：优先回滚 `MainActivity.java` 中对 binder 的接入（保留 `MainScreenBinder.java` 不会影响运行），或者临时让 `rebindViewsAfterConfigChange()` 回到旧逻辑以对比定位。
- 若出现监听不生效：检查是否重复调用了 `bindMainScreen(...)` 导致状态重置；以及是否某些监听（如 `switchCaptureAuto`、`rgCaptureScheme`、overlay/flip listeners）仍在旧位置，需要逐一迁移到统一入口或明确保留在 Activity 内部。

