package com.example.rk3288_opencv;

import android.Manifest;
import android.app.AlertDialog;
import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.Settings;
import android.animation.ValueAnimator;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.view.animation.LinearInterpolator;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.RadioGroup;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.app.ProgressDialog;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.core.content.FileProvider;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.window.layout.WindowMetrics;
import androidx.window.layout.WindowMetricsCalculator;
import com.google.android.material.floatingactionbutton.FloatingActionButton;
import android.provider.MediaStore;
import android.media.MediaMetadataRetriever;
import android.util.Range;
import android.util.Size;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.io.File;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

public class MainActivity extends AppCompatActivity implements CaptureObserver {

    // Load native library
    static {
        System.loadLibrary("native-lib");
    }
    
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_CAMERA_ID = "pref_camera_id";
    private static final String PREF_EVENTS_VISIBLE = "pref_events_visible";
    private static final String PREF_CAPTURE_AUTO = "pref_capture_auto";
    private static final String PREF_CAPTURE_SCHEME = "pref_capture_scheme";
    private static final String PREF_LAST_PREFLIGHT = "pref_last_preflight";
    private static final String PREF_OVERLAY_ENABLED = "pref_overlay_enabled";
    private static final String PREF_OVERLAY_RUNNING = "pref_overlay_running";
    private static final String PREF_FLIP_X_PREFIX = "pref_flip_x_";
    private static final String PREF_FLIP_Y_PREFIX = "pref_flip_y_";
    private static final String PREF_ACCEL_OPENCL = "pref_accel_opencl";
    private static final String PREF_ACCEL_MPP = "pref_accel_mpp";
    private static final String PREF_ACCEL_QUALCOMM = "pref_accel_qualcomm";
    private static final String PREF_DETECTION_THROTTLE_MODE = "pref_detection_throttle_mode";
    private static final String PREF_DETECTION_INTERVAL_MS = "pref_detection_interval_ms";
    private static final String PREF_INFERENCE_THROTTLE_MODE = "pref_inference_throttle_mode";
    private static final String PREF_INFERENCE_INTERVAL_MS = "pref_inference_interval_ms";
    private static final String TAG = "MainActivity";

    private static final int DETECTION_INTERVAL_DEFAULT_MS = 150;
    private static final int DETECTION_INTERVAL_MIN_MS = 80;
    private static final int DETECTION_INTERVAL_MAX_MS = 500;

    private static final int INFERENCE_INTERVAL_DEFAULT_MS = 650;
    private static final int INFERENCE_INTERVAL_MIN_MS = 80;
    private static final int INFERENCE_INTERVAL_MAX_MS = 2000;
    private static final String INFERENCE_INI_FILENAME = "rk3288_opencv.ini";

    private ImageView monitorView;
    private SurfaceView previewSurface;
    private TextView tvFps, tvCpu, tvMemory, tvStatus, tvOverlayStatus;
    private TextView tvLatency;
    private TextView tvCaptureStrategyInfo;
    private View recognitionTitle;
    private Button btnStartStop, btnViewLogs;
    private Button btnNavPlayback, btnNavSettings, btnNavAbout;
    private Button btnExit;
    private Button btnHotRestart;
    private RadioGroup rgMode;
    private Switch switchCaptureAuto;
    private RadioGroup rgCaptureScheme;
    private RadioGroup rgDetectionThrottle;
    private RadioGroup rgInferenceThrottle;
    private Spinner spinnerCameras;
    private Switch switchFlipX;
    private Switch switchFlipY;
    private Switch switchAccelOpenCL;
    private Switch switchAccelMpp;
    private Switch switchAccelQualcomm;
    private Switch switchOverlay;
    private View panelSettings;
    private View videoWrapper;
    private EditText etDetectionIntervalMs;
    private TextView tvDetectionIntervalEffective;
    private EditText etInferenceIntervalMs;
    private TextView tvInferenceIntervalEffective;
    private EditText etMockUrl;
    private Button btnSetMockUrl;
    private EditText etRtmpUrl;
    private Button btnPushRtmp;
    private Button btnStopRtmp;
    private final FfmpegRtmpPusher rtmpPusher = new FfmpegRtmpPusher();

    private View panelRecognitionEvents;
    private RecyclerView rvRecognitionEvents;
    private FloatingActionButton fabToggleEvents;
    private RecognitionEventAdapter recognitionEventAdapter;
    private boolean recognitionEventsVisible = true;
    private boolean isFullscreen = false;
    private GestureDetector monitorGestureDetector;
    
    private boolean isRunning = false;
    private boolean engineInitialized = false;
    private volatile boolean cancelInitMock = false;
    private boolean firstFrameReceived = false;
    private boolean lastReadyVisible = false;
    private boolean restartMonitoringOnStart = false;
    private PermissionStateMachine permissionStateMachine;
    private final MonitoringCoordinator monitoringCoordinator = new MonitoringCoordinator();
    private Bitmap frameBitmap;
    private Bitmap backBitmap;
    private final Object bitmapSwapLock = new Object();
    private volatile boolean previewSurfaceReady = false;
    private volatile long lastPreviewRenderOkRealtimeMs = 0L;
    private volatile int previewRenderFailStreak = 0;
    private volatile int previewRecoveryCount = 0;
    private volatile long lastPreviewRecoveryRealtimeMs = 0L;
    private Runnable previewWatchdog;
    private final SurfaceHolder.Callback previewSurfaceCallback = new SurfaceHolder.Callback() {
        @Override
        public void surfaceCreated(@NonNull SurfaceHolder holder) {
            try {
                nativeSetPreviewSurface(holder.getSurface());
                previewSurfaceReady = true;
                lastPreviewRenderOkRealtimeMs = SystemClock.elapsedRealtime();
                if (monitorView != null) monitorView.setVisibility(View.GONE);
            } catch (Throwable ignored) {
            }
        }

        @Override
        public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
        }

        @Override
        public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
            previewSurfaceReady = false;
            try {
                nativeSetPreviewSurface(null);
            } catch (Throwable ignored) {
            }
            if (monitorView != null) monitorView.setVisibility(View.VISIBLE);
        }
    };

    private final CompoundButton.OnCheckedChangeListener overlaySwitchListener = (buttonView, isChecked) -> {
        handleOverlayToggle(isChecked);
    };
    private final CompoundButton.OnCheckedChangeListener flipXSwitchListener = (buttonView, isChecked) -> {
        flipXEnabled = isChecked;
        flipXHasOverride = true;
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putBoolean(PREF_FLIP_X_PREFIX + getFlipSourceKey(), isChecked).apply();
        applyFlipToNative();
    };
    private final CompoundButton.OnCheckedChangeListener flipYSwitchListener = (buttonView, isChecked) -> {
        flipYEnabled = isChecked;
        flipYHasOverride = true;
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putBoolean(PREF_FLIP_Y_PREFIX + getFlipSourceKey(), isChecked).apply();
        applyFlipToNative();
    };
    private Handler handler = new Handler(Looper.getMainLooper());
    private HandlerThread frameThread;
    private Handler frameHandler;
    private Runnable frameUpdater;
    private Runnable statsUpdater;
    private Runnable captureWatchdog;
    
    private int selectedCameraId = 0;
    private List<CameraInfo> availableCameras = new ArrayList<>();
    private ArrayAdapter<String> cameraAdapter;
    private boolean isSpinnerInitialized = false;

    private boolean captureAutoEnabled = true;
    private CaptureScheme preferredCaptureScheme = CaptureScheme.CAMERA2;
    private CaptureScheme activeCaptureScheme = CaptureScheme.CAMERA2;

    private String detectionThrottleMode = "auto";
    private int detectionIntervalMs = DETECTION_INTERVAL_DEFAULT_MS;
    private int effectiveDetectionIntervalMs = DETECTION_INTERVAL_DEFAULT_MS;

    private String inferenceThrottleMode = "auto";
    private int inferenceIntervalMs = INFERENCE_INTERVAL_DEFAULT_MS;
    private int effectiveInferenceIntervalMs = INFERENCE_INTERVAL_DEFAULT_MS;
    private InferenceThrottleAutoTuner inferenceAutoTuner = new InferenceThrottleAutoTuner(INFERENCE_INTERVAL_DEFAULT_MS);
    private Runnable inferenceAutoUpdater;
    private long lastInferenceAutoHeartbeatMs = 0L;

    private CaptureController camera2Capture;
    private CaptureController cameraXCapture;
    private CaptureController activeCapture;

    private volatile boolean captureEverPushed = false;
    private volatile long lastPushOkRealtimeMs = 0L;
    private volatile int pushFailStreak = 0;

    // Mock fallback tracking
    private static final long MOCK_FALLBACK_TIMEOUT_MS = 5000;
    private final android.os.Handler mockFallbackHandler = new android.os.Handler(Looper.getMainLooper());
    private final Runnable mockFallbackRunnable = this::performMockFallback;
    private volatile boolean flipXEnabled = false;
    private volatile boolean flipYEnabled = false;
    private volatile boolean flipXHasOverride = false;
    private volatile boolean flipYHasOverride = false;

    // Camera Info Wrapper
    private static class CameraInfo {
        String id;
        String description;
        int facing;

        CameraInfo(String id, String description, int facing) {
            this.id = id;
            this.description = description;
            this.facing = facing;
        }

        @Override
        public String toString() {
            return description; // For Spinner display
        }
    }

    private final MainScreenBinder.Callbacks mainScreenCallbacks = new MainScreenBinder.Callbacks() {
        @Override
        public void onStartStopClicked() {
            dispatchMonitoringDecision(monitoringCoordinator.onToggleStartStopClicked(snapshotMonitoringInputs()));
        }

        @Override
        public void onViewLogsClicked() {
            Intent intent = new Intent(MainActivity.this, LogViewerActivity.class);
            startActivity(intent);
        }

        @Override
        public void onNavPlaybackClicked() {
            pickMediaFile();
        }

        @Override
        public void onNavSettingsClicked() {
            toggleSettingsPanel();
        }

        @Override
        public void onNavAboutClicked() {
            showAboutDialog();
        }

        @Override
        public void onExitClicked() {
            showExitDialog();
        }

        @Override
        public void onSetMockUrlClicked() {
            applyMockUrl();
        }

        @Override
        public void onPushRtmpClicked() {
            startRtmpPush();
        }

        @Override
        public void onStopRtmpClicked() {
            stopRtmpPush();
        }

        @Override
        public void onHotRestartClicked() {
            performHotRestartWithPrefsPersist();
        }

        @Override
        public void onModeChanged(int checkedId) {
            int mode = (checkedId == R.id.rb_continuous) ? 0 : 1;
            nativeSetMode(mode);
        }

        @Override
        public void onToggleRecognitionPanelClicked() {
            toggleRecognitionPanel();
        }

        @Override
        public boolean isCameraSpinnerInitialized() {
            return isSpinnerInitialized;
        }

        @Override
        public void setCameraSpinnerInitialized(boolean initialized) {
            isSpinnerInitialized = initialized;
        }

        @Override
        public void onCameraSpinnerItemSelected(int position) {
            handleCameraSpinnerSelection(position);
        }

        @Override
        public void onCaptureAutoChanged(boolean enabled) {
            handleCaptureAutoChanged(enabled);
        }

        @Override
        public void onCaptureSchemeChanged(@NonNull CaptureScheme scheme) {
            handleCaptureSchemeChanged(scheme);
        }
    };

    private void bindMainScreen(boolean resetSpinnerInit) {
        MainScreenBinder.Binding b = MainScreenBinder.bindViews(this);

        monitorView = b.monitorView;
        previewSurface = b.previewSurface;
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
        rgDetectionThrottle = b.rgDetectionThrottle;
        rgInferenceThrottle = b.rgInferenceThrottle;
        switchCaptureAuto = b.switchCaptureAuto;
        spinnerCameras = b.spinnerCameras;
        switchFlipX = b.switchFlipX;
        switchFlipY = b.switchFlipY;
        switchAccelOpenCL = b.switchAccelOpenCL;
        switchAccelMpp = b.switchAccelMpp;
        switchAccelQualcomm = b.switchAccelQualcomm;
        switchOverlay = b.switchOverlay;

        panelSettings = b.panelSettings;

        etDetectionIntervalMs = b.etDetectionIntervalMs;
        tvDetectionIntervalEffective = b.tvDetectionIntervalEffective;
        etInferenceIntervalMs = b.etInferenceIntervalMs;
        tvInferenceIntervalEffective = b.tvInferenceIntervalEffective;

        etMockUrl = b.etMockUrl;
        btnSetMockUrl = b.btnSetMockUrl;

        etRtmpUrl = b.etRtmpUrl;
        btnPushRtmp = b.btnPushRtmp;
        btnStopRtmp = b.btnStopRtmp;

        panelRecognitionEvents = b.panelRecognitionEvents;
        rvRecognitionEvents = b.rvRecognitionEvents;
        fabToggleEvents = b.fabToggleEvents;

        if (tvOverlayStatus != null) {
            tvOverlayStatus.setVisibility(View.GONE);
        }

        MainScreenBinder.bindStaticListeners(b, mainScreenCallbacks);
        MainScreenBinder.bindCaptureControls(b, captureAutoEnabled, preferredCaptureScheme, mainScreenCallbacks);
        bindInferenceThrottleControls();

        if (cameraAdapter != null) {
            MainScreenBinder.bindCameraSpinner(b, cameraAdapter, resetSpinnerInit, mainScreenCallbacks);
        }

        if (switchOverlay != null) {
            switchOverlay.setOnCheckedChangeListener(overlaySwitchListener);
            syncOverlaySwitchState();
        }
        if (switchFlipX != null) switchFlipX.setOnCheckedChangeListener(flipXSwitchListener);
        if (switchFlipY != null) switchFlipY.setOnCheckedChangeListener(flipYSwitchListener);
        refreshFlipFromPrefs(false);
        syncMonitoringUiAfterRebind();
    }

    private MonitoringCoordinator.Inputs snapshotMonitoringInputs() {
        boolean runtimeGranted = permissionStateMachine != null && permissionStateMachine.isRuntimeGranted();
        return new MonitoringCoordinator.Inputs(selectedCameraId, mockFilePath, runtimeGranted);
    }

    private void syncMonitoringUiAfterRebind() {
        renderMonitoringUi(monitoringCoordinator.snapshot(snapshotMonitoringInputs()).uiState);
    }

    private void dispatchMonitoringDecision(@NonNull MonitoringCoordinator.Decision d) {
        MonitoringCoordinator.Decision next = applyMonitoringEffects(d);
        renderMonitoringUi(next.uiState);
    }

    private void renderMonitoringUi(@NonNull MonitoringCoordinator.UiState s) {
        if (btnStartStop != null) {
            btnStartStop.setText(s.startStopText);
            btnStartStop.setEnabled(s.startStopEnabled);
            btnStartStop.setBackgroundColor(s.startStopBackgroundColorArgb);
        }
        if (tvStatus != null) {
            tvStatus.setText(s.statusText);
        }
        if (tvCaptureStrategyInfo != null) {
            tvCaptureStrategyInfo.setText(s.captureStrategyInfoText);
        }
        if (tvOverlayStatus != null) {
            tvOverlayStatus.setVisibility(s.overlayVisible ? View.VISIBLE : View.GONE);
        }
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

    private MonitoringCoordinator.Decision applyMonitoringEffects(@NonNull MonitoringCoordinator.Decision d) {
        if (d.effects == null || d.effects.isEmpty()) return d;

        MonitoringCoordinator.Inputs in = snapshotMonitoringInputs();
        MonitoringCoordinator.Decision current = d;

        for (MonitoringCoordinator.Effect effect : d.effects) {
            if (effect instanceof MonitoringCoordinator.ShowToast) {
                MonitoringCoordinator.ShowToast t = (MonitoringCoordinator.ShowToast) effect;
                Toast.makeText(this, t.text, t.longDuration ? Toast.LENGTH_LONG : Toast.LENGTH_SHORT).show();
                continue;
            }
            if (effect instanceof MonitoringCoordinator.RequestRuntimePermission) {
                if (permissionStateMachine != null) {
                    permissionStateMachine.requestWithUserConfirmation();
                }
                continue;
            }
            if (effect instanceof MonitoringCoordinator.MaybeShowGoToSettingsDialog) {
                if (permissionStateMachine != null) {
                    permissionStateMachine.showGoToSettingsDialogIfNeeded();
                }
                continue;
            }
            if (effect instanceof MonitoringCoordinator.RunPreflight) {
                MonitoringCoordinator.RunPreflight p = (MonitoringCoordinator.RunPreflight) effect;
                String preflight = runInputPreflight(p.wantCamera, p.wantMock);
                if (preflight != null && !preflight.isEmpty()) {
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(PREF_LAST_PREFLIGHT, preflight).apply();
                    AppLog.i("MainActivity", "preflight", preflight.replace('\n', ' '));
                    if (p.wantMock) {
                        AppLog.i("MockMode", "runMockPreflight", "Result: " + preflight.replace('\n', ' '));
                    }
                }
                current = monitoringCoordinator.onPreflightFinished(in, preflight);
                continue;
            }
            if (effect instanceof MonitoringCoordinator.InitEngine) {
                MonitoringCoordinator.InitEngine ie = (MonitoringCoordinator.InitEngine) effect;
                MonitoringCoordinator.Decision after = performInitEngineEffect(ie.wantMock);
                if (after != null) {
                    current = after;
                }
                continue;
            }
            if (effect instanceof MonitoringCoordinator.ScheduleInitEngine) {
                MonitoringCoordinator.ScheduleInitEngine se = (MonitoringCoordinator.ScheduleInitEngine) effect;
                handler.postDelayed(() -> {
                    if (isFinishing()) return;
                    initEngine();
                    if (!se.wantMock) {
                        dispatchMonitoringDecision(monitoringCoordinator.onEngineInitResult(snapshotMonitoringInputs(), engineInitialized, false));
                    }
                }, Math.max(0L, se.delayMs));
                continue;
            }
            if (effect instanceof MonitoringCoordinator.StartMonitoringFlow) {
                MonitoringCoordinator.StartMonitoringFlow s = (MonitoringCoordinator.StartMonitoringFlow) effect;
                MonitoringCoordinator.Decision after = performStartMonitoringFlowEffect(s);
                if (after != null) {
                    current = after;
                }
                continue;
            }
            if (effect instanceof MonitoringCoordinator.StopMonitoringFlow) {
                performStopMonitoringFlowEffect();
                continue;
            }
            if (effect instanceof MonitoringCoordinator.ScheduleStart) {
                MonitoringCoordinator.ScheduleStart ss = (MonitoringCoordinator.ScheduleStart) effect;
                handler.postDelayed(() -> dispatchMonitoringDecision(monitoringCoordinator.onStartRequested(snapshotMonitoringInputs(), null)), Math.max(0L, ss.delayMs));
                continue;
            }
        }
        return current;
    }

    private MonitoringCoordinator.Decision performInitEngineEffect(boolean wantMock) {
        if (wantMock) {
            initEngine();
            return null;
        }
        initEngine();
        return monitoringCoordinator.onEngineInitResult(snapshotMonitoringInputs(), engineInitialized, false);
    }

    private MonitoringCoordinator.Decision performStartMonitoringFlowEffect(@NonNull MonitoringCoordinator.StartMonitoringFlow s) {
        boolean wantCamera = s.wantCamera;
        boolean wantMock = s.wantMock;

        firstFrameReceived = false;
        updateSystemReadyUi("开始监控");

        if (wantCamera) {
            captureEverPushed = false;
            pushFailStreak = 0;
            lastPushOkRealtimeMs = 0L;

            nativeConfigureExternalInput(true, 0, 1);
            activeCaptureScheme = s.schemeOrNull == null ? CaptureScheme.CAMERA2 : s.schemeOrNull;
            activeCapture = (activeCaptureScheme == CaptureScheme.CAMERAX) ? cameraXCapture : camera2Capture;
            boolean started = activeCapture != null && activeCapture.start(String.valueOf(selectedCameraId));
            if (!started) {
                String capName = activeCapture == null ? "N/A" : activeCapture.name();
                nativeConfigureExternalInput(false, 0, 1);
                activeCapture = null;
                updateSystemReadyUi("采集启动失败");
                return monitoringCoordinator.onMonitoringStartFailed(snapshotMonitoringInputs(), "启动采集失败: " + capName);
            }
        }

        nativeStart();
        isRunning = true;
        if (frameHandler != null) {
            frameHandler.post(frameUpdater);
        }
        if (wantCamera) {
            startCaptureWatchdog();
        } else if (wantMock) {
            AppLog.i("MockMode", "performStartMonitoringFlowEffect",
                "Mock monitoring started, scheduling fallback check in " + MOCK_FALLBACK_TIMEOUT_MS + "ms");
            mockFallbackHandler.postDelayed(mockFallbackRunnable, MOCK_FALLBACK_TIMEOUT_MS);
        }
        startPreviewWatchdog();
        updateSystemReadyUi("已启动监控");

        return monitoringCoordinator.onMonitoringStarted(
                snapshotMonitoringInputs(),
                wantCamera,
                activeCapture != null ? activeCapture.name() : null,
                activeCaptureScheme
        );
    }

    private void performStopMonitoringFlowEffect() {
        isRunning = false;
        firstFrameReceived = false;
        mockFallbackHandler.removeCallbacks(mockFallbackRunnable);
        stopCaptureWatchdog();
        stopPreviewWatchdog();
        if (activeCapture != null) {
            try {
                activeCapture.stop();
            } catch (Exception ignored) {
            }
            activeCapture = null;
        }
        if (engineInitialized) {
            try {
                nativeConfigureExternalInput(false, 0, 1);
            } catch (Exception ignored) {
            }
        }
        if (frameHandler != null) {
            frameHandler.removeCallbacks(frameUpdater);
        }
        nativeStop();
        rtmpPusher.stop();
        updateSystemReadyUi("停止监控");
    }

    private void performMockFallback() {
        if (!isRunning || firstFrameReceived) return;
        AppLog.w("MockMode", "performMockFallback",
            "Mock source not responding after " + MOCK_FALLBACK_TIMEOUT_MS + "ms, falling back to camera");
        Toast.makeText(this, "Mock 源无响应，自动切换到相机模式", Toast.LENGTH_LONG).show();
        selectedCameraId = 0;
        mockFilePath = null;
        isSpinnerInitialized = true;
        refreshFlipFromPrefs(true);
        if (isRunning) {
            stopMonitoring();
            handler.postDelayed(this::startMonitoring, 500);
        } else {
            initEngine();
        }
    }

    private void handleCaptureAutoChanged(boolean enabled) {
        captureAutoEnabled = enabled;
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putBoolean(PREF_CAPTURE_AUTO, captureAutoEnabled)
                .apply();
        dispatchMonitoringDecision(monitoringCoordinator.onCapturePolicyChanged(
                snapshotMonitoringInputs(),
                captureAutoEnabled,
                preferredCaptureScheme,
                "切换自动模式"
        ));
    }

    private void handleCaptureSchemeChanged(@NonNull CaptureScheme scheme) {
        if (scheme == preferredCaptureScheme) return;
        preferredCaptureScheme = scheme;
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putString(PREF_CAPTURE_SCHEME, preferredCaptureScheme.name())
                .apply();
        dispatchMonitoringDecision(monitoringCoordinator.onCapturePolicyChanged(
                snapshotMonitoringInputs(),
                captureAutoEnabled,
                preferredCaptureScheme,
                "切换采集方案"
        ));
    }

    private void handleCameraSpinnerSelection(int position) {
        if (availableCameras == null || position < 0 || position >= availableCameras.size()) return;

        CameraInfo info = availableCameras.get(position);
        if ("-1".equals(info.id)) {
            AppLog.i("MainActivity", "onItemSelected", "Selected Mock Source");
            pickMediaFile();
            return;
        }
        if ("-2".equals(info.id)) {
            AppLog.i("MainActivity", "onItemSelected", "Selected System Camera Mock");
            dispatchTakePictureIntent();
            return;
        }

        try {
            int newId = Integer.parseInt(info.id);
            if (newId == selectedCameraId) return;

            selectedCameraId = newId;
            mockFilePath = null;
            refreshFlipFromPrefs(true);
            AppLog.i("MainActivity", "onItemSelected", "切换 Camera ID: " + selectedCameraId);

            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .edit()
                    .putInt(PREF_CAMERA_ID, selectedCameraId)
                    .apply();

            if (isRunning) {
                stopMonitoring();
                handler.postDelayed(() -> startMonitoring(), 500);
            } else {
                initEngine();
                dispatchMonitoringDecision(monitoringCoordinator.onEngineInitResult(snapshotMonitoringInputs(), engineInitialized, false));
            }
        } catch (NumberFormatException e) {
            AppLog.e("MainActivity", "onItemSelected", "Camera ID 非法: " + info.id, e);
        }
    }

    private void performHotRestartWithPrefsPersist() {
        SharedPreferences.Editor ed = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit();
        if (switchAccelOpenCL != null) ed.putBoolean(PREF_ACCEL_OPENCL, switchAccelOpenCL.isChecked());
        if (switchAccelMpp != null) ed.putBoolean(PREF_ACCEL_MPP, switchAccelMpp.isChecked());
        if (switchAccelQualcomm != null) ed.putBoolean(PREF_ACCEL_QUALCOMM, switchAccelQualcomm.isChecked());
        ed.putString(PREF_DETECTION_THROTTLE_MODE, normalizeInferenceThrottleMode(detectionThrottleMode));
        ed.putInt(PREF_DETECTION_INTERVAL_MS, clampDetectionIntervalMs(detectionIntervalMs));
        ed.putString(PREF_INFERENCE_THROTTLE_MODE, normalizeInferenceThrottleMode(inferenceThrottleMode));
        ed.putInt(PREF_INFERENCE_INTERVAL_MS, clampInferenceIntervalMs(inferenceIntervalMs));
        ed.apply();
        persistInferenceThrottleIni();
        performHotRestart();
    }

    private void loadInferenceThrottleSettings(SharedPreferences prefs) {
        String detMode = null;
        Integer detInterval = null;
        String recMode = null;
        Integer recInterval = null;
        if (prefs != null) {
            detMode = prefs.getString(PREF_DETECTION_THROTTLE_MODE, null);
            if (prefs.contains(PREF_DETECTION_INTERVAL_MS)) {
                detInterval = prefs.getInt(PREF_DETECTION_INTERVAL_MS, DETECTION_INTERVAL_DEFAULT_MS);
            }
            recMode = prefs.getString(PREF_INFERENCE_THROTTLE_MODE, null);
            if (prefs.contains(PREF_INFERENCE_INTERVAL_MS)) {
                recInterval = prefs.getInt(PREF_INFERENCE_INTERVAL_MS, INFERENCE_INTERVAL_DEFAULT_MS);
            }
        }

        if (detMode == null || detInterval == null || recMode == null || recInterval == null) {
            InferenceIni ini = readInferenceIniIfExists();
            if (detMode == null && ini.detMode != null) detMode = ini.detMode;
            if (detInterval == null && ini.detIntervalMs != null) detInterval = ini.detIntervalMs;
            if (recMode == null) {
                if (ini.recMode != null) recMode = ini.recMode;
                else if (ini.legacyMode != null) recMode = ini.legacyMode;
            }
            if (recInterval == null) {
                if (ini.recIntervalMs != null) recInterval = ini.recIntervalMs;
                else if (ini.legacyIntervalMs != null) recInterval = ini.legacyIntervalMs;
            }
        }

        detectionThrottleMode = normalizeInferenceThrottleMode(detMode);
        detectionIntervalMs = clampDetectionIntervalMs(detInterval == null ? DETECTION_INTERVAL_DEFAULT_MS : detInterval);
        effectiveDetectionIntervalMs = detectionIntervalMs;

        inferenceThrottleMode = normalizeInferenceThrottleMode(recMode);
        inferenceIntervalMs = clampInferenceIntervalMs(recInterval == null ? INFERENCE_INTERVAL_DEFAULT_MS : recInterval);
        effectiveInferenceIntervalMs = inferenceIntervalMs;
        inferenceAutoTuner.reset(inferenceIntervalMs);
    }

    private void bindInferenceThrottleControls() {
        if (rgDetectionThrottle != null) {
            rgDetectionThrottle.setOnCheckedChangeListener(null);
            String mode = normalizeInferenceThrottleMode(detectionThrottleMode);
            if ("off".equals(mode)) {
                rgDetectionThrottle.check(R.id.rb_detection_off);
            } else if ("manual".equals(mode)) {
                rgDetectionThrottle.check(R.id.rb_detection_manual);
            } else {
                rgDetectionThrottle.check(R.id.rb_detection_auto);
            }

            if (etDetectionIntervalMs != null) {
                etDetectionIntervalMs.setText(String.valueOf(clampDetectionIntervalMs(detectionIntervalMs)));
                etDetectionIntervalMs.setEnabled(!"off".equals(mode));
                etDetectionIntervalMs.setOnFocusChangeListener((v, hasFocus) -> {
                    if (hasFocus) return;
                    onDetectionThrottleIntervalEdited();
                });
            }

            rgDetectionThrottle.setOnCheckedChangeListener((group, checkedId) -> {
                if (checkedId == R.id.rb_detection_off) {
                    detectionThrottleMode = "off";
                } else if (checkedId == R.id.rb_detection_manual) {
                    detectionThrottleMode = "manual";
                } else {
                    detectionThrottleMode = "auto";
                }
                updateInferenceThrottleFromUi(true);
            });
        }

        if (rgInferenceThrottle != null) {
            rgInferenceThrottle.setOnCheckedChangeListener(null);

            String mode = normalizeInferenceThrottleMode(inferenceThrottleMode);
            if ("off".equals(mode)) {
                rgInferenceThrottle.check(R.id.rb_inference_off);
            } else if ("manual".equals(mode)) {
                rgInferenceThrottle.check(R.id.rb_inference_manual);
            } else {
                rgInferenceThrottle.check(R.id.rb_inference_auto);
            }

            if (etInferenceIntervalMs != null) {
                etInferenceIntervalMs.setText(String.valueOf(clampInferenceIntervalMs(inferenceIntervalMs)));
                etInferenceIntervalMs.setEnabled(!"off".equals(mode));
                etInferenceIntervalMs.setOnFocusChangeListener((v, hasFocus) -> {
                    if (hasFocus) return;
                    onInferenceThrottleIntervalEdited();
                });
            }

            rgInferenceThrottle.setOnCheckedChangeListener((group, checkedId) -> {
                if (checkedId == R.id.rb_inference_off) {
                    inferenceThrottleMode = "off";
                } else if (checkedId == R.id.rb_inference_manual) {
                    inferenceThrottleMode = "manual";
                } else {
                    inferenceThrottleMode = "auto";
                }
                updateInferenceThrottleFromUi(true);
            });
        }

        updateInferenceThrottleFromUi(false);
    }

    private void onDetectionThrottleIntervalEdited() {
        if (etDetectionIntervalMs == null) return;
        String raw = etDetectionIntervalMs.getText() == null ? "" : etDetectionIntervalMs.getText().toString();
        int v = DETECTION_INTERVAL_DEFAULT_MS;
        try {
            String t = raw == null ? "" : raw.trim();
            if (!t.isEmpty()) v = Integer.parseInt(t);
        } catch (Exception ignored) {
        }
        int clamped = clampDetectionIntervalMs(v);
        detectionIntervalMs = clamped;
        etDetectionIntervalMs.setText(String.valueOf(clamped));
        updateInferenceThrottleFromUi(true);
    }

    private void onInferenceThrottleIntervalEdited() {
        if (etInferenceIntervalMs == null) return;
        String raw = etInferenceIntervalMs.getText() == null ? "" : etInferenceIntervalMs.getText().toString();
        int v = INFERENCE_INTERVAL_DEFAULT_MS;
        try {
            String t = raw == null ? "" : raw.trim();
            if (!t.isEmpty()) v = Integer.parseInt(t);
        } catch (Exception ignored) {
        }
        int clamped = clampInferenceIntervalMs(v);
        inferenceIntervalMs = clamped;
        etInferenceIntervalMs.setText(String.valueOf(clamped));
        updateInferenceThrottleFromUi(true);
    }

    private void updateInferenceThrottleFromUi(boolean persist) {
        String detMode = normalizeInferenceThrottleMode(detectionThrottleMode);
        if (etDetectionIntervalMs != null) {
            etDetectionIntervalMs.setEnabled(!"off".equals(detMode));
        }
        int detBase = clampDetectionIntervalMs(detectionIntervalMs);
        effectiveDetectionIntervalMs = detBase;

        String recMode = normalizeInferenceThrottleMode(inferenceThrottleMode);
        if (etInferenceIntervalMs != null) {
            etInferenceIntervalMs.setEnabled(!"off".equals(recMode));
        }
        int recBase = clampInferenceIntervalMs(inferenceIntervalMs);
        if ("auto".equals(recMode)) {
            inferenceAutoTuner.reset(recBase);
            effectiveInferenceIntervalMs = inferenceAutoTuner.getIntervalMs();
        } else {
            effectiveInferenceIntervalMs = recBase;
        }

        if (persist) {
            SharedPreferences.Editor ed = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit();
            ed.putString(PREF_DETECTION_THROTTLE_MODE, detMode);
            ed.putInt(PREF_DETECTION_INTERVAL_MS, detBase);
            ed.putString(PREF_INFERENCE_THROTTLE_MODE, recMode);
            ed.putInt(PREF_INFERENCE_INTERVAL_MS, recBase);
            ed.apply();
            persistInferenceThrottleIni();
        }

        applyInferenceThrottleToNative();
        updateInferenceThrottleEffectiveUi();
    }

    private void updateInferenceThrottleEffectiveUi() {
        String detMode = normalizeInferenceThrottleMode(detectionThrottleMode);
        if (tvDetectionIntervalEffective != null) {
            if ("off".equals(detMode)) {
                tvDetectionIntervalEffective.setText("检测节流：关闭");
            } else {
                tvDetectionIntervalEffective.setText(String.format(Locale.US, "检测节流：%s %dms",
                        "manual".equals(detMode) ? "手动" : "自动",
                        clampDetectionIntervalMs(effectiveDetectionIntervalMs)));
            }
        }

        if (tvInferenceIntervalEffective != null) {
            String recMode = normalizeInferenceThrottleMode(inferenceThrottleMode);
            if ("off".equals(recMode)) {
                tvInferenceIntervalEffective.setText("识别节流：关闭");
            } else if ("manual".equals(recMode)) {
                tvInferenceIntervalEffective.setText(String.format(Locale.US, "识别节流：手动 %dms", clampInferenceIntervalMs(inferenceIntervalMs)));
            } else {
                tvInferenceIntervalEffective.setText(String.format(Locale.US, "识别节流：自动 生效 %dms", clampInferenceIntervalMs(effectiveInferenceIntervalMs)));
            }
        }
    }

    private void applyInferenceThrottleToNative() {
        if (!engineInitialized) return;
        String detMode = normalizeInferenceThrottleMode(detectionThrottleMode);
        int detInterval = clampDetectionIntervalMs(effectiveDetectionIntervalMs);
        try {
            NativeBridge.nativeSetDetectionThrottle(detMode, detInterval);
        } catch (Throwable t) {
            AppLog.e("MainActivity", "applyInferenceThrottleToNative", "nativeSetDetectionThrottle 失败", t);
        }

        String recMode = normalizeInferenceThrottleMode(inferenceThrottleMode);
        int recInterval = clampInferenceIntervalMs("auto".equals(recMode) ? effectiveInferenceIntervalMs : inferenceIntervalMs);
        try {
            NativeBridge.nativeSetRecognitionThrottle(recMode, recInterval);
        } catch (Throwable t) {
            AppLog.e("MainActivity", "applyInferenceThrottleToNative", "nativeSetRecognitionThrottle 失败", t);
        }

        if (!"auto".equals(recMode)) {
            if (inferenceAutoUpdater != null) handler.removeCallbacks(inferenceAutoUpdater);
            inferenceAutoUpdater = null;
            lastInferenceAutoHeartbeatMs = 0L;
            return;
        }

        if (inferenceAutoUpdater == null) {
            inferenceAutoUpdater = new Runnable() {
                @Override
                public void run() {
                    if (!isRunning || !engineInitialized) return;
                    String m = normalizeInferenceThrottleMode(inferenceThrottleMode);
                    if (!"auto".equals(m)) return;
                    long nowMs = SystemClock.elapsedRealtime();
                    StatsSnapshot s = StatsRepository.getInstance().getSnapshot();
                    boolean changed = inferenceAutoTuner.update(s);
                    int next = clampInferenceIntervalMs(inferenceAutoTuner.getIntervalMs());
                    if (changed && next != effectiveInferenceIntervalMs) {
                        int from = effectiveInferenceIntervalMs;
                        effectiveInferenceIntervalMs = next;
                        try {
                            NativeBridge.nativeSetRecognitionThrottle("auto", next);
                        } catch (Throwable ignored) {
                        }
                        updateInferenceThrottleEffectiveUi();
                        String cpu = (s == null || s.cpuPercent == null) ? "--" : String.format(Locale.US, "%.1f", s.cpuPercent);
                        String lat = (s == null || s.latencyMs == null) ? "--" : String.format(Locale.US, "%.0f", s.latencyMs);
                        String fps = (s == null || s.fps == null) ? "--" : String.format(Locale.US, "%.1f", s.fps);
                        String action = next >= from ? "step_up" : "step_down";
                        AppLog.i("MainActivity", "inferenceAutoTune",
                                String.format(Locale.US,
                                        "action=%s mode=auto base_ms=%d from_ms=%d to_ms=%d cpu=%s lat_ms=%s ui_fps=%s",
                                        action,
                                        clampInferenceIntervalMs(inferenceIntervalMs),
                                        from,
                                        next,
                                        cpu,
                                        lat,
                                        fps));
                    } else if (lastInferenceAutoHeartbeatMs > 0 && (nowMs - lastInferenceAutoHeartbeatMs) >= 30_000) {
                        String cpu = (s == null || s.cpuPercent == null) ? "--" : String.format(Locale.US, "%.1f", s.cpuPercent);
                        String lat = (s == null || s.latencyMs == null) ? "--" : String.format(Locale.US, "%.0f", s.latencyMs);
                        String fps = (s == null || s.fps == null) ? "--" : String.format(Locale.US, "%.1f", s.fps);
                        AppLog.i("MainActivity", "inferenceAutoTune",
                                String.format(Locale.US,
                                        "action=heartbeat mode=auto base_ms=%d effective_ms=%d cpu=%s lat_ms=%s ui_fps=%s",
                                        clampInferenceIntervalMs(inferenceIntervalMs),
                                        clampInferenceIntervalMs(effectiveInferenceIntervalMs),
                                        cpu,
                                        lat,
                                        fps));
                        lastInferenceAutoHeartbeatMs = nowMs;
                    }
                    handler.postDelayed(this, 1000);
                }
            };
        }
        handler.removeCallbacks(inferenceAutoUpdater);
        lastInferenceAutoHeartbeatMs = SystemClock.elapsedRealtime();
        handler.postDelayed(inferenceAutoUpdater, 800);
    }

    private void persistInferenceThrottleIni() {
        String baseDir = getAppStoragePath();
        if (baseDir == null || baseDir.trim().isEmpty()) return;
        File ini = new File(baseDir, INFERENCE_INI_FILENAME);

        String detMode = normalizeInferenceThrottleMode(detectionThrottleMode);
        int detInterval = clampDetectionIntervalMs(detectionIntervalMs);
        String recMode = normalizeInferenceThrottleMode(inferenceThrottleMode);
        int recInterval = clampInferenceIntervalMs(inferenceIntervalMs);

        List<String> lines = new ArrayList<>();
        if (ini.exists()) {
            try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(ini), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) {
                    lines.add(line);
                }
            } catch (Exception e) {
                AppLog.e("MainActivity", "persistInferenceThrottleIni", "读取 ini 失败: " + ini.getAbsolutePath(), e);
                lines.clear();
            }
        }

        List<String> out = new ArrayList<>();
        boolean hasSection = false;
        boolean inSection = false;
        boolean wroteLegacyMode = false;
        boolean wroteLegacyInterval = false;
        boolean wroteDetMode = false;
        boolean wroteDetInterval = false;
        boolean wroteRecMode = false;
        boolean wroteRecInterval = false;

        for (String line : lines) {
            String t = line == null ? "" : line.trim();
            if (t.startsWith("[") && t.endsWith("]")) {
                if (inSection) {
                    if (!wroteLegacyMode) out.add("throttle_mode=" + recMode);
                    if (!wroteLegacyInterval) out.add("interval_ms=" + recInterval);
                    if (!wroteDetMode) out.add("det_throttle_mode=" + detMode);
                    if (!wroteDetInterval) out.add("det_interval_ms=" + detInterval);
                    if (!wroteRecMode) out.add("rec_throttle_mode=" + recMode);
                    if (!wroteRecInterval) out.add("rec_interval_ms=" + recInterval);
                }
                inSection = "[inference]".equalsIgnoreCase(t);
                if (inSection) {
                    hasSection = true;
                    wroteLegacyMode = false;
                    wroteLegacyInterval = false;
                    wroteDetMode = false;
                    wroteDetInterval = false;
                    wroteRecMode = false;
                    wroteRecInterval = false;
                }
                out.add(line);
                continue;
            }

            if (inSection) {
                String lower = t.toLowerCase(Locale.US);
                if (lower.startsWith("throttle_mode=")) {
                    out.add("throttle_mode=" + recMode);
                    wroteLegacyMode = true;
                    continue;
                }
                if (lower.startsWith("interval_ms=")) {
                    out.add("interval_ms=" + recInterval);
                    wroteLegacyInterval = true;
                    continue;
                }
                if (lower.startsWith("det_throttle_mode=")) {
                    out.add("det_throttle_mode=" + detMode);
                    wroteDetMode = true;
                    continue;
                }
                if (lower.startsWith("det_interval_ms=")) {
                    out.add("det_interval_ms=" + detInterval);
                    wroteDetInterval = true;
                    continue;
                }
                if (lower.startsWith("rec_throttle_mode=")) {
                    out.add("rec_throttle_mode=" + recMode);
                    wroteRecMode = true;
                    continue;
                }
                if (lower.startsWith("rec_interval_ms=")) {
                    out.add("rec_interval_ms=" + recInterval);
                    wroteRecInterval = true;
                    continue;
                }
            }
            out.add(line);
        }

        if (inSection) {
            if (!wroteLegacyMode) out.add("throttle_mode=" + recMode);
            if (!wroteLegacyInterval) out.add("interval_ms=" + recInterval);
            if (!wroteDetMode) out.add("det_throttle_mode=" + detMode);
            if (!wroteDetInterval) out.add("det_interval_ms=" + detInterval);
            if (!wroteRecMode) out.add("rec_throttle_mode=" + recMode);
            if (!wroteRecInterval) out.add("rec_interval_ms=" + recInterval);
        }

        if (!hasSection) {
            if (!out.isEmpty() && !out.get(out.size() - 1).trim().isEmpty()) out.add("");
            out.add("[inference]");
            out.add("throttle_mode=" + recMode);
            out.add("interval_ms=" + recInterval);
            out.add("det_throttle_mode=" + detMode);
            out.add("det_interval_ms=" + detInterval);
            out.add("rec_throttle_mode=" + recMode);
            out.add("rec_interval_ms=" + recInterval);
        }

        try (BufferedWriter bw = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(ini, false), StandardCharsets.UTF_8))) {
            for (int i = 0; i < out.size(); i++) {
                bw.write(out.get(i) == null ? "" : out.get(i));
                bw.newLine();
            }
        } catch (Exception e) {
            AppLog.e("MainActivity", "persistInferenceThrottleIni", "写入 ini 失败: " + ini.getAbsolutePath(), e);
        }
    }

    private static final class InferenceIni {
        final String detMode;
        final Integer detIntervalMs;
        final String recMode;
        final Integer recIntervalMs;
        final String legacyMode;
        final Integer legacyIntervalMs;

        InferenceIni(String detMode,
                     Integer detIntervalMs,
                     String recMode,
                     Integer recIntervalMs,
                     String legacyMode,
                     Integer legacyIntervalMs) {
            this.detMode = detMode;
            this.detIntervalMs = detIntervalMs;
            this.recMode = recMode;
            this.recIntervalMs = recIntervalMs;
            this.legacyMode = legacyMode;
            this.legacyIntervalMs = legacyIntervalMs;
        }
    }

    private InferenceIni readInferenceIniIfExists() {
        String baseDir = getAppStoragePath();
        if (baseDir == null || baseDir.trim().isEmpty()) return new InferenceIni(null, null, null, null, null, null);
        File ini = new File(baseDir, INFERENCE_INI_FILENAME);
        if (!ini.exists()) return new InferenceIni(null, null, null, null, null, null);

        String detMode = null;
        Integer detInterval = null;
        String recMode = null;
        Integer recInterval = null;
        String legacyMode = null;
        Integer legacyInterval = null;
        boolean inSection = false;

        try (BufferedReader br = new BufferedReader(new InputStreamReader(new FileInputStream(ini), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                String t = line.trim();
                if (t.isEmpty() || t.startsWith(";") || t.startsWith("#")) continue;
                if (t.startsWith("[") && t.endsWith("]")) {
                    inSection = "[inference]".equalsIgnoreCase(t);
                    continue;
                }
                if (!inSection) continue;
                int eq = t.indexOf('=');
                if (eq <= 0) continue;
                String k = t.substring(0, eq).trim().toLowerCase(Locale.US);
                String v = t.substring(eq + 1).trim();
                if ("throttle_mode".equals(k)) {
                    legacyMode = v;
                } else if ("interval_ms".equals(k)) {
                    try {
                        legacyInterval = Integer.parseInt(v);
                    } catch (Exception ignored) {
                    }
                } else if ("det_throttle_mode".equals(k)) {
                    detMode = v;
                } else if ("det_interval_ms".equals(k)) {
                    try {
                        detInterval = Integer.parseInt(v);
                    } catch (Exception ignored) {
                    }
                } else if ("rec_throttle_mode".equals(k)) {
                    recMode = v;
                } else if ("rec_interval_ms".equals(k)) {
                    try {
                        recInterval = Integer.parseInt(v);
                    } catch (Exception ignored) {
                    }
                }
            }
        } catch (Exception e) {
            AppLog.e("MainActivity", "readInferenceIniIfExists", "读取 ini 失败: " + ini.getAbsolutePath(), e);
            return new InferenceIni(null, null, null, null, null, null);
        }

        return new InferenceIni(detMode, detInterval, recMode, recInterval, legacyMode, legacyInterval);
    }

    private static String normalizeInferenceThrottleMode(String raw) {
        String v = raw == null ? "" : raw.trim().toLowerCase(Locale.US);
        if ("0".equals(v) || "false".equals(v) || "off".equals(v) || "disable".equals(v) || "disabled".equals(v)) return "off";
        if ("1".equals(v) || "manual".equals(v) || "fixed".equals(v)) return "manual";
        if ("2".equals(v) || "auto".equals(v) || "adaptive".equals(v)) return "auto";
        return "auto";
    }

    private static int clampInferenceIntervalMs(int v) {
        if (v < INFERENCE_INTERVAL_MIN_MS) return INFERENCE_INTERVAL_MIN_MS;
        if (v > INFERENCE_INTERVAL_MAX_MS) return INFERENCE_INTERVAL_MAX_MS;
        return v;
    }

    private static int clampDetectionIntervalMs(int v) {
        if (v < DETECTION_INTERVAL_MIN_MS) return DETECTION_INTERVAL_MIN_MS;
        if (v > DETECTION_INTERVAL_MAX_MS) return DETECTION_INTERVAL_MAX_MS;
        return v;
    }

    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            String action = intent.getAction();
            if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                Toast.makeText(context, "USB Device Attached", Toast.LENGTH_SHORT).show();
                discoverCameras(); // Refresh list
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                Toast.makeText(context, "USB Device Detached", Toast.LENGTH_SHORT).show();
                discoverCameras(); // Refresh list
            }
        }
    };

    // Native methods
    public native String stringFromJNI();
    public native boolean nativeInit(int cameraId, String cascadePath, String storagePath);
    public native boolean nativeInitFile(String filePath, String cascadePath, String storagePath);
    public native void nativeStart();
    public native void nativeStop();
    public native void nativeRequestCancelInit();
    public native void nativeSetMode(int mode);
    public native void nativeSetFlip(boolean flipX, boolean flipY);
    public native boolean nativeGetFrame(Bitmap bitmap);
    public native void nativeSetPreviewSurface(Surface surface);
    public native boolean nativeRenderFrameToSurface();
    public native void nativeConfigureExternalInput(boolean enabled, int backpressureMode, int queueCapacity);
    public native boolean nativePushFrameYuv420888(ByteBuffer yBuffer,
                                                   int yRowStride,
                                                   ByteBuffer uBuffer,
                                                   int uRowStride,
                                                   int uPixelStride,
                                                   ByteBuffer vBuffer,
                                                   int vRowStride,
                                                   int vPixelStride,
                                                   int width,
                                                   int height,
                                                   long timestampNs,
                                                   int rotationDegrees,
                                                   boolean mirrored);
    public native boolean nativePushFrameYuv420888Bytes(byte[] yBytes,
                                                        int yRowStride,
                                                        byte[] uBytes,
                                                        int uRowStride,
                                                        int uPixelStride,
                                                        byte[] vBytes,
                                                        int vRowStride,
                                                        int vPixelStride,
                                                        int width,
                                                        int height,
                                                        long timestampNs,
                                                        int rotationDegrees,
                                                        boolean mirrored);

    private static final int REQUEST_CODE_PICK_MEDIA = 2001;
    private static final int REQUEST_CODE_TAKE_PHOTO = 1003;
    private String mockFilePath = null;
    private String currentPhotoPath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AppLog.enter("MainActivity", "onCreate");
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        try {
            WindowMetrics m = WindowMetricsCalculator.getOrCreate().computeCurrentWindowMetrics(this);
            AppLog.i("MainActivity", "onCreate", "WindowMetrics=" + m.getBounds().toShortString());
        } catch (Throwable ignored) {
        }

        DeviceProfile profile = DeviceRuntime.get().getProfile();
        DeviceClass deviceClass = DeviceRuntime.get().getDeviceClass();

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        selectedCameraId = prefs.getInt(PREF_CAMERA_ID, 0);
        recognitionEventsVisible = prefs.getBoolean(PREF_EVENTS_VISIBLE, true);
        captureAutoEnabled = prefs.getBoolean(PREF_CAPTURE_AUTO, true);
        String schemeRaw = prefs.getString(PREF_CAPTURE_SCHEME, CaptureScheme.CAMERA2.name());
        preferredCaptureScheme = parseCaptureScheme(schemeRaw, CaptureScheme.CAMERA2);
        loadInferenceThrottleSettings(prefs);

        cameraAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        cameraAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);

        bindMainScreen(true);

        if (switchAccelOpenCL != null) {
            switchAccelOpenCL.setChecked(prefs.getBoolean(PREF_ACCEL_OPENCL, false));
        }
        if (switchAccelMpp != null) {
            switchAccelMpp.setChecked(prefs.getBoolean(PREF_ACCEL_MPP, false));
        }
        if (switchAccelQualcomm != null) {
            switchAccelQualcomm.setChecked(prefs.getBoolean(PREF_ACCEL_QUALCOMM, false));
        }

        frameThread = new HandlerThread("FrameWorker");
        frameThread.start();
        frameHandler = new Handler(frameThread.getLooper());
        ExternalFrameSink sink = this::pushExternalYuv420888;
        camera2Capture = new Camera2CaptureController(this, sink, this, () -> getDeviceRotationDegrees());
        cameraXCapture = new CameraXCaptureController(this, this, sink, this, () -> getDeviceRotationDegrees());

        if (previewSurface == null) {
            frameBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888);
            backBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888);
            monitorView.setImageBitmap(frameBitmap);
        } else {
            try {
                previewSurface.getHolder().addCallback(previewSurfaceCallback);
            } catch (Throwable ignored) {
            }
        }
        monitorGestureDetector = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                toggleFullscreen();
                return true;
            }
        });
        if (previewSurface != null) {
            previewSurface.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));
        }
        if (monitorView != null) {
            monitorView.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));
        }

        rvRecognitionEvents.setLayoutManager(new LinearLayoutManager(this));
        recognitionEventAdapter = new RecognitionEventAdapter();
        rvRecognitionEvents.setAdapter(recognitionEventAdapter);
        setRecognitionPanelVisible(recognitionEventsVisible, false);

        // Register USB Receiver
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(usbReceiver, filter);

        // Discover Cameras
        discoverCameras();

        permissionStateMachine = new PermissionStateMachine(this, this::onPermissionStateChanged);
        permissionStateMachine.evaluate();
        if (!permissionStateMachine.isRuntimeGranted()) {
            permissionStateMachine.requestWithUserConfirmation();
        }

        // Frame Update Loop
        frameUpdater = new Runnable() {
            @Override
            public void run() {
                if (!isRunning) return;
                boolean ok;
                if (previewSurfaceReady) {
                    ok = nativeRenderFrameToSurface();
                } else {
                    Bitmap target;
                    synchronized (bitmapSwapLock) {
                        target = backBitmap;
                    }
                    ok = nativeGetFrame(target);
                }
                if (!ok) {
                    previewRenderFailStreak = Math.min(10_000, previewRenderFailStreak + 1);
                    if (frameHandler != null) {
                        frameHandler.postDelayed(this, 33);
                    }
                    return;
                }
                previewRenderFailStreak = 0;
                lastPreviewRenderOkRealtimeMs = SystemClock.elapsedRealtime();
                StatsRepository.getInstance().reportRenderTimeNs(System.nanoTime());

                handler.post(() -> {
                    if (!isRunning) return;
                    if (!previewSurfaceReady) {
                        synchronized (bitmapSwapLock) {
                            Bitmap tmp = frameBitmap;
                            frameBitmap = backBitmap;
                            backBitmap = tmp;
                        }
                        if (monitorView != null && frameBitmap != null) {
                            monitorView.setImageBitmap(frameBitmap);
                        }
                    }
                    if (!firstFrameReceived) {
                        firstFrameReceived = true;
                        mockFallbackHandler.removeCallbacks(mockFallbackRunnable);
                        if (frameBitmap != null) {
                            applyMonitorLayoutRule(frameBitmap.getWidth(), frameBitmap.getHeight());
                        } else {
                            applyMonitorLayoutRule(640, 480);
                        }
                        updateSystemReadyUi("首帧到达");
                        dispatchMonitoringDecision(monitoringCoordinator.onFirstFrameArrived(snapshotMonitoringInputs()));
                    }
                    if (frameHandler != null) {
                        frameHandler.postDelayed(frameUpdater, 16);
                    }
                });
            }
        };

        statsUpdater = new Runnable() {
            @Override
            public void run() {
                StatsSnapshot s = StatsRepository.getInstance().getSnapshot();
                String fpsText = (s.fps == null) ? "--" : String.format(Locale.US, "%.1f", s.fps);
                String capText = (s.captureFps == null) ? "--" : String.format(Locale.US, "%.1f", s.captureFps);
                tvFps.setText(String.format(Locale.US, "FPS: %s | CAP: %s", fpsText, capText));
                if (s.cpuPercent == null) {
                    tvCpu.setText("CPU: --");
                } else {
                    tvCpu.setText(String.format(Locale.US, "CPU: %.1f%%", s.cpuPercent));
                }
                if (s.memPssMb == null) {
                    tvMemory.setText("MEM: --");
                } else {
                    tvMemory.setText(String.format(Locale.US, "MEM: %.0fMB", s.memPssMb));
                }
                if (tvLatency != null) {
                    if (s.latencyMs == null) {
                        tvLatency.setText("LAT: --ms");
                    } else {
                        tvLatency.setText(String.format(Locale.US, "LAT: %.0fms", s.latencyMs));
                    }
                }
                handler.postDelayed(this, 500);
            }
        };
        handler.post(statsUpdater);
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        View root = getWindow().getDecorView();
        root.animate().alpha(0f).setDuration(150).setInterpolator(new LinearInterpolator()).withEndAction(() -> {
            setContentView(R.layout.activity_main);
            rebindViewsAfterConfigChange();
            View root2 = getWindow().getDecorView();
            root2.setAlpha(0f);
            root2.animate().alpha(1f).setDuration(150).setInterpolator(new LinearInterpolator()).start();
        }).start();
    }

    private void rebindViewsAfterConfigChange() {
        bindMainScreen(true);

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        if (switchAccelOpenCL != null) {
            switchAccelOpenCL.setChecked(prefs.getBoolean(PREF_ACCEL_OPENCL, false));
        }
        if (switchAccelMpp != null) {
            switchAccelMpp.setChecked(prefs.getBoolean(PREF_ACCEL_MPP, false));
        }
        if (switchAccelQualcomm != null) {
            switchAccelQualcomm.setChecked(prefs.getBoolean(PREF_ACCEL_QUALCOMM, false));
        }

        if (frameBitmap != null) {
            monitorView.setImageBitmap(frameBitmap);
        }
        if (previewSurface != null) {
            try {
                previewSurface.getHolder().removeCallback(previewSurfaceCallback);
            } catch (Throwable ignored) {
            }
            try {
                previewSurface.getHolder().addCallback(previewSurfaceCallback);
            } catch (Throwable ignored) {
            }
        }
        monitorGestureDetector = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                toggleFullscreen();
                return true;
            }
        });
        if (previewSurface != null) {
            previewSurface.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));
        }
        monitorView.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));

        if (rvRecognitionEvents != null) {
            rvRecognitionEvents.setLayoutManager(new LinearLayoutManager(this));
            if (recognitionEventAdapter != null) {
                rvRecognitionEvents.setAdapter(recognitionEventAdapter);
            }
        }
        setRecognitionPanelVisible(recognitionEventsVisible, false);

        if (isFullscreen) {
            if (recognitionTitle != null) recognitionTitle.setVisibility(View.GONE);
            if (panelSettings != null) panelSettings.setVisibility(View.GONE);
            if (panelRecognitionEvents != null) panelRecognitionEvents.setVisibility(View.GONE);
            if (videoWrapper != null) {
                FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) videoWrapper.getLayoutParams();
                lp.width = ViewGroup.LayoutParams.MATCH_PARENT;
                lp.height = ViewGroup.LayoutParams.MATCH_PARENT;
                videoWrapper.setLayoutParams(lp);
            }
            if (monitorView != null) monitorView.setScaleType(ImageView.ScaleType.CENTER_CROP);
        } else {
            if (recognitionTitle != null) recognitionTitle.setVisibility(View.VISIBLE);
            setRecognitionPanelVisible(recognitionEventsVisible, false);
            if (monitorView != null) monitorView.setScaleType(ImageView.ScaleType.FIT_CENTER);
            if (videoWrapper != null && frameBitmap != null) {
                videoWrapper.post(() -> applyMonitorLayoutRule(frameBitmap.getWidth(), frameBitmap.getHeight()));
            }
        }

        discoverCameras();
    }

    private void pickMediaFile() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        String[] mimetypes = {"image/*", "video/*"};
        intent.putExtra(Intent.EXTRA_MIME_TYPES, mimetypes);
        startActivityForResult(intent, REQUEST_CODE_PICK_MEDIA);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_CODE_PICK_MEDIA && resultCode == RESULT_OK) {
            if (data != null) {
                Uri uri = data.getData();
                if (uri != null) {
                    handleMockFileSelection(uri);
                }
            }
        } else if (requestCode == REQUEST_CODE_TAKE_PHOTO && resultCode == RESULT_OK) {
             handleSystemCameraResult();
        } else if (requestCode == 1002) {
            syncOverlaySwitchState();
        }
    }

    private void dispatchTakePictureIntent() {
        Intent takePictureIntent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        if (takePictureIntent.resolveActivity(getPackageManager()) != null) {
            File photoFile = null;
            try {
                photoFile = createImageFile();
            } catch (IOException ex) {
                Toast.makeText(this, "Error creating file", Toast.LENGTH_SHORT).show();
                AppLog.e("MainActivity", "dispatchTakePictureIntent", "Create file failed", ex);
            }
            if (photoFile != null) {
                Uri photoURI = FileProvider.getUriForFile(this,
                        BuildConfig.APPLICATION_ID + ".fileprovider",
                        photoFile);
                takePictureIntent.putExtra(MediaStore.EXTRA_OUTPUT, photoURI);
                startActivityForResult(takePictureIntent, REQUEST_CODE_TAKE_PHOTO);
            }
        } else {
             Toast.makeText(this, "No Camera App Found", Toast.LENGTH_SHORT).show();
        }
    }

    private File createImageFile() throws IOException {
        String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        String imageFileName = "mock_capture_" + timeStamp + "_";
        File storageDir = getCacheDir();
        File image = File.createTempFile(
            imageFileName,  /* prefix */
            ".jpg",         /* suffix */
            storageDir      /* directory */
        );
        currentPhotoPath = image.getAbsolutePath();
        return image;
    }

    private void handleSystemCameraResult() {
        if (currentPhotoPath == null) return;
        
        mockFilePath = currentPhotoPath;
        selectedCameraId = -1; // Mock Flag
        
        AppLog.i("MainActivity", "handleSystemCameraResult", "Photo captured: " + mockFilePath);
        Toast.makeText(this, "Photo Captured", Toast.LENGTH_SHORT).show();
        
        if (isRunning) {
            stopMonitoring();
            handler.postDelayed(MainActivity.this::startMonitoring, 500);
        } else {
            initEngine();
        }
    }

    private void handleMockFileSelection(Uri uri) {
        if (uri == null) return;

        String direct = null;
        try {
            if ("file".equalsIgnoreCase(uri.getScheme())) {
                java.io.File f = new java.io.File(uri.getPath());
                if (f.exists() && f.isFile() && f.canRead()) {
                    direct = f.getAbsolutePath();
                }
            }
        } catch (Throwable ignored) {
        }

        if (direct != null && !direct.isEmpty()) {
            mockFilePath = direct;
            selectedCameraId = -1;
            isSpinnerInitialized = true;
            refreshFlipFromPrefs(true);
            if (isRunning) {
                stopMonitoring();
                handler.postDelayed(MainActivity.this::startMonitoring, 500);
            } else {
                initEngine();
            }
            return;
        }

        ProgressDialog progressDialog = new ProgressDialog(this);
        progressDialog.setTitle("加载 Mock 文件");
        progressDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        progressDialog.setIndeterminate(true);
        progressDialog.setCancelable(true);
        progressDialog.setMessage("准备读取…");
        progressDialog.show();

        final java.util.concurrent.atomic.AtomicBoolean cancelled = new java.util.concurrent.atomic.AtomicBoolean(false);
        progressDialog.setOnCancelListener(d -> cancelled.set(true));

        new Thread(() -> {
            try {
                String displayName = "mock_source";
                long totalBytes = -1L;
                try {
                    Cursor c = getContentResolver().query(uri, null, null, null, null);
                    if (c != null) {
                        try {
                            if (c.moveToFirst()) {
                                int nameIndex = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                                if (nameIndex >= 0) {
                                    String n = c.getString(nameIndex);
                                    if (n != null && !n.trim().isEmpty()) displayName = n.trim();
                                }
                                int sizeIndex = c.getColumnIndex(OpenableColumns.SIZE);
                                if (sizeIndex >= 0) {
                                    long s = c.getLong(sizeIndex);
                                    if (s > 0) totalBytes = s;
                                }
                            }
                        } finally {
                            try { c.close(); } catch (Throwable ignored) {}
                        }
                    }
                } catch (Throwable ignored) {
                }

                String ext = "";
                try {
                    int dot = displayName.lastIndexOf('.');
                    if (dot >= 0 && dot < displayName.length() - 1) {
                        ext = displayName.substring(dot + 1);
                    }
                } catch (Throwable ignored) {
                }
                ext = sanitizeExtension(ext);

                // ---- Magic number pre-check ----
                String magicCheckErr = null;
                try {
                    java.io.InputStream magicIs = getContentResolver().openInputStream(uri);
                    if (magicIs != null) {
                        try {
                            byte[] magic = new byte[16];
                            int magicN = magicIs.read(magic);
                            String hexMagic = bytesToHex(magic, Math.max(0, magicN));
                            AppLog.i("MockMode", "handleMockFileSelection", "Magic bytes: " + hexMagic + " ext=" + ext);
                            if (!isValidMagicNumber(magic, magicN, ext)) {
                                magicCheckErr = "不支持的文件格式或文件已损坏（魔数校验失败）";
                                AppLog.w("MockMode", "handleMockFileSelection",
                                    "Magic check REJECTED: " + hexMagic + " ext=" + ext);
                            } else {
                                AppLog.i("MockMode", "handleMockFileSelection",
                                    "Magic check PASSED: " + hexMagic + " ext=" + ext);
                            }
                        } finally {
                            try { magicIs.close(); } catch (Throwable ignored) {}
                        }
                    }
                } catch (Throwable t) {
                    AppLog.w("MockMode", "handleMockFileSelection", "Magic read error: " + t.getMessage());
                }
                if (magicCheckErr != null) {
                    String finalErrMsg = magicCheckErr;
                    runOnUiThread(() -> {
                        try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                        Toast.makeText(MainActivity.this, finalErrMsg, Toast.LENGTH_LONG).show();
                    });
                    return;
                }

                java.io.File root = null;
                try {
                    root = getExternalCacheDir();
                } catch (Throwable ignored) {
                }
                if (root == null) root = getCacheDir();

                String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
                String saveName = "mock_source_" + timeStamp + (ext.isEmpty() ? ".dat" : ("." + ext));
                java.io.File outFile = new java.io.File(root, saveName);

                long copied = 0L;
                long startMs = SystemClock.elapsedRealtime();
                long lastUiMs = 0L;
                boolean ok = false;
                String err = null;

                AppLog.i("MockMode", "handleMockFileSelection",
                    "Copy start: name=" + saveName + " ext=" + ext +
                    " totalBytes=" + totalBytes + " dest=" + outFile.getAbsolutePath());

                java.io.InputStream is = null;
                java.io.BufferedInputStream bis = null;
                java.io.FileOutputStream fos = null;
                try {
                    is = getContentResolver().openInputStream(uri);
                    if (is == null) throw new IOException("无法打开输入流");
                    bis = new java.io.BufferedInputStream(is, 1024 * 1024);
                    fos = new java.io.FileOutputStream(outFile);

                    if (totalBytes > 0) {
                        final int maxKb = (int) Math.min(Integer.MAX_VALUE, Math.max(1L, totalBytes / 1024L));
                        int finalMaxKb = maxKb;
                        runOnUiThread(() -> {
                            progressDialog.setIndeterminate(false);
                            progressDialog.setMax(finalMaxKb);
                        });
                    }

                    byte[] buffer = new byte[1024 * 1024];
                    int n;
                    while ((n = bis.read(buffer)) != -1) {
                        if (cancelled.get()) {
                            err = "用户取消";
                            break;
                        }
                        fos.write(buffer, 0, n);
                        copied += n;

                        long nowMs = SystemClock.elapsedRealtime();
                        if (nowMs - lastUiMs >= 350) {
                            lastUiMs = nowMs;
                            long elapsedMs = Math.max(1L, nowMs - startMs);
                            double speed = (copied / 1024.0) / (elapsedMs / 1000.0);
                            String msg = "已复制 " + formatBytes(copied) +
                                    (totalBytes > 0 ? (" / " + formatBytes(totalBytes)) : "") +
                                    "  速度 " + String.format(Locale.US, "%.1f", speed) + " KB/s";
                            int progressKb = (int) Math.min(Integer.MAX_VALUE, Math.max(0L, copied / 1024L));
                            runOnUiThread(() -> {
                                try {
                                    progressDialog.setMessage(msg);
                                    if (!progressDialog.isIndeterminate()) {
                                        progressDialog.setProgress(progressKb);
                                    }
                                } catch (Throwable ignored) {
                                }
                            });
                        }
                    }

                    fos.flush();
                    if (!cancelled.get() && (err == null || err.isEmpty())) {
                        ok = true;
                    }
                } catch (Throwable t) {
                    err = t.getMessage();
                } finally {
                    try { if (fos != null) fos.close(); } catch (Throwable ignored) {}
                    try { if (bis != null) bis.close(); } catch (Throwable ignored) {}
                    try { if (is != null) is.close(); } catch (Throwable ignored) {}
                }

                long elapsedMs = Math.max(1L, SystemClock.elapsedRealtime() - startMs);

                boolean finalOk = ok;
                String finalErr = err;
                long finalCopied = copied;
                String finalDisplayName = displayName;
                String finalOutPath = outFile.getAbsolutePath();
                runOnUiThread(() -> {
                    try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                    if (!finalOk) {
                        try { if (outFile.exists()) outFile.delete(); } catch (Throwable ignored) {}
                        AppLog.e("MockMode", "handleMockFileSelection",
                            "Copy FAILED: err=" + finalErr + " copied=" + finalCopied + " elapsedMs=" + elapsedMs);
                        Toast.makeText(MainActivity.this, "加载失败: " + finalErr, Toast.LENGTH_LONG).show();
                        return;
                    }

                    AppLog.i("MockMode", "handleMockFileSelection",
                        "Copy COMPLETE: path=" + finalOutPath + " copied=" + finalCopied +
                        " elapsedMs=" + elapsedMs + " speedKBps=" + (finalCopied / 1024L * 1000L / elapsedMs));

                    mockFilePath = finalOutPath;
                    selectedCameraId = -1;
                    isSpinnerInitialized = true;
                    refreshFlipFromPrefs(true);

                    AppLog.i("MainActivity", "handleMockFileSelection",
                            "Mock file ready: " + finalOutPath + " copied=" + finalCopied + " name=" + finalDisplayName);
                    Toast.makeText(MainActivity.this, "Mock 源已就绪: " + finalDisplayName, Toast.LENGTH_SHORT).show();

                    if (isRunning) {
                        stopMonitoring();
                        handler.postDelayed(MainActivity.this::startMonitoring, 500);
                    } else {
                        initEngine();
                    }
                });
            } catch (Throwable mainThreadCrash) {
                AppLog.e("MainActivity", "handleMockFileSelection", "未捕获的 Mock 文件加载异常", mainThreadCrash);
                runOnUiThread(() -> {
                    try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                    Toast.makeText(MainActivity.this, "加载文件遇到严重异常: " + mainThreadCrash.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    private static String sanitizeExtension(String ext) {
        if (ext == null) return "";
        String s = ext.trim();
        if (s.isEmpty()) return "";
        if (s.length() > 10) s = s.substring(0, 10);
        if (!s.matches("^[A-Za-z0-9]+$")) return "";
        return s.toLowerCase(Locale.ROOT);
    }

    private static String bytesToHex(byte[] bytes, int len) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < len && i < bytes.length; i++) {
            sb.append(String.format(Locale.US, "%02X", bytes[i] & 0xFF));
            if ((i + 1) % 4 == 0 && i < len - 1) sb.append(' ');
        }
        return sb.toString();
    }

    private static boolean isValidMagicNumber(byte[] magic, int len, String ext) {
        if (len < 2) return false;
        // Common file format magic signatures
        // JPEG: FF D8 FF
        if (len >= 3 && (magic[0] & 0xFF) == 0xFF && (magic[1] & 0xFF) == 0xD8 && (magic[2] & 0xFF) == 0xFF) return true;
        // PNG: 89 50 4E 47
        if (len >= 4 && (magic[0] & 0xFF) == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return true;
        // BMP: 42 4D
        if (len >= 2 && magic[0] == 0x42 && magic[1] == 0x4D) return true;
        // WEBP (RIFF....WEBP): 52 49 46 46 xx xx xx xx 57 45 42 50
        if (len >= 12 && magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46
            && magic[8] == 0x57 && magic[9] == 0x45 && magic[10] == 0x42 && magic[11] == 0x50) return true;
        // MP4/M4V (ftyp): xx xx xx xx 66 74 79 70
        if (len >= 8 && magic[4] == 0x66 && magic[5] == 0x74 && magic[6] == 0x79 && magic[7] == 0x70) return true;
        // AVI (RIFF....AVI ): 52 49 46 46 xx xx xx xx 41 56 49 20
        if (len >= 12 && magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46
            && magic[8] == 0x41 && magic[9] == 0x56 && magic[10] == 0x49 && magic[11] == 0x20) return true;
        // Fallback: if the extension is known but magic unknown, accept anyway (allow extension-only check)
        String knownExts = "jpg,jpeg,png,bmp,webp,mp4,m4v,avi,mov,3gp,mkv,wmv,flv,dat";
        return knownExts.contains(ext);
    }

    private static String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        double b = bytes;
        if (b < 1024) return String.format(Locale.US, "%.0fB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(Locale.US, "%.1fKB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(Locale.US, "%.1fMB", b);
        b /= 1024.0;
        return String.format(Locale.US, "%.2fGB", b);
    }

    private void startMonitoring() {
        AppLog.enter("MainActivity", "startMonitoring");
        dispatchMonitoringDecision(monitoringCoordinator.onStartRequested(snapshotMonitoringInputs(), null));
    }

    private String runInputPreflight(boolean wantCamera, boolean wantMock) {
        if (wantCamera) {
            return runCameraPreflight();
        }
        if (wantMock) {
            return runMockPreflight();
        }
        return "";
    }

    private String runCameraPreflight() {
        try {
            CameraManager mgr = (CameraManager) getSystemService(CAMERA_SERVICE);
            if (mgr == null) return "相机预检失败：CameraManager 不可用";
            CameraCharacteristics chars = mgr.getCameraCharacteristics(String.valueOf(selectedCameraId));
            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes = map != null ? map.getOutputSizes(ImageFormat.YUV_420_888) : null;
            Range<Integer>[] fpsRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);

            Size chosen = chooseBestSizeNoMoreThan1080p(sizes);
            Range<Integer> fps = chooseBestFpsNoMoreThan60(fpsRanges);

            StringBuilder sb = new StringBuilder();
            sb.append("相机预检: Cam ").append(selectedCameraId);
            sb.append("\n输出: ").append(chosen == null ? "未知" : (chosen.getWidth() + "x" + chosen.getHeight()));
            sb.append("\n帧率: ").append(fps == null ? "未知" : (fps.getLower() + "-" + fps.getUpper()));
            sb.append("\n策略: 输入≤1080p60；推理固定640x480");
            return sb.toString();
        } catch (Throwable t) {
            return "相机预检失败: " + t.getMessage();
        }
    }

    private String runMockPreflight() {
        try {
            String path = mockFilePath;
            if (path == null || path.isEmpty()) return "Mock预检失败：未选择输入";
            if (path.startsWith("http://") || path.startsWith("https://")) {
                return "Mock预检: URL 源（无法预读元数据，运行时将自动降档到≤1080p60，推理640x480）";
            }

            File f = new File(path);
            if (!f.exists()) return "Mock预检: 文件不存在";

            Integer w = null;
            Integer h = null;
            Integer rot = null;
            Integer bitrate = null;
            Float fps = null;

            try {
                MediaMetadataRetriever mmr = new MediaMetadataRetriever();
                mmr.setDataSource(f.getAbsolutePath());
                String sw = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
                String sh = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
                String sr = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
                String sb = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_BITRATE);
                String sfps = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_CAPTURE_FRAMERATE);
                if (sw != null) w = Integer.parseInt(sw);
                if (sh != null) h = Integer.parseInt(sh);
                if (sr != null) rot = Integer.parseInt(sr);
                if (sb != null) bitrate = Integer.parseInt(sb);
                if (sfps != null) fps = Float.parseFloat(sfps);
                mmr.release();
            } catch (Throwable t) {
                AppLog.w("MockMode", "runMockPreflight", "MediaMetadataRetriever failed: " + t.getMessage());
            }

            if (w == null || h == null) {
                try {
                    BitmapFactory.Options opt = new BitmapFactory.Options();
                    opt.inJustDecodeBounds = true;
                    BitmapFactory.decodeFile(f.getAbsolutePath(), opt);
                    if (opt.outWidth > 0 && opt.outHeight > 0) {
                        w = opt.outWidth;
                        h = opt.outHeight;
                    }
                } catch (Throwable t) {
                    AppLog.w("MockMode", "runMockPreflight", "BitmapFactory decodeBounds failed: " + t.getMessage());
                }
            }

            boolean oversize = (w != null && h != null) && (Math.max(w, h) > 1920 || Math.min(w, h) > 1080);
            boolean overFps = fps != null && fps > 60.5f;

            StringBuilder sb = new StringBuilder();
            sb.append("Mock预检: ").append(f.getName());
            sb.append("\n尺寸: ").append(w == null ? "未知" : (w + "x" + h));
            if (rot != null) sb.append(" rot=").append(rot);
            if (fps != null) sb.append("\n帧率: ").append(String.format(Locale.US, "%.2f", fps));
            if (bitrate != null) sb.append("\n码率: ").append(bitrate);
            if (oversize || overFps) {
                sb.append("\n提示: 输入超规格，将自动降档到≤1080p60，推理640x480");
            } else {
                sb.append("\n策略: 输入≤1080p60；推理固定640x480");
            }
            return sb.toString();
        } catch (Throwable t) {
            return "Mock预检失败: " + t.getMessage();
        }
    }

    private static Size chooseBestSizeNoMoreThan1080p(Size[] sizes) {
        if (sizes == null || sizes.length == 0) return null;
        Size best = null;
        long bestArea = -1;
        for (Size s : sizes) {
            if (s == null) continue;
            int w = s.getWidth();
            int h = s.getHeight();
            boolean ok = (w <= 1920 && h <= 1080) || (w <= 1080 && h <= 1920);
            if (!ok) continue;
            long area = (long) w * (long) h;
            if (area > bestArea) {
                bestArea = area;
                best = s;
            }
        }
        if (best != null) return best;
        for (Size s : sizes) {
            if (s == null) continue;
            int w = s.getWidth();
            int h = s.getHeight();
            long area = (long) w * (long) h;
            if (area > bestArea) {
                bestArea = area;
                best = s;
            }
        }
        return best;
    }

    private static Range<Integer> chooseBestFpsNoMoreThan60(Range<Integer>[] ranges) {
        if (ranges == null || ranges.length == 0) return null;
        Range<Integer> best = null;
        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            Integer lower = r.getLower();
            Integer upper = r.getUpper();
            if (lower == null || upper == null) continue;
            if (upper > 60) continue;
            if (best == null) {
                best = r;
                continue;
            }
            int bestUpper = best.getUpper();
            int bestLower = best.getLower();
            if (upper > bestUpper) {
                best = r;
            } else if (upper == bestUpper && lower < bestLower) {
                best = r;
            }
        }
        if (best != null) return best;
        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            Integer upper = r.getUpper();
            if (upper != null && upper <= 60) return r;
        }
        return ranges[0];
    }

    private void handleOverlayToggle(boolean enabled) {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_OVERLAY_ENABLED, enabled).apply();
        if (enabled) {
            boolean granted = Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(this);
            if (granted) {
                startService(new Intent(this, StatusService.class));
            } else {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_OVERLAY_ENABLED, false).apply();
                if (Build.VERSION.SDK_INT < 23) {
                    Toast.makeText(this, "当前系统版本不支持悬浮窗权限设置入口", Toast.LENGTH_SHORT).show();
                    setOverlaySwitchCheckedSilently(false);
                    return;
                }
                Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + getPackageName()));
                new AlertDialog.Builder(this)
                        .setTitle("需要悬浮窗权限")
                        .setMessage("开启悬浮窗将显示 FPS/CPU/MEM 指标，需要在系统设置授予“在其他应用上层显示”。")
                        .setPositiveButton("打开设置", (d, w) -> startActivityForResult(intent, 1002))
                        .setNegativeButton("取消", null)
                        .show();
                setOverlaySwitchCheckedSilently(false);
                return;
            }
        } else {
            stopService(new Intent(this, StatusService.class));
        }
        handler.postDelayed(this::syncOverlaySwitchState, 200);
    }

    private void syncOverlaySwitchState() {
        if (switchOverlay == null) return;
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        boolean desired = prefs.getBoolean(PREF_OVERLAY_ENABLED, false);
        boolean granted = Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(this);
        boolean running = isServiceRunning(StatusService.class);

        if (!running && prefs.getBoolean(PREF_OVERLAY_RUNNING, false)) {
            prefs.edit().putBoolean(PREF_OVERLAY_RUNNING, false).apply();
        }
        if (running && !desired) {
            desired = true;
            prefs.edit().putBoolean(PREF_OVERLAY_ENABLED, true).apply();
        }

        if (!granted) {
            if (running) {
                stopService(new Intent(this, StatusService.class));
                running = false;
            }
            if (desired) {
                prefs.edit().putBoolean(PREF_OVERLAY_ENABLED, false).apply();
                desired = false;
            }
        } else {
            if (desired && !running) {
                startService(new Intent(this, StatusService.class));
                handler.postDelayed(this::syncOverlaySwitchState, 200);
            } else if (!desired && running) {
                stopService(new Intent(this, StatusService.class));
                running = false;
            }
        }

        setOverlaySwitchCheckedSilently(running);
    }

    private void setOverlaySwitchCheckedSilently(boolean checked) {
        if (switchOverlay == null) return;
        switchOverlay.setOnCheckedChangeListener(null);
        switchOverlay.setChecked(checked);
        switchOverlay.setOnCheckedChangeListener(overlaySwitchListener);
    }

    private boolean isServiceRunning(Class<?> serviceClass) {
        try {
            ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) return false;
            List<ActivityManager.RunningServiceInfo> services = am.getRunningServices(Integer.MAX_VALUE);
            if (services == null) return false;
            String target = serviceClass.getName();
            for (ActivityManager.RunningServiceInfo s : services) {
                ComponentName cn = s.service;
                if (cn != null && target.equals(cn.getClassName())) {
                    return true;
                }
            }
        } catch (Throwable ignored) {
        }
        return false;
    }

    private void showExitDialog() {
        new AlertDialog.Builder(this)
                .setTitle("退出")
                .setMessage("请选择退出方式：\n\n安全退出（推荐）：释放资源并从最近任务移除，但系统不保证立刻杀死进程。\n强退（不推荐）：在安全退出基础上强制结束进程，可能影响未完成的后台工作。")
                .setPositiveButton("安全退出(推荐)", (d, w) -> performExit(false))
                .setNeutralButton("强退(不推荐)", (d, w) -> {
                    new AlertDialog.Builder(this)
                            .setTitle("强退确认")
                            .setMessage("强退会在清理资源后强制结束进程。\n\n可能风险：\n- 未写完的日志/文件被截断\n- 正在执行的后台任务被中断\n\n仍要继续吗？")
                            .setPositiveButton("继续强退", (d2, w2) -> performExit(true))
                            .setNegativeButton("取消", null)
                            .show();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void performExit(boolean forceQuit) {
        try {
            if (isRunning) {
                stopMonitoring();
            }
        } catch (Throwable ignored) {
        }
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_OVERLAY_ENABLED, false).apply();
            stopService(new Intent(this, StatusService.class));
        } catch (Throwable ignored) {
        }
        try {
            finishAndRemoveTask();
        } catch (Throwable ignored) {
            try {
                finishAffinity();
            } catch (Throwable ignored2) {
                finish();
            }
        }
        if (forceQuit) {
            try {
                Handler h = new Handler(Looper.getMainLooper());
                h.postDelayed(() -> {
                    try {
                        android.os.Process.killProcess(android.os.Process.myPid());
                    } catch (Throwable ignored) {
                    }
                    try {
                        System.exit(0);
                    } catch (Throwable ignored) {
                    }
                }, 350);
            } catch (Throwable ignored) {
            }
        }
    }

    private void stopMonitoring() {
        AppLog.enter("MainActivity", "stopMonitoring");
        dispatchMonitoringDecision(monitoringCoordinator.onStopRequested(snapshotMonitoringInputs(), null));
    }

    private void initEngine() {
        AppLog.enter("MainActivity", "initEngine");

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        try {
            android.system.Os.setenv("RK_USE_OPENCL", prefs.getBoolean(PREF_ACCEL_OPENCL, false) ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_MPP", prefs.getBoolean(PREF_ACCEL_MPP, false) ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_QUALCOMM", prefs.getBoolean(PREF_ACCEL_QUALCOMM, false) ? "1" : "0", true);
        } catch (Exception e) {
            AppLog.e("MainActivity", "initEngine", "Failed to setenv for acceleration", e);
        }

        boolean wantCamera = (selectedCameraId >= 0 && mockFilePath == null);
        boolean wantMock = (selectedCameraId == -1 && mockFilePath != null);
        if (wantCamera && permissionStateMachine != null && !permissionStateMachine.isRuntimeGranted()) {
            engineInitialized = false;
            firstFrameReceived = false;
            updateSystemReadyUi("权限不足，禁止初始化引擎");
            return;
        }

        String cascadePath = copyAssetToCache("lbpcascade_frontalface.xml");
        String storagePath = getAppStoragePath();
        
        if (wantMock) {
            ProgressDialog dialog = new ProgressDialog(this);
            dialog.setMessage("正在初始化 Mock 源…");
            dialog.setCancelable(true);
            dialog.show();
            cancelInitMock = false;
            dialog.setOnCancelListener(d -> {
                cancelInitMock = true;
                try {
                    nativeRequestCancelInit();
                } catch (Throwable ignored) {
                }
            });
            new Thread(() -> {
                boolean ok = false;
                try {
                    ok = !cancelInitMock && nativeInitFile(mockFilePath, cascadePath, storagePath);
                } catch (Throwable t) {
                    AppLog.e("MainActivity", "initEngine", "nativeInitFile 异常", t);
                }
                final boolean result = ok && !cancelInitMock;
                runOnUiThread(() -> {
                    try { dialog.dismiss(); } catch (Throwable ignored) {}
                    engineInitialized = result;
                    if (engineInitialized) {
                        applyFlipToNative();
                        applyInferenceThrottleToNative();
                        updateSystemReadyUi("引擎初始化成功 (Mock)");
                    } else {
                        updateSystemReadyUi("引擎初始化失败 (Mock)");
                    }
                    dispatchMonitoringDecision(monitoringCoordinator.onEngineInitResult(snapshotMonitoringInputs(), engineInitialized, true));
                });
            }).start();
        } else if (wantCamera) {
            engineInitialized = nativeInit(-1, cascadePath, storagePath);
            if (engineInitialized) {
                applyFlipToNative();
                applyInferenceThrottleToNative();
                updateSystemReadyUi("引擎初始化成功 (外部采集)");
            } else {
                updateSystemReadyUi("引擎初始化失败");
            }
        } else {
            engineInitialized = false;
            updateSystemReadyUi("未选择输入源");
        }
    }

    private String getAppStoragePath() {
        File f = getExternalFilesDir(null);
        if (f != null) return f.getAbsolutePath() + "/";
        return getFilesDir().getAbsolutePath() + "/";
    }

    private String copyAssetToCache(String fileName) {
        File cacheFile = new File(getFilesDir(), fileName);
        if (!cacheFile.exists()) {
            try (java.io.InputStream is = getAssets().open(fileName);
                 java.io.FileOutputStream fos = new java.io.FileOutputStream(cacheFile)) {
                byte[] buffer = new byte[1024];
                int size;
                while ((size = is.read(buffer)) != -1) {
                    fos.write(buffer, 0, size);
                }
            } catch (IOException e) {
                AppLog.e(TAG, "copyAssetToCache", "Failed to copy asset: " + fileName, e);
                return "";
            }
        }
        return cacheFile.getAbsolutePath();
    }

    private static CaptureScheme parseCaptureScheme(String raw, CaptureScheme fallback) {
        if (raw == null || raw.trim().isEmpty()) return fallback;
        try {
            return CaptureScheme.valueOf(raw.trim());
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private int getDeviceRotationDegrees() {
        try {
            int rot = getWindowManager().getDefaultDisplay().getRotation();
            if (rot == android.view.Surface.ROTATION_90) return 90;
            if (rot == android.view.Surface.ROTATION_180) return 180;
            if (rot == android.view.Surface.ROTATION_270) return 270;
            return 0;
        } catch (Exception ignored) {
            return 0;
        }
    }

    private String getFlipSourceKey() {
        boolean wantCamera = (selectedCameraId >= 0 && mockFilePath == null);
        return wantCamera ? ("cam_" + selectedCameraId) : "mock";
    }

    private void refreshFlipFromPrefs(boolean applyToNativeIfReady) {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String sourceKey = getFlipSourceKey();
        String kx = PREF_FLIP_X_PREFIX + sourceKey;
        String ky = PREF_FLIP_Y_PREFIX + sourceKey;

        boolean hasX = prefs.contains(kx);
        boolean hasY = prefs.contains(ky);

        boolean defaultX = false;
        if (selectedCameraId >= 0 && mockFilePath == null) {
            defaultX = isFrontCamera(selectedCameraId);
        }
        boolean nextX = hasX ? prefs.getBoolean(kx, defaultX) : defaultX;
        boolean nextY = hasY ? prefs.getBoolean(ky, false) : false;

        flipXHasOverride = hasX;
        flipYHasOverride = hasY;
        flipXEnabled = nextX;
        flipYEnabled = nextY;

        if (switchFlipX != null) {
            switchFlipX.setOnCheckedChangeListener(null);
            switchFlipX.setChecked(flipXEnabled);
            switchFlipX.setOnCheckedChangeListener(flipXSwitchListener);
        }
        if (switchFlipY != null) {
            switchFlipY.setOnCheckedChangeListener(null);
            switchFlipY.setChecked(flipYEnabled);
            switchFlipY.setOnCheckedChangeListener(flipYSwitchListener);
        }

        if (applyToNativeIfReady) {
            applyFlipToNative();
        }
    }

    private boolean isFrontCamera(int camId) {
        try {
            android.hardware.camera2.CameraManager mgr = (android.hardware.camera2.CameraManager) getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) return false;
            android.hardware.camera2.CameraCharacteristics chars = mgr.getCameraCharacteristics(String.valueOf(camId));
            Integer facing = chars.get(android.hardware.camera2.CameraCharacteristics.LENS_FACING);
            return facing != null && facing == android.hardware.camera2.CameraCharacteristics.LENS_FACING_FRONT;
        } catch (Throwable ignored) {
            return false;
        }
    }

    private void applyFlipToNative() {
        if (!engineInitialized) return;
        try {
            nativeSetFlip(flipXEnabled, flipYEnabled);
        } catch (Throwable ignored) {
        }
    }

    private boolean pushExternalYuv420888(ByteBuffer yBuffer,
                                          int yRowStride,
                                          ByteBuffer uBuffer,
                                          int uRowStride,
                                          int uPixelStride,
                                          ByteBuffer vBuffer,
                                          int vRowStride,
                                          int vPixelStride,
                                          int width,
                                          int height,
                                          long timestampNs,
                                          int rotationDegrees,
                                          boolean mirrored) {
        if (!engineInitialized) return false;
        long ts = timestampNs > 0 ? timestampNs : System.nanoTime();
        int chromaW = (width + 1) / 2;
        int chromaH = (height + 1) / 2;
        int yNeed = (yRowStride > 0 && height > 0) ? (yRowStride * (height - 1) + width) : 0;
        int uNeed = (uRowStride > 0 && uPixelStride > 0 && chromaW > 0 && chromaH > 0)
                ? (uRowStride * (chromaH - 1) + (chromaW - 1) * uPixelStride + 1)
                : 0;
        int vNeed = (vRowStride > 0 && vPixelStride > 0 && chromaW > 0 && chromaH > 0)
                ? (vRowStride * (chromaH - 1) + (chromaW - 1) * vPixelStride + 1)
                : 0;
        try {
            if (yBuffer != null) yBuffer.rewind();
            if (uBuffer != null) uBuffer.rewind();
            if (vBuffer != null) vBuffer.rewind();
            if (yBuffer != null && yBuffer.isDirect()
                    && uBuffer != null && uBuffer.isDirect()
                    && vBuffer != null && vBuffer.isDirect()) {
                if (yNeed > 0 && yBuffer.remaining() < yNeed) throw new IllegalStateException("Y缓冲区不足");
                if (uNeed > 0 && uBuffer.remaining() < uNeed) throw new IllegalStateException("U缓冲区不足");
                if (vNeed > 0 && vBuffer.remaining() < vNeed) throw new IllegalStateException("V缓冲区不足");
                boolean ok = nativePushFrameYuv420888(
                        yBuffer, yRowStride,
                        uBuffer, uRowStride, uPixelStride,
                        vBuffer, vRowStride, vPixelStride,
                        width, height,
                        ts, rotationDegrees, mirrored);
                if (ok) return true;
            }
        } catch (Exception ignored) {
        }

        try {
            byte[] yBytes = copyPlaneBytes(yBuffer, yNeed);
            byte[] uBytes = (uNeed > 0) ? copyPlaneBytes(uBuffer, uNeed) : null;
            byte[] vBytes = (vNeed > 0) ? copyPlaneBytes(vBuffer, vNeed) : null;
            if (yBytes == null) return false;
            return nativePushFrameYuv420888Bytes(
                    yBytes, yRowStride,
                    uBytes, uRowStride, uPixelStride,
                    vBytes, vRowStride, vPixelStride,
                    width, height,
                    ts, rotationDegrees, mirrored);
        } catch (Exception ignored) {
            return false;
        }
    }

    private static byte[] copyPlaneBytes(ByteBuffer buffer, int need) {
        if (buffer == null) return null;
        if (need <= 0) return new byte[0];
        ByteBuffer dup = buffer.duplicate();
        dup.rewind();
        if (dup.remaining() < need) return null;
        byte[] out = new byte[need];
        dup.get(out, 0, need);
        return out;
    }

    @Override
    public void onFramePushed(boolean ok, long timestampNs) {
        if (!ok) {
            pushFailStreak = Math.min(10_000, pushFailStreak + 1);
            return;
        }
        boolean first = !captureEverPushed;
        pushFailStreak = 0;
        captureEverPushed = true;
        lastPushOkRealtimeMs = SystemClock.elapsedRealtime();
        long ts = timestampNs > 0 ? timestampNs : System.nanoTime();
        StatsRepository.getInstance().reportCaptureFrameTimestampNs(ts);
        if (first) {
            String src = activeCapture != null ? activeCapture.name() : "N/A";
            AppLog.i("MainActivity", "capture", "首帧推入 ok tsNs=" + ts + " src=" + src + " camId=" + selectedCameraId);
        }
    }

    @Override
    public void onError(String stage, String message) {
        AppLog.e("MainActivity", "captureError", stage + ": " + message);
        handler.post(() -> handleCaptureFailure(stage + ": " + message));
    }

    private void startCaptureWatchdog() {
        stopCaptureWatchdog();
        long startMs = SystemClock.elapsedRealtime();
        lastPushOkRealtimeMs = startMs;
        captureEverPushed = false;
        captureWatchdog = new Runnable() {
            @Override
            public void run() {
                if (!isRunning) return;
                if (activeCapture == null) return;
                long now = SystemClock.elapsedRealtime();
                if (!captureEverPushed && now - startMs > 2500) {
                    handleCaptureFailure("首帧超时");
                    return;
                }
                if (captureEverPushed && now - lastPushOkRealtimeMs > 2000) {
                    handleCaptureFailure("出帧停滞");
                    return;
                }
                if (pushFailStreak >= 30) {
                    handleCaptureFailure("推帧失败过多");
                    return;
                }
                handler.postDelayed(this, 300);
            }
        };
        handler.postDelayed(captureWatchdog, 300);
    }

    private void stopCaptureWatchdog() {
        if (captureWatchdog != null) {
            handler.removeCallbacks(captureWatchdog);
            captureWatchdog = null;
        }
    }

    private void startPreviewWatchdog() {
        stopPreviewWatchdog();
        lastPreviewRenderOkRealtimeMs = SystemClock.elapsedRealtime();
        previewRenderFailStreak = 0;
        previewRecoveryCount = 0;
        lastPreviewRecoveryRealtimeMs = 0L;
        previewWatchdog = new Runnable() {
            @Override
            public void run() {
                if (!isRunning) return;
                long now = SystemClock.elapsedRealtime();
                boolean wantCamera = (selectedCameraId >= 0 && mockFilePath == null);
                boolean wantMock = (selectedCameraId == -1 && mockFilePath != null);
                boolean captureOk = !wantCamera || (captureEverPushed && now - lastPushOkRealtimeMs < 1200);
                boolean renderStalled = firstFrameReceived && now - lastPreviewRenderOkRealtimeMs > 2200;

                if (wantCamera && previewSurfaceReady && captureOk && renderStalled && previewRenderFailStreak >= 60) {
                    attemptRecoverPreview("渲染停滞");
                }
                handler.postDelayed(this, 500);
            }
        };
        handler.postDelayed(previewWatchdog, 500);
    }

    private void stopPreviewWatchdog() {
        if (previewWatchdog != null) {
            handler.removeCallbacks(previewWatchdog);
            previewWatchdog = null;
        }
    }

    private void attemptRecoverPreview(String reason) {
        long now = SystemClock.elapsedRealtime();
        if (now - lastPreviewRecoveryRealtimeMs < 1800) return;
        lastPreviewRecoveryRealtimeMs = now;
        previewRecoveryCount = Math.min(10_000, previewRecoveryCount + 1);

        String diag = buildPreviewDiag(reason);
        AppLog.e("MainActivity", "previewRecovery", diag);
        Toast.makeText(this, "预览恢复: " + reason, Toast.LENGTH_SHORT).show();

        if (previewRecoveryCount >= 3) {
            previewSurfaceReady = false;
            if (monitorView != null) monitorView.setVisibility(View.VISIBLE);
            if (previewSurface != null) previewSurface.setVisibility(View.GONE);
            return;
        }

        try {
            previewSurfaceReady = false;
            if (monitorView != null) monitorView.setVisibility(View.VISIBLE);
            nativeSetPreviewSurface(null);
        } catch (Throwable ignored) {
        }

        handler.postDelayed(() -> {
            if (!isRunning) return;
            if (previewSurface == null) return;
            try {
                Surface s = previewSurface.getHolder().getSurface();
                if (s != null && s.isValid()) {
                    nativeSetPreviewSurface(s);
                    previewSurfaceReady = true;
                    if (monitorView != null) monitorView.setVisibility(View.GONE);
                    previewSurface.setVisibility(View.VISIBLE);
                } else {
                    previewSurfaceReady = false;
                    if (monitorView != null) monitorView.setVisibility(View.VISIBLE);
                }
            } catch (Throwable ignored) {
            }
        }, 220);
    }

    private String buildPreviewDiag(String reason) {
        long now = SystemClock.elapsedRealtime();
        long capAge = lastPushOkRealtimeMs > 0 ? (now - lastPushOkRealtimeMs) : -1L;
        long rendAge = lastPreviewRenderOkRealtimeMs > 0 ? (now - lastPreviewRenderOkRealtimeMs) : -1L;
        String src = activeCapture != null ? activeCapture.name() : "N/A";
        return "reason=" + reason +
                " previewSurfaceReady=" + previewSurfaceReady +
                " renderFailStreak=" + previewRenderFailStreak +
                " recoveryCount=" + previewRecoveryCount +
                " captureEverPushed=" + captureEverPushed +
                " capAgeMs=" + capAge +
                " renderAgeMs=" + rendAge +
                " scheme=" + src +
                " camId=" + selectedCameraId +
                " mock=" + (mockFilePath != null);
    }

    private void performHotRestart() {
        String diag = buildPreviewDiag("手动热重启");
        AppLog.i("MainActivity", "hotRestart", diag);
        
        ProgressDialog dialog = new ProgressDialog(this);
        dialog.setMessage("正在应用设置并重启引擎...");
        dialog.setCancelable(false);
        dialog.show();

        if (isRunning) {
            restartMonitoring("手动热重启", 350);
        } else {
            startMonitoring();
        }

        // 延迟关闭对话框
        handler.postDelayed(() -> {
            try {
                if (dialog.isShowing()) dialog.dismiss();
            } catch (Exception ignored) {}
        }, 1500);
    }

    private void handleCaptureFailure(String reason) {
        updateSystemReadyUi("采集异常: " + reason);
        dispatchMonitoringDecision(monitoringCoordinator.onCaptureFailure(snapshotMonitoringInputs(), reason));
    }

    private void restartMonitoring(String reason) {
        restartMonitoring(reason, 450);
    }

    private void restartMonitoring(String reason, long delayMs) {
        dispatchMonitoringDecision(monitoringCoordinator.onRestartRequested(snapshotMonitoringInputs(), reason, delayMs));
    }

    private void updateSystemReadyUi(String reason) {
        boolean runtimeGranted = permissionStateMachine != null && permissionStateMachine.isRuntimeGranted();
        boolean permissionOk = runtimeGranted || (selectedCameraId == -1 && mockFilePath != null);
        boolean ready = permissionOk && engineInitialized && isRunning && firstFrameReceived;
        if (ready) {
            if (!lastReadyVisible) {
                AppLog.i("MainActivity", "updateSystemReadyUi", "SYSTEM READY permissionOk=" + permissionOk + " engineInitialized=" + engineInitialized + " firstFrameReceived=" + firstFrameReceived);
            }
            lastReadyVisible = true;
            return;
        }

        if (lastReadyVisible) {
            AppLog.e("MainActivity", "updateSystemReadyUi", "SYSTEM NOT READY: " + reason);
        } else if (reason != null && !reason.isEmpty()) {
            // Distinguish errors from progress updates
            if (reason.contains("失败") || reason.contains("不足") || reason.contains("FAILED") || reason.contains("Error")) {
                AppLog.e("MainActivity", "updateSystemReadyUi", "SYSTEM NOT READY (Error): " + reason);
            } else {
                AppLog.i("MainActivity", "updateSystemReadyUi", "System initializing... (" + reason + ")");
            }
        }
        lastReadyVisible = false;
    }

    public void onNativeResult(@NonNull String result) {
        runOnUiThread(() -> {
            if (recognitionEventAdapter == null) return;
            recognitionEventAdapter.prepend(new RecognitionEvent(System.currentTimeMillis(), result));
            if (rvRecognitionEvents != null) {
                rvRecognitionEvents.scrollToPosition(0);
            }
        });
    }

    private void toggleRecognitionPanel() {
        setRecognitionPanelVisible(!recognitionEventsVisible, true);
    }

    private void setRecognitionPanelVisible(boolean visible, boolean animate) {
        recognitionEventsVisible = visible;
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_EVENTS_VISIBLE, visible).apply();

        if (panelRecognitionEvents == null) return;
        int target = visible ? dpToPx(180) : 0;
        ViewGroup.LayoutParams lp = panelRecognitionEvents.getLayoutParams();
        if (lp == null) return;

        if (!animate) {
            lp.height = target;
            panelRecognitionEvents.setLayoutParams(lp);
            panelRecognitionEvents.setVisibility(visible ? View.VISIBLE : View.GONE);
            return;
        }

        int start = panelRecognitionEvents.getHeight();
        panelRecognitionEvents.setVisibility(View.VISIBLE);
        ValueAnimator animator = ValueAnimator.ofInt(start, target);
        animator.setDuration(300);
        animator.addUpdateListener(a -> {
            int h = (int) a.getAnimatedValue();
            ViewGroup.LayoutParams p = panelRecognitionEvents.getLayoutParams();
            if (p != null) {
                p.height = h;
                panelRecognitionEvents.setLayoutParams(p);
            }
        });
        animator.start();
        animator.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(android.animation.Animator animation) {
                panelRecognitionEvents.setVisibility(target == 0 ? View.GONE : View.VISIBLE);
            }
        });
    }

    private int dpToPx(int dp) {
        float density = getResources().getDisplayMetrics().density;
        return Math.round(dp * density);
    }

    private void applyMonitorLayoutRule(int frameW, int frameH) {
        if (videoWrapper == null) return;
        View parent = (View) videoWrapper.getParent();
        if (parent == null) return;
        int pw = parent.getWidth();
        int ph = parent.getHeight();
        if (pw <= 0 || ph <= 0 || frameW <= 0 || frameH <= 0) return;

        float ar = frameW / (float) frameH;
        float containerAr = pw / (float) ph;

        ViewGroup.LayoutParams rawLp = videoWrapper.getLayoutParams();
        if (!(rawLp instanceof FrameLayout.LayoutParams)) return;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) rawLp;
        lp.gravity = Gravity.CENTER;

        if (ar >= containerAr) {
            // Video is wider than container -> Fit width, letterbox height
            int h = Math.round(pw / ar);
            lp.width = pw;
            lp.height = h;
        } else {
            // Video is taller than container -> Fit height, pillarbox width
            int w = Math.round(ph * ar);
            lp.width = w;
            lp.height = ph;
        }
        videoWrapper.setLayoutParams(lp);
    }

    private void toggleSettingsPanel() {
        if (panelSettings == null) return;
        boolean show = panelSettings.getVisibility() != View.VISIBLE;
        if (show) {
            panelSettings.setAlpha(0f);
            panelSettings.setVisibility(View.VISIBLE);
            panelSettings.animate().alpha(1f).setDuration(300).start();
        } else {
            panelSettings.animate().alpha(0f).setDuration(300).withEndAction(() -> {
                panelSettings.setVisibility(View.GONE);
                panelSettings.setAlpha(1f);
            }).start();
        }
    }

    private void showAboutDialog() {
        DeviceProfile p = DeviceRuntime.get().getProfile();
        DeviceClass c = DeviceRuntime.get().getDeviceClass();
        new AlertDialog.Builder(this)
                .setTitle("关于")
                .setMessage("RK3288 AI Engine\\n\\n设备分类: " + c + "\\n" + p.toShortString() +
                        "\\n\\n退出说明：\\n" +
                        "- 安全退出：释放资源并从最近任务移除，但系统不保证立刻杀死进程\\n" +
                        "- 强退：在安全退出基础上强制结束进程（不推荐）")
                .setPositiveButton("关闭", null)
                .show();
    }

    private void applyMockUrl() {
        if (etMockUrl == null) return;
        String url = etMockUrl.getText() == null ? "" : etMockUrl.getText().toString().trim();
        if (url.isEmpty()) {
            Toast.makeText(this, "请输入 Mock URL", Toast.LENGTH_SHORT).show();
            return;
        }
        mockFilePath = url;
        selectedCameraId = -1;
        isSpinnerInitialized = true;
        refreshFlipFromPrefs(true);
        initEngine();
    }

    private void startRtmpPush() {
        if (etRtmpUrl == null) return;
        String url = etRtmpUrl.getText() == null ? "" : etRtmpUrl.getText().toString().trim();
        if (url.isEmpty()) {
            Toast.makeText(this, "请输入 RTMP URL", Toast.LENGTH_SHORT).show();
            return;
        }
        
        String input;
        if (selectedCameraId == -1 && mockFilePath != null) {
            input = mockFilePath;
        } else {
            // Camera source - Use Pipe (requires native implementation)
            // For now, we pass "pipe:0" to test command construction, 
            // but show warning that stream won't flow without JNI pipe feed.
            input = "pipe:0";
            if (selectedCameraId != -1) {
                 Toast.makeText(this, "Camera push requires native pipe (Not fully implemented)", Toast.LENGTH_LONG).show();
            } else {
                 Toast.makeText(this, "请先设置 Mock 源或选择摄像头", Toast.LENGTH_SHORT).show();
                 return;
            }
        }
        
        Toast.makeText(this, "开始推流: " + url, Toast.LENGTH_SHORT).show();
        rtmpPusher.start(input, url, (success, code) -> runOnUiThread(() -> {
            Toast.makeText(this, "推流结束 success=" + success + " code=" + code, Toast.LENGTH_LONG).show();
        }));
    }

    private void stopRtmpPush() {
        rtmpPusher.stop();
        Toast.makeText(this, "已请求停止推流", Toast.LENGTH_SHORT).show();
    }

    private void toggleFullscreen() {
        isFullscreen = !isFullscreen;
        if (isFullscreen) {
            if (recognitionTitle != null) recognitionTitle.setVisibility(View.GONE);
            if (panelSettings != null) panelSettings.setVisibility(View.GONE);
            if (panelRecognitionEvents != null) {
                ViewGroup.LayoutParams lp = panelRecognitionEvents.getLayoutParams();
                if (lp != null) {
                    lp.height = 0;
                    panelRecognitionEvents.setLayoutParams(lp);
                }
                panelRecognitionEvents.setVisibility(View.GONE);
            }
            // Reset wrapper to match parent for fullscreen
            if (videoWrapper != null) {
                 FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) videoWrapper.getLayoutParams();
                 lp.width = ViewGroup.LayoutParams.MATCH_PARENT;
                 lp.height = ViewGroup.LayoutParams.MATCH_PARENT;
                 videoWrapper.setLayoutParams(lp);
            }
            if (monitorView != null) monitorView.setScaleType(ImageView.ScaleType.CENTER_CROP);
        } else {
            if (recognitionTitle != null) recognitionTitle.setVisibility(View.VISIBLE);
            setRecognitionPanelVisible(recognitionEventsVisible, false);
            // Restore aspect ratio logic
            if (frameBitmap != null) {
                applyMonitorLayoutRule(frameBitmap.getWidth(), frameBitmap.getHeight());
            }
            if (monitorView != null) monitorView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        }
    }

    private void onPermissionStateChanged(@NonNull PermissionStateMachine.State state, @NonNull List<String> missingRuntimePermissions) {
        AppLog.enter("MainActivity", "onPermissionStateChanged");
        boolean granted = (state == PermissionStateMachine.State.GRANTED);
        if (!granted && selectedCameraId >= 0) {
            engineInitialized = false;
            firstFrameReceived = false;
        }
        dispatchMonitoringDecision(monitoringCoordinator.onPermissionStateChanged(
                snapshotMonitoringInputs(),
                granted,
                missingRuntimePermissions,
                state.name()
        ));
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        AppLog.enter("MainActivity", "onRequestPermissionsResult");
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (permissionStateMachine != null) {
            permissionStateMachine.onRequestPermissionsResult(requestCode, permissions, grantResults);
        }
    }

    private void discoverCameras() {
        AppLog.enter("MainActivity", "discoverCameras");
        CameraManager manager = (CameraManager) getSystemService(Context.CAMERA_SERVICE);
        availableCameras.clear();
        List<String> displayNames = new ArrayList<>();
        int selectionIndex = 0;

        try {
            if (manager == null) return;
            String[] cameraIds = manager.getCameraIdList();
            
            for (int i = 0; i < cameraIds.length; i++) {
                String id = cameraIds[i];
                try {
                    CameraCharacteristics chars = manager.getCameraCharacteristics(id);
                    Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
                    
                    String facingStr = "Unknown";
                    if (facing != null) {
                        if (facing == CameraCharacteristics.LENS_FACING_BACK) facingStr = "Back";
                        else if (facing == CameraCharacteristics.LENS_FACING_FRONT) facingStr = "Front";
                        else if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) facingStr = "External/USB";
                    }

                    String desc = String.format("Cam %s (%s)", id, facingStr);
                    availableCameras.add(new CameraInfo(id, desc, facing != null ? facing : -1));
                    displayNames.add(desc);

                    // Check if this matches selected ID
                    try {
                        if (Integer.parseInt(id) == selectedCameraId) {
                            selectionIndex = i;
                        }
                    } catch (NumberFormatException e) {
                        AppLog.w("MainActivity", "discoverCameras", "Invalid camera ID format: " + id, e);
                    }

                } catch (Exception e) {
                    AppLog.e("MainActivity", "discoverCameras", "查询相机失败 id=" + id, e);
                }
            }
        } catch (Exception e) {
            AppLog.e("MainActivity", "discoverCameras", "发现相机失败", e);
            Toast.makeText(this, "Failed to discover cameras: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
        
        // Add Mock Option
        availableCameras.add(new CameraInfo("-1", "Mock Source (File Picker)", -1));
        displayNames.add("Mock Source (File Picker)");
        
        availableCameras.add(new CameraInfo("-2", "Mock Camera (System App)", -1));
        displayNames.add("Mock Camera (System App)");
        
        // If current selection is -1 (Mock), set index to last
        if (selectedCameraId == -1) {
            selectionIndex = availableCameras.size() - 1;
        }

        // Update UI
        int finalSelectionIndex = selectionIndex;
        runOnUiThread(() -> {
            cameraAdapter.clear();
            cameraAdapter.addAll(displayNames);
            cameraAdapter.notifyDataSetChanged();
            if (!availableCameras.isEmpty()) {
                spinnerCameras.setSelection(finalSelectionIndex);
            }
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (permissionStateMachine != null) {
            permissionStateMachine.evaluate();
        }
        syncOverlaySwitchState();
        
        // Issue 42 Fix: Restore native preview surface if it was already ready but native lost it
        if (previewSurfaceReady && previewSurface != null) {
            try {
                Surface surface = previewSurface.getHolder().getSurface();
                if (surface != null && surface.isValid()) {
                    nativeSetPreviewSurface(surface);
                }
            } catch (Throwable ignored) {
            }
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
    }

    @Override
    protected void onStart() {
        super.onStart();
        try {
            Intent i = new Intent(this, StatusService.class);
            i.setAction(StatusService.ACTION_SET_FOREGROUND);
            i.putExtra(StatusService.EXTRA_FOREGROUND, false);
            startService(i);
        } catch (Throwable ignored) {
        }
        if (restartMonitoringOnStart) {
            restartMonitoringOnStart = false;
            if (permissionStateMachine != null) {
                permissionStateMachine.evaluate();
            }
            if (permissionStateMachine != null && permissionStateMachine.isRuntimeGranted() && !isRunning) {
                handler.postDelayed(this::startMonitoring, 200);
            }
        }
    }

    @Override
    protected void onStop() {
        if (isRunning) {
            restartMonitoringOnStart = true;
            stopMonitoring();
        }
        try {
            boolean overlayEnabled = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getBoolean(PREF_OVERLAY_ENABLED, false);
            if (overlayEnabled) {
                Intent i = new Intent(this, StatusService.class);
                i.setAction(StatusService.ACTION_SET_FOREGROUND);
                i.putExtra(StatusService.EXTRA_FOREGROUND, true);
                startService(i);
            }
        } catch (Throwable ignored) {
        }
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopMonitoring();
        if (statsUpdater != null) {
            handler.removeCallbacks(statsUpdater);
        }
        if (inferenceAutoUpdater != null) {
            handler.removeCallbacks(inferenceAutoUpdater);
            inferenceAutoUpdater = null;
        }
        try {
            unregisterReceiver(usbReceiver);
        } catch (Exception ignored) {
        }
        if (frameHandler != null) {
            frameHandler.removeCallbacksAndMessages(null);
        }
        frameHandler = null;
        if (frameThread != null) {
            frameThread.quitSafely();
        }
        frameThread = null;
        if (isFinishing()) {
            new Thread(() -> {
                CacheCleaner.Result r = CacheCleaner.cleanOnExit(MainActivity.this);
                AppLog.i("MainActivity", "onDestroy", "缓存清理: deletedFiles=" + r.deletedFiles + " deletedBytes=" + r.deletedBytes + " errors=" + r.errors);
            }, "CacheCleaner").start();
        }
    }
}
