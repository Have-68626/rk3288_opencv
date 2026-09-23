package com.example.rk3288_opencv;

import android.Manifest;
import android.app.AlertDialog;
import android.app.ActivityManager;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
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
import android.view.LayoutInflater;
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
import androidx.annotation.Nullable;
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
import java.io.IOException;
import java.io.InputStream;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

import androidx.lifecycle.ViewModelProvider;

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
    private static final String PREF_ACCEL_LIBYUV = "pref_accel_libyuv";
    private static final String PREF_ACCEL_MPP = "pref_accel_mpp";
    private static final String PREF_ACCEL_QUALCOMM = "pref_accel_qualcomm";
    private static final String PREF_DETECTION_THROTTLE_MODE = "pref_detection_throttle_mode";
    private static final String PREF_DETECTION_INTERVAL_MS = "pref_detection_interval_ms";
    private static final String PREF_INFERENCE_THROTTLE_MODE = "pref_inference_throttle_mode";
    private static final String PREF_INFERENCE_INTERVAL_MS = "pref_inference_interval_ms";
    /**
     * 【历史】任务18 的 personId → 显示名明文映射前缀。**任务 B 起不再写入**。
     *
     * <p>前缀与「检测/清除」策略见 {@link LegacyDisplayNames}（那边有完整的保护边界说明）。
     * 保护边界摘要：注册表（`face_registry_v1.dat`）只存 userId 等元数据、**不含显示名**；
     * 显示名自任务 B 起由 {@link PersonDisplayNameStore} **加密**存于 `files/face_names/`；
     * 模板向量加密存于 `files/face_templates/`。另注：`userId` 本身是**可逆 hex 编码**（非加密），
     * 故**不得**宣称「人员 PII 全部加密」。
     */
    private static final String PREF_PERSON_NAME_PREFIX = LegacyDisplayNames.PREF_PREFIX;

    /** 任务 B：旧明文显示名是否已迁移完毕（迁移成功才置位；失败保留原值下次重试）。 */
    private static final String PREF_NAME_MIGRATED = "pref_person_name_migrated_v1";
    private static final String TAG = "MainActivity";

    /** Sentinel: no real camera device, external frame input only */
    private static final String NO_CAMERA_ID = null;

    private static final int DETECTION_INTERVAL_DEFAULT_MS = 150;
    private static final int DETECTION_INTERVAL_MIN_MS = 80;
    private static final int DETECTION_INTERVAL_MAX_MS = 500;

    private static final int INFERENCE_INTERVAL_DEFAULT_MS = 650;
    private static final int INFERENCE_INTERVAL_MIN_MS = 80;
    private static final int INFERENCE_INTERVAL_MAX_MS = 2000;

    // 任务15：模型来源与加速能力状态的刷新间隔（低频，避免逐帧查询 JNI）。
    // FPS/CAP/LAT/CPU/MEM 仍沿用各自的 500ms 节流，不受此常量影响。
    private static final long RUNTIME_STATUS_REFRESH_INTERVAL_MS = 2000L;
    // 标准 Gradle 构建在 app/build.gradle 显式传入 -DRK_ENABLE_LIBYUV=OFF，
    // 因此 libyuv 在编译期即被禁用（compiled=false），不能仅凭 requested 标为生效。
    private static final boolean LIBYUV_COMPILED_IN_BUILD = false;

    private ImageView monitorView;
    private SurfaceView previewSurface;
    private TextView tvFps, tvCpu, tvMemory, tvStatus, tvOverlayStatus;
    private TextView tvLatency;
    private TextView tvCaptureStrategyInfo;
    private TextView tvDiagnostics;
    // 任务15：模型来源/加速能力状态缓存文本与上次刷新时间（低频刷新，事件驱动 + 2s 节流）。
    private String runtimeCapabilityText = "";
    private long lastRuntimeStatusRefreshMs = 0L;
    /**
     * Med（第三波审查）：模型状态快照刷新的在途标志，合并并发刷新请求。
     *
     * <p>此前每次刷新都 {@code new Thread}，导入/切换持目录锁期间（含 native 自检与基准推理）
     * 多个刷新线程会在锁上堆叠。本标志保证任一时刻至多 1 个刷新线程在途：在途期间的请求直接
     * 跳过——在途线程解锁后即产出最新快照，且 UI 侧仍有 2s 周期刷新兜底，不会漏刷。
     */
    private final AtomicBoolean modelStatusRefreshInFlight = new AtomicBoolean(false);

    /**
     * 模型状态刷新的单调序号（第五轮审查 P2）：只允许**最新**一次刷新的结果写入状态区。
     *
     * <p>{@link #modelStatusRefreshInFlight} 门在投递 UI 之前就已释放，旧线程可能在更新的一次刷新
     * 落地之后才投递，从而把状态区覆盖回过期文本。序号使迟到的旧结果被丢弃。
     */
    private final AtomicLong modelStatusRefreshSeq = new AtomicLong();
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

    // 任务18 / #1：模型管理与人员注册接入（视图在 settings 面板中，直接 findViewById 绑定，
    // 不经过 MainScreenBinder，避免改动该绑定类的既有契约）。
    private Button btnRegisterFace;
    private Button btnImportModel;
    private Button btnSwitchModel;
    private Button btnManagePersons;
    private TextView tvModelManagement;
    /**
     * 「处理未决替换」按钮（2026-09-17 外部审查修复）：仅当状态报告「新旧配置并存且无提交证据」
     * 时可见。它的存在使 {@code resolveAmbiguousSwap} 不会成为死代码——没有它，那种现场会永久
     * 阻塞该 profile 的后续导入。
     */
    private Button btnResolveSwap;
    /**
     * 当前需要人工了结的 profileId（由 {@link #refreshModelManagementStatus()} 在 UI 线程写、
     * 由按钮点击时在 UI 线程读，故无需 volatile；后台刷新线程不触碰它）。
     */
    @Nullable
    private String ambiguousSwapProfileId;
    /**
     * #3-High（第三波审查）：UI 线程（{@code statusSnapshot()}）与后台刷新线程（{@code status()}）
     * 首次会几乎同时调用 {@link #modelManager()}。字段必须 volatile 配合双检锁，保证二者共享
     * <b>同一</b>实例——否则会各建一个 {@link FaceModelManager}、各自独立的 statusCache，UI 侧
     * 实例从未被调用 status() 时其 statusSnapshot() 将永久返回占位「未导入」。
     */
    private volatile FaceModelManager faceModelManager;
    /** 同类懒加载（UI/后台线程并发首次调用），按同一策略一并加固为 volatile + 双检锁。 */
    private volatile AndroidEnrollmentRepository enrollmentRepository;

    /** 显示名加密存储 + 内存缓存（任务 B）。解密不能上 UI 线程，UI 侧只读其缓存。 */
    private volatile PersonDisplayNameStore displayNameStore;

    /**
     * 采集期间的显示名（**仅内存**，任务 B）。
     *
     * <p>显示名改为「注册成功后才加密落盘」：采集开始前不再写任何持久化存储，故**取消或注册失败
     * 天然不会留下残留**（此前是先写 SharedPreferences 再开始采集，取消/失败都不清理）。
     * 会话结束（成功/取消/失败/销毁）时清空本字段。
     */
    private volatile String pendingEnrollmentDisplayName;
    /** 执行中的注册会话；native 采集线程会读取，故 volatile 保证可见性。 */
    private volatile EnrollmentSession enrollmentSession;
    /** #1 注册采集：复用的 direct ByteBuffer（512×4 字节 LE float32），由 native 逐帧写入。 */
    private ByteBuffer enrollmentCaptureBuffer;
    private AlertDialog enrollmentDialog;
    private EditText regPersonIdInput;
    private EditText regDisplayNameInput;
    private TextView regStatusText;
    private Button regStartButton;
    private Button regCancelButton;
    private Runnable enrollmentPoller;
    private volatile boolean enrollmentInProgress = false;
    /** 注册准备/轮询代次：对话框关闭或取消时递增，使未完成的准备回调整体作废。 */
    private int enrollmentGeneration = 0;
    /** 最近一次采样被拒原因（仅在喂入样本时更新）；不含任何模板/向量内容。 */
    private volatile String lastEnrollmentRejectReason = "";

    // Delegate helpers (preliminary split P1.7)
    private CameraFragment cameraFragment;
    private EngineViewModel engineViewModel;

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
    private volatile boolean restartMonitoringOnStart = false;
    private volatile boolean pendingResumeStart = false;
    // 生命周期停止门：onStop 置位、onStart 复位。
    // 消除"后台未销毁 + isRunning==false 时 onStart 轮询到期误启动监控"窗口（RW5 遗留；
    // 代次门拦不住因为轮询续排发生在代次递增之后）。
    private volatile boolean isStopped = false;
    private int surfaceCreatedCount = 0;
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
                surfaceCreatedCount++;
                AppLog.d("MainActivity", "surfaceCreated",
                    "count=" + surfaceCreatedCount + " pendingResume=" + pendingResumeStart +
                    " engineInit=" + engineInitialized + " isRunning=" + isRunning);
                if (monitorView != null) monitorView.setVisibility(View.GONE);
                // Auto-recover: if Engine is ready but monitoring stopped, restart
                if (pendingResumeStart && engineInitialized && !isRunning && selectedCameraId != null) {
                    pendingResumeStart = false;
                    AppLog.i("MainActivity", "surfaceCreated",
                        "Surface ready, resuming monitoring (delay 100ms)");
                    scheduleStartMonitoring(100);
                }
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
        prefs.edit().putBoolean(PREF_FLIP_X_PREFIX + cameraFragment.getFlipSourceKey(), isChecked).apply();
        applyFlipToNative();
    };
    private final CompoundButton.OnCheckedChangeListener flipYSwitchListener = (buttonView, isChecked) -> {
        flipYEnabled = isChecked;
        flipYHasOverride = true;
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        prefs.edit().putBoolean(PREF_FLIP_Y_PREFIX + cameraFragment.getFlipSourceKey(), isChecked).apply();
        applyFlipToNative();
    };
    private Handler handler = new Handler(Looper.getMainLooper());
    private HandlerThread frameThread;
    private Handler frameHandler;
    private Runnable frameUpdater;
    private Runnable statsUpdater;
    private Runnable captureWatchdog;
    
    private String selectedCameraId = "0";
    private List<CameraFragment.CameraInfo> availableCameras = new ArrayList<>();
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
        tvDiagnostics = b.tvDiagnostics;
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
        bindModelManagementUi();
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
        // 任务15：监控状态变更属低频事件，强制刷新一次模型/加速能力状态块。
        refreshRuntimeStatusIfDue(true);
        updateDiagnosticsText();
    }

    // 任务3：构建/相机诊断信息 —— 状态页展示 Git commit/构建时间/ABI/source epoch/
    // flip generation/cameraId，仅在状态渲染时刷新（非逐帧写日志），用于直接判断设备
    // 是否安装旧 APK，并定位频繁切源或配置变更。
    // 任务15：在其后追加「模型来源 + 每能力 compiled/installed/requested/effective/
    // reason/evidence + 最近推理耗时或 p95」状态块（数据源见 buildRuntimeCapabilityText）。
    private void updateDiagnosticsText() {
        if (tvDiagnostics == null) return;
        String cam = selectedCameraId != null ? selectedCameraId : "?";
        long srcEpoch = (activeCapture != null) ? activeCapture.currentSourceEpoch() : 0L;
        long flipGen = nativeGetFlipGeneration();
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "?";
        // 双保险：本方法也会被低频统计循环调用，这里再走一次 2s 节流（非强制）。
        refreshRuntimeStatusIfDue(false);
        tvDiagnostics.setText(
                "git=" + BuildConfig.BUILD_GIT_COMMIT +
                " time=" + BuildConfig.BUILD_TIME +
                " abi=" + abi +
                "\ncam=" + cam + " srcEpoch=" + srcEpoch + " flipGen=" + flipGen +
                "\n" + runtimeCapabilityText);
    }

    /**
     * 任务15：按需刷新模型/加速能力状态文本。
     *
     * <p>刷新策略：事件驱动（监控状态渲染、能力开关变化等）时 {@code force=true} 立即刷新；
     * 低频统计循环里 {@code force=false}，由 {@link #RUNTIME_STATUS_REFRESH_INTERVAL_MS}
     * 节流（默认 2s），避免逐帧调用 JNI / 逐帧写 UI。日志不写入任何模板或向量内容。
     *
     * <p>#3（第三波审查）：本方法自身在主线程执行，只做「触发后台刷新 + 读无锁快照组装文本」，
     * 绝不在此调用 {@link FaceModelManager#status()}（其持目录锁并含文件 IO/native，导入/切换
     * 持锁期间会整段阻塞 UI -> ANR）。快照由后台线程刷新，展示文本可能落后一个刷新周期。
     *
     * @return 本次是否真正刷新了展示文本
     */
    private boolean refreshRuntimeStatusIfDue(boolean force) {
        long now = SystemClock.elapsedRealtime();
        if (!force && now - lastRuntimeStatusRefreshMs < RUNTIME_STATUS_REFRESH_INTERVAL_MS) {
            return false;
        }
        lastRuntimeStatusRefreshMs = now;
        // 触发后台刷新模型状态快照（renderMonitoringUi 等事件驱动点由此落到后台线程）。
        requestModelStatusSnapshotRefresh();
        runtimeCapabilityText = buildRuntimeCapabilityText();
        return true;
    }

    /**
     * #3（第三波审查）：在后台线程刷新 {@link FaceModelManager} 的状态快照。
     *
     * <p>{@code status()} 持目录锁且内部做文件 IO 与 native {@code getRuntimeStatus}；导入/切换在持锁
     * 期间还会执行 native 自检与基准推理，故绝不能在 UI 线程调用。此处在后台线程调用 {@code status()}，
     * 其内部把结果写入 volatile 快照，UI 线程随后经 {@link FaceModelManager#statusSnapshot()} 无锁读取。
     * 失败静默：UI 继续显示上一帧快照或首帧占位。
     */
    private void requestModelStatusSnapshotRefresh() {
        // Med（第三波审查）：合并并发刷新——已有刷新在途时直接跳过，避免导入/切换持锁期线程堆叠。
        if (!modelStatusRefreshInFlight.compareAndSet(false, true)) {
            return;
        }
        try {
            new Thread(() -> {
                try {
                    // 副作用：刷新 FaceModelManager 内部 volatile 快照；返回值此处不需要。
                    modelManager().status();
                } catch (Throwable ignored) {
                    // 快照刷新失败不阻断 UI：保持上一帧快照/占位。
                } finally {
                    modelStatusRefreshInFlight.set(false);
                }
            }, "FaceModelStatus").start();
        } catch (Throwable t) {
            // 线程创建/启动失败（极端 OOM 等）：复位在途标志，避免刷新永久停摆。
            modelStatusRefreshInFlight.set(false);
        }
    }

    /**
     * 任务15：构造模型来源 + 各加速能力的状态行。
     *
     * <p>数据来源：
     * <ul>
     *   <li>{@code FaceModelManager.status()}（任务14，P1-6）：模型来源标签「用户导入/未导入/
     *       不可用」、profileId/维度/许可与缺失原因。</li>
     *   <li>{@code NativeFaceBridge.getRuntimeStatus()} 返回的 JSON（任务11/13）：读取顶层
     *       {@code ok/profileValidated/profileLoaded/profileId/embeddingDim/galleryLoaded/
     *       galleryCount/galleryDim/enrollmentState}，以及
     *       {@code effective/reason}（**native 已输出**，见 {@code native_face_bridge.cpp} 的
     *       {@code bridge_getRuntimeStatus}）。
     *       ⚠️ 同处更正（A1 审计指出）：此处原写「（若 native 侧后续暴露）{@code requested/
     *       effective/reason/evidence/benchmarkP95Ms}」，其中 {@code effective/reason} 早已输出，
     *       该措辞会让人误以为管道级字段尚未可用。
     *       仍旧**未暴露**的是 {@code requested/evidence/benchmarkP95Ms}，与嵌套
     *       {@code accelerators.<cap>.{compiled,installed,requested,effective,reason,evidence}}
     *       —— 后者由 {@link JsonText#capObject} 做前向兼容，现行数据下恒为 null。</li>
     *   <li>Java 侧既有事实：能力开关与偏好（与 initEngine 的 RK_USE_* 一致）、
     *       {@link BuildConfig} 构建信息、{@link StatsRepository} 最近推理耗时。</li>
     * </ul>
     *
     * <p>规则：compiled/installed 不等于 effective；OpenCL/Qualcomm 即使编译进来，未经
     * native 明确置 effective=true 一律显示为未生效。模型未导入时显式给出 {@code missing_model}
     * 缺失原因，绝不静默回退 LBP。native 未暴露的字段以 {@code ?}/“未暴露”如实展示，不臆测。
     */
    private String buildRuntimeCapabilityText() {
        String json = null;
        try {
            json = NativeFaceBridge.getRuntimeStatus();
        } catch (Throwable t) {
            // 动态注册缺失（jni-sync 未同步）时记下状态不可用，但仍继续展示模型来源
            // （FaceModelManager 会自行降级），不中断状态区渲染。
            json = null;
        }
        boolean nativeOk = json != null && !json.isEmpty() && jsonGetBool(json, "ok", true);
        String nativeNote = (json == null || json.isEmpty())
                ? "native 状态不可用"
                : (nativeOk ? "" : "native 返回 ok=false");

        boolean profileLoaded = jsonGetBool(json, "profileLoaded", false);
        String profileId = jsonGetString(json, "profileId", "");
        long dim = jsonGetLong(json, "embeddingDim", 0);
        long galleryCount = jsonGetLong(json, "galleryCount", 0);
        String enrollState = jsonGetString(json, "enrollmentState", "?");

        // 管道级（任务11 快照）字段：native 尚未暴露时为 null/空，下游按保守值回退。
        // 兼容顶层 flat 与 accelerators 嵌套两种形态。
        String pipelineReason = jsonGetString(json, "reason", "");
        String pipelineEvidence = jsonGetString(json, "evidence", "");
        Boolean pipelineEffective = hasJsonKey(json, "effective")
                ? jsonGetBool(json, "effective", false) : null;
        double p95 = jsonGetDouble(json, "benchmarkP95Ms", -1.0);

        // 模型来源（P1-6）：优先使用任务14 的 FaceModelManager 状态（其内部已合并 native
        // getRuntimeStatus 的 profileLoaded），给出「用户导入/未导入/不可用」；未导入时显式
        // 展示缺失原因（默认 missing_model），绝不静默回退 LBP。
        // #3（第三波审查）：此处为 UI 线程调用点，改用无锁的 statusSnapshot() 读取后台快照，
        // 绝不在 UI 线程调用持目录锁的 status()（导入/切换持锁期间会整段阻塞 -> ANR）。
        String modelLabel;
        boolean modelImported;
        // #3-Low-1（第三波审查）：状态快照尚未产出（LOADING）时置 true，用于抑制「未导入/缺失模型」
        // 结论，避免能力行与「正在读取模型状态…」自相矛盾（待状态读取完成后再判定缺失模型）。
        boolean modelLoading = false;
        String modelDetail = "";
        try {
            FaceRuntimeStatus rs = modelManager().statusSnapshot();
            modelLabel = rs.sourceLabel();
            modelImported = rs.isUserImported();
            if (rs.isReady()) {
                modelDetail = " profile=" + rs.profileId + " dim=" + rs.nativeEmbeddingDim
                        + (rs.importedProfileCount > 1 ? " imported=" + rs.importedProfileCount : "");
            } else if (rs.state == FaceRuntimeStatus.State.ERROR) {
                // 已导入但校验失败：直接展示管理器的中文原因，便于定位高精度路径不兼容。
                modelDetail = " 缺失原因=" + (rs.message.isEmpty() ? "runtime_init_failed" : rs.message);
            } else if (rs.isLoading()) {
                // #3-Low-1（第三波审查）：首帧占位（后台快照尚未产出）显式标「加载中」，
                // 绝不臆测为「未导入」——否则冷启动最多 2s 会误显「模型来源=未导入」。
                modelDetail = " 正在读取模型状态…";
                modelLoading = true;
            } else {
                modelDetail = " 缺失原因=" + jsonGetString(json, "missingReason", "missing_model");
            }
        } catch (Throwable t) {
            // 模型管理器不可用（文件/依赖异常）时退回 native profileLoaded 判定，行为保守。
            modelImported = jsonGetBool(json, "modelImported", profileLoaded);
            modelLabel = modelImported ? "用户导入" : "未导入";
            if (modelImported) {
                modelDetail = (profileId.isEmpty() ? "" : " profile=" + profileId) + " dim=" + dim;
            } else {
                modelDetail = " 缺失原因=" + jsonGetString(json, "missingReason", "missing_model");
            }
        }

        StringBuilder sb = new StringBuilder();
        if (!nativeNote.isEmpty()) {
            sb.append(nativeNote).append('\n');
        }
        sb.append("模型来源=").append(modelLabel).append(modelDetail);
        if (!modelImported && !modelLoading) {
            // 无模型时显式给出缺失原因，不静默回退 LBP；加载中不臆断缺失（见上）。
            sb.append("（不静默回退 LBP）");
        }
        sb.append(" gallery=").append(galleryCount).append(" enroll=").append(enrollState);

        // requested 一律取 UI/偏好，与 initEngine 写入的 RK_USE_* 环境变量保持一致。
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        boolean reqOpencl = switchAccelOpenCL != null
                ? switchAccelOpenCL.isChecked() : prefs.getBoolean(PREF_ACCEL_OPENCL, false);
        boolean reqMpp = switchAccelMpp != null
                ? switchAccelMpp.isChecked() : prefs.getBoolean(PREF_ACCEL_MPP, false);
        boolean reqQualcomm = switchAccelQualcomm != null
                ? switchAccelQualcomm.isChecked() : prefs.getBoolean(PREF_ACCEL_QUALCOMM, false);
        // libyuv 无 UI 开关，initEngine 默认按 true 传入 RK_USE_LIBYUV，但编译期已 OFF。
        boolean reqLibyuv = prefs.getBoolean(PREF_ACCEL_LIBYUV, true);

        // ncnn：主推（首推）检测后端 —— 规则「ncnn 首推才 effective」：仅主推后端可标为生效，
        // 且只有用户导入模型已激活时才生效。若 native 显式暴露 effective（管道级或 accelerators
        // 嵌套），则以 native 为准（capLine 内部优先级：嵌套 > 派生）。
        // #3-Low-1（第三波审查）：后台状态快照尚未产出（modelLoading）时不能断言「无用户导入模型」，
        // 否则与「模型来源=加载中」自相矛盾；此时 installed 标 ?、reason=loading，待状态读取完成再判定。
        Boolean ncnnEffective;
        String ncnnReason;
        String ncnnEvidence;
        if (modelLoading) {
            // native 若已显式报告管道级 effective 仍以其为准；否则保持未知（不派生）。
            ncnnEffective = pipelineEffective;
            ncnnReason = "loading";
            ncnnEvidence = "模型状态加载中";
        } else {
            ncnnEffective = (pipelineEffective != null) ? pipelineEffective : modelImported;
            if (!modelImported) {
                ncnnReason = "missing_model";
            } else if (!pipelineReason.isEmpty()) {
                ncnnReason = pipelineReason;
            } else {
                ncnnReason = "ok";
            }
            ncnnEvidence = !modelImported
                    ? "无用户导入模型"
                    : (pipelineEvidence.isEmpty() ? "首推后端；模型已激活" : pipelineEvidence);
        }
        sb.append('\n').append(capLine(json, "ncnn", "ncnn", true,
                null, modelLoading ? null : Boolean.valueOf(modelImported),
                ncnnEffective, ncnnReason, ncnnEvidence));
        // MPP：仅标注编码文件路径作为 evidence；不因 compiled=true 就标 effective。
        String mppPath = jsonGetCapString(json, "mpp", "evidence", null);
        if (mppPath == null || mppPath.isEmpty()) {
            mppPath = jsonGetCapString(json, "mpp", "encoderFile", null);
        }
        sb.append('\n').append(capLine(json, "mpp", "MPP", reqMpp,
                null, false, null, "missing_dependency",
                (mppPath == null || mppPath.isEmpty()) ? "编码文件=未暴露" : mppPath));
        // libyuv：标准 Gradle 构建 -DRK_ENABLE_LIBYUV=OFF，编译期禁用 -> build_disabled。
        sb.append('\n').append(capLine(json, "libyuv", "libyuv", reqLibyuv,
                LIBYUV_COMPILED_IN_BUILD, LIBYUV_COMPILED_IN_BUILD, null, "build_disabled",
                "Gradle -DRK_ENABLE_LIBYUV=OFF"));
        // OpenCL / Qualcomm：编译进来也不等于生效，默认 missing_dependency。
        sb.append('\n').append(capLine(json, "opencl", "OpenCL", reqOpencl,
                null, false, null, "missing_dependency", "未暴露"));
        sb.append('\n').append(capLine(json, "qualcomm", "Qualcomm", reqQualcomm,
                null, false, null, "missing_dependency", "未暴露"));

        // 最近推理耗时或 p95：优先 native 基准 p95；缺失时回退 StatsRepository 最近一次延迟。
        Double recentLatency = null;
        try {
            recentLatency = StatsRepository.getInstance().getSnapshot().latencyMs;
        } catch (Throwable ignored) {
        }
        sb.append('\n').append("推理: ");
        if (p95 >= 0.0) {
            sb.append(String.format(Locale.US, "p95=%.0fms", p95));
        } else if (recentLatency != null) {
            sb.append(String.format(Locale.US, "最近耗时=%.0fms", recentLatency));
        } else {
            sb.append("耗时=--");
        }
        if (p95 >= 0.0 && recentLatency != null) {
            sb.append(String.format(Locale.US, " 最近=%.0fms", recentLatency));
        }

        // 高精度路径不兼容/基准失败：显式给出原因，并说明保留轻量后端。
        if (modelImported && !pipelineReason.isEmpty() && !"ok".equals(pipelineReason)) {
            sb.append("\n提示: 高精度路径未生效 reason=").append(pipelineReason)
              .append("，保留轻量后端");
        }
        return sb.toString();
    }

    /**
     * 生成单条能力状态行。优先采用 JSON 中 {@code accelerators.<cap>.*}（或顶层 cap 对象）字段；
     * 缺失时用调用方派生的保守值。effective 只在 native 明确置 true（或主推后端派生为 true）时为 Y，
     * 杜绝“compiled 即 effective”的误报。{@code installedDerived} 允许传 {@code null} 表示「未知」
     * （如模型状态仍在加载），此时 installed 渲染为 {@code ?}，绝不臆断为未安装。
     */
    private String capLine(String json, String cap, String label, boolean requested,
                           Boolean compiledDerived, Boolean installedDerived,
                           Boolean effectiveDerived, String reasonDerived, String evidenceDerived) {
        Boolean compiledJson = jsonGetCapBool(json, cap, "compiled");
        Boolean installedJson = jsonGetCapBool(json, cap, "installed");
        Boolean requestedJson = jsonGetCapBool(json, cap, "requested");
        Boolean effectiveJson = jsonGetCapBool(json, cap, "effective");
        String reasonJson = jsonGetCapString(json, cap, "reason", null);
        String evidenceJson = jsonGetCapString(json, cap, "evidence", null);

        boolean req = (requestedJson != null) ? requestedJson : requested;
        String compiled = (compiledJson != null) ? yn(compiledJson)
                : (compiledDerived != null ? yn(compiledDerived) : "?");
        String installed = (installedJson != null) ? yn(installedJson)
                : (installedDerived != null ? yn(installedDerived) : "?");
        boolean eff;
        if (effectiveJson != null) {
            eff = effectiveJson;
        } else {
            eff = (effectiveDerived != null) && effectiveDerived;
        }
        String reason = (reasonJson != null) ? reasonJson : reasonDerived;
        String evidence = (evidenceJson != null) ? evidenceJson : evidenceDerived;
        return label + " compiled=" + compiled + " installed=" + installed
                + " req=" + yn(req) + " eff=" + yn(eff)
                + " reason=" + reason + " ev=" + sanitizeStatusDetail(evidence);
    }

    private static String yn(boolean v) {
        return v ? "Y" : "N";
    }

    /** 单行化并截断 evidence，避免长文本（含换行）撑爆状态区。 */
    private static String sanitizeStatusDetail(String s) {
        if (s == null) return "";
        String flat = s.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ').trim();
        return flat.length() > 96 ? flat.substring(0, 96) + "…" : flat;
    }

    /** 判断 flat JSON 中是否存在某顶层键。 */
    private static boolean hasJsonKey(String json, String key) {
        return jsonExtractRawValue(json, key) != null;
    }

    // ---------- 轻量 JSON 读取（仅用于解析 getRuntimeStatus 的扁平小对象） ----------

    /** 读取字符串字段；缺失或非法返回 def。非字符串 token 按字面返回。 */
    private static String jsonGetString(String json, String key, String def) {
        String raw = jsonExtractRawValue(json, key);
        if (raw == null) return def;
        if (raw.length() >= 2 && raw.charAt(0) == '"' && raw.charAt(raw.length() - 1) == '"') {
            return jsonUnescape(raw.substring(1, raw.length() - 1));
        }
        return raw;
    }

    private static boolean jsonGetBool(String json, String key, boolean def) {
        String raw = jsonExtractRawValue(json, key);
        if (raw == null) return def;
        if (raw.startsWith("true")) return true;
        if (raw.startsWith("false")) return false;
        return def;
    }

    private static long jsonGetLong(String json, String key, long def) {
        String raw = jsonExtractRawValue(json, key);
        if (raw == null) return def;
        try {
            return Long.parseLong(raw.trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }

    private static double jsonGetDouble(String json, String key, double def) {
        String raw = jsonExtractRawValue(json, key);
        if (raw == null) return def;
        try {
            return Double.parseDouble(raw.trim());
        } catch (NumberFormatException e) {
            return def;
        }
    }

    /** 读取 accelerators.<cap>.<key>（或顶层 <cap>.<key>）字符串；缺失返回 def。 */
    private static String jsonGetCapString(String json, String cap, String key, String def) {
        String sub = capObject(json, cap);
        return sub == null ? def : jsonGetString(sub, key, def);
    }

    /** 读取 accelerators.<cap>.<key>（或顶层 <cap>.<key>）布尔；缺失返回 null（区别于 false）。 */
    private static Boolean jsonGetCapBool(String json, String cap, String key) {
        String sub = capObject(json, cap);
        if (sub == null || jsonExtractRawValue(sub, key) == null) return null;
        return jsonGetBool(sub, key, false);
    }

    /** 定位 cap 对应对象：优先 accelerators 下的同名对象，回退顶层同名对象。实现见 {@link JsonText#capObject}。 */
    private static String capObject(String json, String cap) {
        return JsonText.capObject(json, cap);
    }

    /**
     * 提取 "key": 后的原始值 token（字符串含引号）；<b>仅匹配顶层对象键</b>，缺失返回 null。
     *
     * <p>限定顶层是为了避免误取 {@code accelerators.<cap>.*} 内的同名键（如 effective/reason）：
     * 嵌套取值必须先 {@link JsonText#extractObject}（{@link #capObject}）缩小到子对象体，
     * 再对子对象体调用本方法——此时子对象体的键仍处于其自身顶层。
     */
    private static String jsonExtractRawValue(String json, String key) {
        return JsonText.extractRawValue(json, key);
    }

    /**
     * 纯文本 JSON 取值工具：不依赖 Activity 实例与 Android API，抽成静态嵌套类以便
     * JVM 单测直接覆盖（访问嵌套类不会触发 MainActivity 的静态初始化 / native 加载）。
     *
     * <p>⚠️ **边界如实说明**：{@link #extractRawValue}/{@link #extractObject} 是**通用**扫描；
     * 而 {@link #capObject} 内嵌了「{@code accelerators.<cap>}」这一**加速能力矩阵的业务结构**，
     * 严格说超出了「纯工具」的职责（子代理 4 审计 P5 指出）。之所以仍放在此处，是因为本类是
     * `MainActivity` 的私有嵌套类、仅为状态区渲染与可测性服务，并非对外通用 API；
     * 且该分支在现行生产数据上恒为 null（见 {@link #capObject}）。若将来它被复用或长大，
     * 应迁到独立的能力矩阵领域类。
     */
    static final class JsonText {

        private JsonText() {
        }

        /** 提取 {@code "key":} 后的原始值 token（字符串含引号，保留转义）；仅匹配顶层键。 */
        static String extractRawValue(String json, String key) {
            if (json == null || key == null) return null;
            String needle = "\"" + key + "\"";
            int idx = indexOfTopLevelKey(json, needle);
            if (idx < 0) return null;
            int i = idx + needle.length();
            while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
            if (i >= json.length() || json.charAt(i) != ':') return null;
            i++;
            while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
            if (i >= json.length()) return null;
            char c = json.charAt(i);
            if (c == '"') {
                StringBuilder sb = new StringBuilder();
                int j = i + 1;
                while (j < json.length()) {
                    char ch = json.charAt(j);
                    if (ch == '\\' && j + 1 < json.length()) {
                        sb.append(ch).append(json.charAt(j + 1));
                        j += 2;
                        continue;
                    }
                    if (ch == '"') {
                        return "\"" + sb + "\"";
                    }
                    sb.append(ch);
                    j++;
                }
                return null;  // 引号未闭合
            }
            int j = i;
            while (j < json.length()) {
                char ch = json.charAt(j);
                if (ch == ',' || ch == '}' || ch == ']' || Character.isWhitespace(ch)) break;
                j++;
            }
            return j > i ? json.substring(i, j) : null;
        }

        /**
         * 提取 {@code "key":} 后**顶层**对象的值子串（含花括号；按花括号配平、忽略字符串内的括号）；
         * 键缺失、或该键的值不是对象时返回 null。
         *
         * <p><b>为什么必须限定顶层（§2.5-G）</b>：旧实现用整串 {@code indexOf} 取**首个** {@code "key"}，
         * 于是两种情形会取到错的对象——
         * <ol>
         *   <li>更早处存在**同名嵌套键**（如 {@code {"other":{"mpp":…},"mpp":…}}）；</li>
         *   <li>更早处有**字符串值恰好等于该键名**（值自身的引号构成 {@code "key"} 字面量，
         *       如 {@code {"note":"mpp",…,"mpp":{…}}}）。</li>
         * </ol>
         * 这与 {@link #extractRawValue} 的顶层纪律同源；两条取值路径的保护强度自此对齐。
         *
         * <p>与 {@link #capObject} 的配合：当 {@code accelerators} **存在**时，{@code capObject} 会先把它的
         * 对象体取出、再在该本体上找 cap；对象体自带花括号，故其键仍处于自身顶层 —— 本方法对其适用。
         * ⚠️ 但 native 侧当前**并不输出** {@code accelerators}（见 {@link #capObject} 的现状说明），
         * 故这条配合在**现行生产数据上不会被走到**。
         */
        static String extractObject(String json, String key) {
            if (json == null || key == null) return null;
            String needle = "\"" + key + "\"";
            int idx = indexOfTopLevelKey(json, needle);
            if (idx < 0) return null;
            int i = idx + needle.length();
            while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
            if (i >= json.length() || json.charAt(i) != ':') return null;
            i++;
            while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
            if (i >= json.length() || json.charAt(i) != '{') return null;

            int open = i;
            int depth = 0;
            boolean inStr = false;
            boolean esc = false;
            for (int j = open; j < json.length(); j++) {
                char ch = json.charAt(j);
                if (inStr) {
                    if (esc) {
                        esc = false;
                    } else if (ch == '\\') {
                        esc = true;
                    } else if (ch == '"') {
                        inStr = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    inStr = true;
                } else if (ch == '{') {
                    depth++;
                } else if (ch == '}') {
                    depth--;
                    if (depth == 0) return json.substring(open, j + 1);
                }
            }
            return null;
        }

        /**
         * 定位某能力（cap）对应的对象体：优先 {@code accelerators.<cap>}，回退**顶层** {@code <cap>}；
         * 两者皆无则返回 null。
         *
         * <p>⚠️ <b>现状（勿高估覆盖）</b>：native 侧 {@code NativeFaceBridge.getRuntimeStatus()}
         * <b>当前并不输出 {@code accelerators}，也不输出任何 cap 对象</b>——它返回的顶层是
         * {@code ok/profileValidated/profileLoaded/profileId/embeddingDim/galleryLoaded/galleryCount/
         * galleryDim/enrollmentState/effective/reason}（见 {@code jni/native_face_bridge.cpp} 的
         * {@code bridge_getRuntimeStatus}）。⇒ 本方法在**现行生产数据上恒返回 null**，
         * {@code jsonGetCapString}/{@code jsonGetCapBool} 一律回落到默认值。
         * 本方法是为「native 后续暴露该形态」预留的**前向兼容**路径，**不是**已生效的生产路径。
         */
        static String capObject(String json, String cap) {
            if (json == null) return null;
            String acc = extractObject(json, "accelerators");
            if (acc != null) {
                String capObj = extractObject(acc, cap);
                if (capObj != null) return capObj;
            }
            return extractObject(json, cap);
        }

        /**
         * 只在顶层容器（深度 1）内定位键 token 的起始引号下标；未命中返回 -1。
         *
         * <p>顶层键后必须紧跟 {@code ':'}，从而把同名嵌套键（深度 &gt; 1）与字符串值排除在外，
         * 不会因 {@code indexOf} 的首个命中而误读管道级字段。
         */
        private static int indexOfTopLevelKey(String json, String needle) {
            int depth = 0;
            boolean inStr = false;
            boolean esc = false;
            for (int i = 0; i < json.length(); i++) {
                char ch = json.charAt(i);
                if (inStr) {
                    if (esc) {
                        esc = false;
                    } else if (ch == '\\') {
                        esc = true;
                    } else if (ch == '"') {
                        inStr = false;
                    }
                    continue;
                }
                if (ch == '"') {
                    if (depth == 1 && json.startsWith(needle, i)) {
                        int j = i + needle.length();
                        while (j < json.length() && Character.isWhitespace(json.charAt(j))) j++;
                        if (j < json.length() && json.charAt(j) == ':') {
                            return i;
                        }
                    }
                    inStr = true;
                } else if (ch == '{' || ch == '[') {
                    depth++;
                } else if (ch == '}' || ch == ']') {
                    depth--;
                    if (depth <= 0) {
                        return -1;  // 顶层容器已闭合仍未命中
                    }
                }
            }
            return -1;
        }
    }

    /** 解转义（与 native jsonEscape 对齐，仅处理常用控制字符）。 */
    private static String jsonUnescape(String s) {
        if (s.indexOf('\\') < 0) return s;
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) {
                out.append(c);
                continue;
            }
            char n = s.charAt(++i);
            switch (n) {
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case '"': out.append('"'); break;
                case '\\': out.append('\\'); break;
                default: out.append(n); break;
            }
        }
        return out.toString();
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
                final int scheduledGen = se.generation;
                handler.postDelayed(() -> {
                    // 取消门：代次已变（Activity 停止/撤权/销毁）则陈旧任务不执行
                    if (!monitoringCoordinator.isCurrentGeneration(scheduledGen)) return;
                    // isStopped 门补充：重启/恢复排程在 onStop 后到期不得后台启动（R2-6 双审 P2）
                    if (isStopped) return;
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
                final int scheduledGen = ss.generation;
                handler.postDelayed(() -> {
                    // 取消门：代次已变（Activity 停止/撤权/销毁）则陈旧任务不执行
                    if (!monitoringCoordinator.isCurrentGeneration(scheduledGen)) return;
                    // isStopped 门补充：重启/恢复排程在 onStop 后到期不得后台启动（R2-6 双审 P2）
                    if (isStopped) return;
                    if (isFinishing()) return;
                    dispatchMonitoringDecision(monitoringCoordinator.onStartRequested(snapshotMonitoringInputs(), null));
                }, Math.max(0L, ss.delayMs));
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
        // 启动时统一同步 CameraFragment 相机选择（覆盖启动恢复/mock 回退等路径，
        // 而不仅 onItemSelected），确保翻转预置 key（getFlipSourceKey）与 MainActivity 一致
        if (cameraFragment != null) {
            cameraFragment.setSelectedCameraId(selectedCameraId);
        }

        firstFrameReceived = false;
        updateSystemReadyUi("开始监控");

        if (wantCamera) {
            captureEverPushed = false;
            pushFailStreak = 0;
            lastPushOkRealtimeMs = 0L;

            nativeConfigureExternalInput(true, 0, 1);
            activeCaptureScheme = s.schemeOrNull == null ? CaptureScheme.CAMERA2 : s.schemeOrNull;
            activeCapture = (activeCaptureScheme == CaptureScheme.CAMERAX) ? cameraXCapture : camera2Capture;
            boolean started = activeCapture != null && activeCapture.start(selectedCameraId);
            if (!started) {
                String capName = activeCapture == null ? "N/A" : activeCapture.name();
                nativeConfigureExternalInput(false, 0, 1);
                activeCapture = null;
                updateSystemReadyUi("采集启动失败");
                return monitoringCoordinator.onMonitoringStartFailed(snapshotMonitoringInputs(), "启动采集失败: " + capName);
            }
        }

        nativeStart();
        // 暂停时 native stopAndJoinEngineThreadIfRunning 已释放 g_previewWindow；
        // 恢复 start 必须重新注册预览 surface，否则渲染线程恒失败 → 预览卡在暂停帧（回归修复）
        if (previewSurfaceReady && previewSurface != null) {
            nativeSetPreviewSurface(previewSurface.getHolder().getSurface());
        }
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
        selectedCameraId = "0";
        mockFilePath = null;
        isSpinnerInitialized = true;
        refreshFlipFromPrefs(true);
        if (isRunning) {
            stopMonitoring();
            scheduleStartMonitoring(500);
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

        CameraFragment.CameraInfo info = availableCameras.get(position);
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
            if (info.id.equals(selectedCameraId)) return;

            selectedCameraId = String.valueOf(newId);
            mockFilePath = null;
            // 同步 CameraFragment 的相机选择，使翻转预置 key（getFlipSourceKey）随相机切换
            if (cameraFragment != null) {
                cameraFragment.setSelectedCameraId(selectedCameraId);
            }
            refreshFlipFromPrefs(true);
            AppLog.i("MainActivity", "onItemSelected", "切换 Camera ID: " + selectedCameraId);

            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                    .edit()
                    .putInt(PREF_CAMERA_ID, Integer.parseInt(selectedCameraId))
                    .apply();

            if (isRunning) {
                stopMonitoring();
                scheduleStartMonitoring(500);
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
    public native boolean nativeInit(String cameraId, String cascadePath, String storagePath);
    public native boolean nativeInitFile(String filePath, String cascadePath, String storagePath);
    public native void nativeStart();
    public native void nativeStop();
    public native void nativeRequestCancelInit();
    public native void nativeSetMode(int mode);
    public native void nativeSetFlip(boolean flipX, boolean flipY);
    public native long nativeGetFlipGeneration();
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
    /** 任务18：SAF 选择模型 ZIP 包。 */
    private static final int REQUEST_CODE_IMPORT_MODEL = 2003;
    /** 注册模板的模型版本 / schema 版本（仓库元数据字段，落盘一致性由仓库保证）。 */
    private static final int ENROLL_MODEL_VERSION = 1;
    private static final int ENROLL_TEMPLATE_SCHEMA_VERSION = 1;
    /** 注册进度轮询间隔：250ms，仅刷新 UI，不在 UI 线程做重 IO。 */
    private static final long ENROLLMENT_POLL_INTERVAL_MS = 250L;
    private String mockFilePath = null;
    private String currentPhotoPath;
    // 拍照输出 URI：授予跨 UID 相机后，消费完成须及时撤销（Q-CQ-B07-009）
    private Uri currentPhotoUri;

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
        selectedCameraId = String.valueOf(prefs.getInt(PREF_CAMERA_ID, 0));
        recognitionEventsVisible = prefs.getBoolean(PREF_EVENTS_VISIBLE, true);
        captureAutoEnabled = prefs.getBoolean(PREF_CAPTURE_AUTO, true);
        String schemeRaw = prefs.getString(PREF_CAPTURE_SCHEME, CaptureScheme.CAMERA2.name());
        preferredCaptureScheme = EngineViewModel.parseCaptureScheme(schemeRaw, CaptureScheme.CAMERA2);
        loadInferenceThrottleSettings(prefs);

        cameraAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        cameraAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);

        // cameraFragment 必须先于 bindMainScreen 初始化：bindMainScreen → refreshFlipFromPrefs
        // 会访问 cameraFragment.getFlipSourceKey()，此处若为 null 则启动即 NPE 闪退（bf9b009 引入）
        cameraFragment = new CameraFragment();
        getSupportFragmentManager().beginTransaction()
            .add(cameraFragment, "camera_fragment")
            .commitNow();

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

        cameraFragment.setCallback(new CameraFragment.Callback() {
            @Override
            public void onCameraChanged() {
                refreshFlipFromPrefs(true);
                if (isRunning) {
                    stopMonitoring();
                    scheduleStartMonitoring(500);
                } else {
                    initEngine();
                }
            }
            @Override
            public void onInitEngine() { initEngine(); }
            @Override
            public void onStopMonitoring() { stopMonitoring(); }
            @Override
            public void onStartMonitoring(long delayMs) {
                scheduleStartMonitoring(delayMs);
            }
            @Override
            public void onCameraSpinnerInitialized(boolean initialized) {
                isSpinnerInitialized = initialized;
            }
        });
        engineViewModel = new ViewModelProvider(this).get(EngineViewModel.class);

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
                // 任务15：沿用本循环 500ms 节奏，但能力状态只按 2s 节流刷新（p95/最近耗时保持新鲜，
                // 又不逐帧查询 JNI）；FPS/CAP/LAT/CPU/MEM 的既有节流不变。
                if (refreshRuntimeStatusIfDue(false)) {
                    updateDiagnosticsText();
                }
                handler.postDelayed(this, 500);
            }
        };
        handler.post(statsUpdater);

        // #1（第二波审查）：启动恢复已导入并激活的模型 profile，使实时管线在重启后生效。
        // 全程后台线程执行、失败不阻塞启动；结果只更新状态区文本。
        restoreActiveProfileOnStartup();
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
        } else if (requestCode == REQUEST_CODE_TAKE_PHOTO) {
            if (resultCode == RESULT_OK) {
                handleSystemCameraResult();
            }
            // 无论拍照成功与否，使用后都及时撤销跨 UID 授权（Q-CQ-B07-009）
            revokePhotoUriPermission();
        } else if (requestCode == 1002) {
            syncOverlaySwitchState();
        } else if (requestCode == REQUEST_CODE_IMPORT_MODEL && resultCode == RESULT_OK) {
            // 任务18 / #1：SAF 选择模型 ZIP 包 -> 后台导入（不在 UI 线程做重 IO）。
            if (data != null && data.getData() != null) {
                importModelZip(data.getData());
            }
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
                currentPhotoUri = photoURI; // 记录 URI，消费完成后撤销（Q-CQ-B07-009）
                // WHY 单一来源：授权组装（flags + ClipData + EXTRA_OUTPUT）收敛到 PhotoUriGrant.applyTakePictureGrant
                PhotoUriGrant.applyTakePictureGrant(takePictureIntent, photoURI);
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
        selectedCameraId = null; // Mock Flag
        
        AppLog.i("MainActivity", "handleSystemCameraResult", "Photo captured: " + mockFilePath);
        Toast.makeText(this, "Photo Captured", Toast.LENGTH_SHORT).show();

        if (isRunning) {
            stopMonitoring();
            scheduleStartMonitoring(500);
        } else {
            initEngine();
        }
        revokePhotoUriPermission(); // 消费完成，及时撤销跨 UID 授权（Q-CQ-B07-009）
    }

    /**
     * 撤销跨 UID 拍照授权：幂等，以与授予一致的掩码回收读写权限，避免授权残留（Q-CQ-B07-009）。
     */
    private void revokePhotoUriPermission() {
        if (currentPhotoUri == null) return;
        try {
            // WHY 单一来源：撤销调用收敛到 PhotoUriGrant.revoke，掩码与授予一致（Q-CQ-B07-009）
            PhotoUriGrant.revoke(this, currentPhotoUri);
        } catch (Exception ex) {
            AppLog.e("MainActivity", "revokePhotoUriPermission", "Revoke failed", ex);
        } finally {
            currentPhotoUri = null; // 无论成败都清空引用，保证只撤销一次
        }
    }

    private void handleMockFileSelection(Uri uri) {
        if (uri == null) return;

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
                ext = CameraFragment.sanitizeExtension(ext);

                // ---- Magic number pre-check ----
                String magicCheckErr = null;
                try {
                    java.io.InputStream magicIs = getContentResolver().openInputStream(uri);
                    if (magicIs != null) {
                        try {
                            byte[] magic = new byte[16];
                            int magicN = magicIs.read(magic);
                            String hexMagic = CameraFragment.bytesToHex(magic, Math.max(0, magicN));
                            AppLog.i("MockMode", "handleMockFileSelection", "Magic bytes: " + hexMagic + " ext=" + ext);
                            if (!CameraFragment.isValidMagicNumber(magic, magicN, ext)) {
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
                final long[] lastUiMs = {0L};
                boolean ok = false;
                String err = null;

                AppLog.i("MockMode", "handleMockFileSelection",
                    "Copy start: name=" + saveName + " ext=" + ext +
                    " totalBytes=" + totalBytes + " dest=" + outFile.getAbsolutePath());

                java.io.InputStream is = null;
                java.io.BufferedInputStream bis = null;
                java.io.FileOutputStream fos = null;
                // totalBytes 在查询块内被重赋值，故为进度回调取一份 final 快照（仅用于展示）。
                final long totalForProgress = totalBytes;
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
                    CameraFragment.BoundedStreamCopy.Result r = CameraFragment.BoundedStreamCopy.copy(
                            bis, fos, buffer,
                            CameraFragment.MAX_MOCK_IMPORT_BYTES,
                            CameraFragment.MAX_MOCK_IMPORT_MILLIS,
                            cancelled::get,
                            SystemClock::elapsedRealtime,
                            p -> {
                                long nowMs = SystemClock.elapsedRealtime();
                                if (nowMs - lastUiMs[0] >= 350) {
                                    lastUiMs[0] = nowMs;
                                    long elapsedMs = Math.max(1L, nowMs - startMs);
                                    double speed = (p.bytesCopied / 1024.0) / (elapsedMs / 1000.0);
                                    String msg = "已复制 " + CameraFragment.formatBytes(p.bytesCopied) +
                                            (totalForProgress > 0 ? (" / " + CameraFragment.formatBytes(totalForProgress)) : "") +
                                            "  速度 " + String.format(Locale.US, "%.1f", speed) + " KB/s";
                                    int progressKb = (int) Math.min(Integer.MAX_VALUE, Math.max(0L, p.bytesCopied / 1024L));
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
                            });

                    fos.flush();
                    copied = r.bytesCopied;
                    if (r.status == CameraFragment.BoundedStreamCopy.Status.OK) {
                        ok = true;
                    } else {
                        err = CameraFragment.BoundedStreamCopy.messageFor(r.status);
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
                    selectedCameraId = null;
                    isSpinnerInitialized = true;
                    refreshFlipFromPrefs(true);

                    AppLog.i("MainActivity", "handleMockFileSelection",
                            "Mock file ready: " + finalOutPath + " copied=" + finalCopied + " name=" + finalDisplayName);
                    Toast.makeText(MainActivity.this, "Mock 源已就绪: " + finalDisplayName, Toast.LENGTH_SHORT).show();

                    if (isRunning) {
                        stopMonitoring();
                        scheduleStartMonitoring(500);
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

    // 统一入口：延迟启动监控（stop 后重启 / Surface 恢复 / 相机切换 / Mock 切换等），携代次排程。
    // 调度前先拦截 Activity 已销毁/正在结束的无效调度；回调内取消门：
    // 代次已变（用户 STOP/撤权/Activity 停止/销毁）则陈旧触发不执行。
    private void scheduleStartMonitoring(long delayMs) {
        if (isFinishing() || isDestroyed() || isStopped) return;
        final int scheduledGen = monitoringCoordinator.currentGeneration();
        handler.postDelayed(() -> {
            // 取消门：代次已变（Activity 停止/撤权/销毁）则陈旧任务不执行
            if (!monitoringCoordinator.isCurrentGeneration(scheduledGen)) return;
            if (isFinishing() || isDestroyed() || isStopped) return;
            startMonitoring();
        }, Math.max(0L, delayMs));
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
            CameraCharacteristics chars = mgr.getCameraCharacteristics(selectedCameraId);
            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes = map != null ? map.getOutputSizes(ImageFormat.YUV_420_888) : null;
            Range<Integer>[] fpsRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);

            Size chosen = CameraFragment.chooseBestSizeNoMoreThan1080p(sizes);
            Range<Integer> fps = CameraFragment.chooseBestFpsNoMoreThan60(fpsRanges);

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
            Uri parsed = Uri.parse(path);
            String scheme = parsed.getScheme();
            if (scheme != null && !scheme.isEmpty() && !"file".equalsIgnoreCase(scheme)) {
                return "Mock预检: URL 源（无法预读元数据，运行时将自动降档到≤1080p60，推理640x480）";
            }

            File f = toSafeAppFile(path);
            if (f == null) return "Mock预检失败：文件路径不受信任";
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

    private File toSafeAppFile(String path) {
        try {
            File f = new File(path);
            File canonical = f.getCanonicalFile();
            File cacheDir = getCacheDir() == null ? null : getCacheDir().getCanonicalFile();
            File filesDir = getFilesDir() == null ? null : getFilesDir().getCanonicalFile();
            if (isUnderDir(canonical, cacheDir) || isUnderDir(canonical, filesDir)) {
                return canonical;
            }
        } catch (Throwable ignored) {
        }
        return null;
    }

    private boolean isUnderDir(File target, File base) {
        if (target == null || base == null) return false;
        try {
            String basePath = base.getCanonicalPath();
            String targetPath = target.getCanonicalPath();
            return targetPath.equals(basePath) || targetPath.startsWith(basePath + File.separator);
        } catch (Throwable ignored) {
            return false;
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
            android.system.Os.setenv("RK_USE_LIBYUV", prefs.getBoolean(PREF_ACCEL_LIBYUV, true) ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_MPP", prefs.getBoolean(PREF_ACCEL_MPP, false) ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_QUALCOMM", prefs.getBoolean(PREF_ACCEL_QUALCOMM, false) ? "1" : "0", true);
        } catch (Exception e) {
            AppLog.e("MainActivity", "initEngine", "Failed to setenv for acceleration", e);
        }

        boolean wantCamera = (selectedCameraId != null && mockFilePath == null);
        boolean wantMock = (selectedCameraId == null && mockFilePath != null);
        if (wantCamera && permissionStateMachine != null && !permissionStateMachine.isRuntimeGranted()) {
            engineInitialized = false;
            firstFrameReceived = false;
            updateSystemReadyUi("权限不足，禁止初始化引擎");
            return;
        }

        String cascadePath = engineViewModel.copyAssetToCache("lbpcascade_frontalface.xml");
        String storagePath = engineViewModel.getAppStoragePath();
        
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
            engineInitialized = nativeInit(NO_CAMERA_ID, cascadePath, storagePath);
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

    private void refreshFlipFromPrefs(boolean applyToNativeIfReady) {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        String sourceKey = cameraFragment.getFlipSourceKey();
        String kx = PREF_FLIP_X_PREFIX + sourceKey;
        String ky = PREF_FLIP_Y_PREFIX + sourceKey;

        boolean hasX = prefs.contains(kx);
        boolean hasY = prefs.contains(ky);

        boolean defaultX = false;
        if (selectedCameraId != null && mockFilePath == null) {
            defaultX = cameraFragment.isFrontCamera(selectedCameraId);
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

    private boolean isFrontCamera(String camId) {
        try {
            android.hardware.camera2.CameraManager mgr = (android.hardware.camera2.CameraManager) getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) return false;
            android.hardware.camera2.CameraCharacteristics chars = mgr.getCameraCharacteristics(camId);
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
        // 任务3 诊断：翻转开关变化即刷新 flipGen 展示（renderMonitoringUi 事件驱动不覆盖此场景）
        updateDiagnosticsText();
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
            AppLog.i("MainActivity", "capture", "首帧推入 ok tsNs=" + ts + " src=" + src + " camId=" + (selectedCameraId != null ? selectedCameraId : "null"));
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
                boolean wantCamera = (selectedCameraId != null && mockFilePath == null);
                boolean wantMock = (selectedCameraId == null && mockFilePath != null);
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
                " camId=" + (selectedCameraId != null ? selectedCameraId : "null") +
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
        boolean permissionOk = runtimeGranted || (selectedCameraId == null && mockFilePath != null);
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
        final String display = formatRecognitionResult(result);
        runOnUiThread(() -> {
            if (recognitionEventAdapter == null) return;
            recognitionEventAdapter.prepend(new RecognitionEvent(System.currentTimeMillis(), display));
            if (rvRecognitionEvents != null) {
                rvRecognitionEvents.scrollToPosition(0);
            }
        });
    }

    /**
     * 任务18：把原生识别结果格式化为可读文本并回填 displayName。
     *
     * <p>兼容两种契约：
     * <ul>
     *   <li>实时路径纯文本（{@code ResultPublisher}）：{@code VERIFIED <personId> <pct>%} /
     *       {@code AUTH_FAIL Unknown} / {@code NO_FACE} / {@code FACES n ...}。</li>
     *   <li>JSON 结果（FaceInferOutcomeJson 契约，含 {@code TopK.hits[].personId/displayName}）：
     *       优先取 hits 的 displayName，缺失时用 personId 映射本地展示名。</li>
     * </ul>
     * 无法识别时原样返回，绝不臆造身份。展示名映射仅存于 SharedPreferences，不含模板。
     */
    @NonNull
    private String formatRecognitionResult(@Nullable String raw) {
        if (raw == null) return "";
        String s = raw.trim();
        if (s.isEmpty()) return s;

        if (s.charAt(0) == '{') {
            // JSON 契约：displayName 优先（native 可能留空），否则按 personId 映射本地展示名。
            // #6：容忍字段缺失——native 未知时省略 personId/displayName，jsonFirstStringField 返回空串，
            // 两者皆缺则原样返回不再臆造身份；personId 缺失时仅展示 displayName，不拼接 id 后缀。
            String personId = jsonFirstStringField(s, "personId");
            String displayName = jsonFirstStringField(s, "displayName");
            if (personId.isEmpty() && displayName.isEmpty()) {
                return s;
            }
            String shown = !displayName.isEmpty() ? displayName
                    : (personId.isEmpty() ? "" : displayNameFor(personId));
            if (shown.isEmpty()) return s;
            return "识别: " + shown + (personId.isEmpty() ? "" : ("（" + personId + "）"));
        }

        if (s.startsWith("VERIFIED ")) {
            String[] t = s.split("\\s+");
            if (t.length >= 2 && !t[1].isEmpty()) {
                String suffix = (t.length >= 3) ? (" " + t[2]) : "";
                return "识别: " + displayNameFor(t[1]) + suffix;
            }
            return s;
        }
        if (s.startsWith("AUTH_FAIL")) return "未识别（Unknown）";
        if ("NO_FACE".equals(s)) return "无人脸";
        if (s.startsWith("FACES ")) {
            return "多人脸（" + s.substring(6).trim() + "）";
        }
        return s;
    }

    /**
     * 在 JSON 文本中查找首个任意深度的 {@code "key":"value"} 字符串字段；缺失/非法返回空串。
     * 仅用于读取状态型小对象（结果 JSON），不构造对象、不依赖字段顺序。
     */
    @NonNull
    private static String jsonFirstStringField(@Nullable String json, @NonNull String key) {
        if (json == null || key.isEmpty()) return "";
        String needle = "\"" + key + "\"";
        int idx = json.indexOf(needle);
        if (idx < 0) return "";
        int i = idx + needle.length();
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
        if (i >= json.length() || json.charAt(i) != ':') return "";
        i++;
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) i++;
        if (i >= json.length() || json.charAt(i) != '"') return "";
        StringBuilder sb = new StringBuilder();
        int j = i + 1;
        while (j < json.length()) {
            char c = json.charAt(j);
            if (c == '\\' && j + 1 < json.length()) {
                sb.append(json.charAt(j + 1));
                j += 2;
                continue;
            }
            if (c == '"') break;
            sb.append(c);
            j++;
        }
        return sb.toString();
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
        Uri parsed = Uri.parse(url);
        String scheme = parsed.getScheme();
        if (scheme == null
                || (!"http".equalsIgnoreCase(scheme)
                    && !"https".equalsIgnoreCase(scheme)
                    && !"rtsp".equalsIgnoreCase(scheme)
                    && !"rtmp".equalsIgnoreCase(scheme))) {
            Toast.makeText(this, "仅支持 http/https/rtsp/rtmp URL", Toast.LENGTH_SHORT).show();
            return;
        }
        mockFilePath = url;
        selectedCameraId = null;
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
        if (selectedCameraId == null && mockFilePath != null) {
            input = mockFilePath;
        } else {
            // Camera source - Use Pipe (requires native implementation)
            // For now, we pass "pipe:0" to test command construction, 
            // but show warning that stream won't flow without JNI pipe feed.
            input = "pipe:0";
            if (selectedCameraId != null) {
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

    // ==================== 任务18 / #1：模型管理与人员注册 ====================
    //
    // 设计要点：
    // - 所有模型/仓库 IO（导入、状态、模板读写、图库替换）一律在后台线程执行，UI 线程只做展示，
    //   与 FaceModelManager.status() 的 2s 节流策略一致，绝不在 UI 线程做重 IO。
    // - 注册模板与向量绝不写日志；展示名映射仅存 SharedPreferences（不含任何模板内容）。
    // - 图库替换失败时绝不覆盖运行中图库（refreshGalleryFromRepository 的失败语义）。

    /** 当前设备首选 ABI（manifest 校验与 native 自检使用）。 */
    @NonNull
    private String currentAbi() {
        String[] abis = Build.SUPPORTED_ABIS;
        if (abis != null && abis.length > 0 && abis[0] != null && !abis[0].isEmpty()) {
            return abis[0];
        }
        return Build.CPU_ABI == null ? "" : Build.CPU_ABI;
    }

    /**
     * #3-High（第三波审查）：线程安全的懒加载单例（volatile 字段 + 双检锁）。
     *
     * <p>UI 线程（{@code buildRuntimeCapabilityText} 读 {@code statusSnapshot()}）与后台刷新线程
     * （{@code status()}）会在首帧几乎同时进入本方法；若不加锁，两次「判空-赋值」会竞态各建一个
     * {@link FaceModelManager}。UI 侧持有的实例若从未被调用 {@code status()}，其 statusCache 恒为
     * null，{@code statusSnapshot()} 便永久返回占位，模型状态永不刷新（#3 回归）。本地双检锁保证
     * 全进程仅创建一个实例、两条路径共用同一 statusCache。
     */
    @NonNull
    private FaceModelManager modelManager() {
        FaceModelManager mgr = faceModelManager;
        if (mgr == null) {
            synchronized (this) {
                mgr = faceModelManager;
                if (mgr == null) {
                    mgr = FaceModelManager.forDefault(getApplicationContext());
                    faceModelManager = mgr;
                }
            }
        }
        return mgr;
    }

    /** 与 {@link #modelManager()} 同一策略：volatile + 双检锁，避免并发首次调用各建实例。 */
    @NonNull
    private AndroidEnrollmentRepository enrollmentRepository() {
        AndroidEnrollmentRepository repo = enrollmentRepository;
        if (repo == null) {
            synchronized (this) {
                repo = enrollmentRepository;
                if (repo == null) {
                    repo = AndroidEnrollmentRepository.forDefaultGallery(getApplicationContext());
                    enrollmentRepository = repo;
                }
            }
        }
        return repo;
    }

    /** 绑定 settings 面板中的模型/人员管理控件（直接 findViewById，不改动 MainScreenBinder 契约）。 */
    private void bindModelManagementUi() {
        btnImportModel = findViewById(R.id.btn_import_model);
        btnSwitchModel = findViewById(R.id.btn_switch_model);
        btnManagePersons = findViewById(R.id.btn_manage_persons);
        tvModelManagement = findViewById(R.id.tv_model_management);
        btnRegisterFace = findViewById(R.id.btn_register_face);
        btnResolveSwap = findViewById(R.id.btn_resolve_swap);

        if (btnImportModel != null) btnImportModel.setOnClickListener(v -> confirmLicenseThenPickModelZip());
        if (btnSwitchModel != null) btnSwitchModel.setOnClickListener(v -> showSwitchModelDialog());
        if (btnManagePersons != null) btnManagePersons.setOnClickListener(v -> showPersonManagementDialog());
        if (btnRegisterFace != null) btnRegisterFace.setOnClickListener(v -> showFaceRegisterDialog());
        if (btnResolveSwap != null) btnResolveSwap.setOnClickListener(v -> {
            final String id = ambiguousSwapProfileId;
            if (id != null) showResolveSwapDialog(id);
        });

        refreshModelManagementStatus();
    }

    /**
     * 后台读取模型状态并刷新状态文本（事件驱动，不在 UI 线程做文件 IO）。
     *
     * <p>#2（第三波审查）：与周期/事件快照刷新 {@link #requestModelStatusSnapshotRefresh()} 共用同一
     * {@link #modelStatusRefreshInFlight} 门。二者恒在导入/切换/启动恢复的完成回调里成对触发，且都会
     * {@code new Thread} 调用持目录锁、含文件 IO/native 的 {@code modelManager().status()}；不合并则同
     * 一次事件里并发堆叠两个 status() 线程（正是本门要消除的堆叠）。合并后任一时刻至多 1 个刷新线程：
     * 拿到门者执行 status()——其副作用亦刷新 volatile 快照，故被跳过的快照刷新不会漏刷；未拿到门者直接
     * 跳过（本次操作结果已由 {@link #showModelStatusToUser} 写入状态区，模型文本随下次事件/周期刷新收敛）。
     */
    private void refreshModelManagementStatus() {
        final TextView target = tvModelManagement;
        if (target == null) return;
        if (!modelStatusRefreshInFlight.compareAndSet(false, true)) {
            return;  // 已有刷新在途：不再并发 status()，避免线程堆叠
        }
        // 在门内取号：序号严格随刷新顺序递增（门保证同一时刻只有一个刷新线程）。
        final long refreshSeq = modelStatusRefreshSeq.incrementAndGet();
        try {
            new Thread(() -> {
                String text;
                String ambiguous = null;
                try {
                    FaceRuntimeStatus s = modelManager().status();
                    ambiguous = s.ambiguousSwapProfileId;
                    text = "模型: " + s.sourceLabel()
                            + (s.isUserImported() ? (" profile=" + s.profileId) : "")
                            + " 已导入=" + s.importedProfileCount
                            + (s.isRuntimeEffective() ? " 已生效" : " 未生效")
                            + ((s.message == null || s.message.isEmpty()) ? "" : ("\n" + s.message));
                } catch (Throwable t) {
                    text = "模型状态不可用";
                } finally {
                    modelStatusRefreshInFlight.set(false);
                }
                // lambda 捕获要求 effectively final：先求值到 text，再转 final 供 UI 线程使用
                final String summary = text;
                final String ambiguousId = ambiguous;
                runOnUiThread(() -> {
                    // #2：Activity 已销毁时丢弃陈旧 UI 回调，避免窗口 token 失效。
                    if (isFinishing() || isDestroyed()) return;
                    // 第五轮审查 P2：门在上方 finally 中先于本投递释放，此后可能有更新的一次刷新
                    // 完成并抢先投递；序号比对丢弃迟到的旧结果，避免过期状态覆盖新状态。
                    if (refreshSeq != modelStatusRefreshSeq.get()) return;
                    if (tvModelManagement == target) {
                        target.setText(summary);
                    }
                    // 未决替换入口随状态一起更新：只有状态确实报告「新旧并存」时才可见，且携带
                    // 它报告的 profileId（点击时才不会去动一个猜出来的目标）。
                    ambiguousSwapProfileId = ambiguousId;
                    if (btnResolveSwap != null) {
                        btnResolveSwap.setVisibility(
                                ambiguousId == null ? View.GONE : View.VISIBLE);
                    }
                });
            }, "FaceModelStatus").start();
        } catch (Throwable t) {
            // 线程创建/启动失败（极端 OOM 等）：复位在途标志，避免刷新永久停摆。
            modelStatusRefreshInFlight.set(false);
        }
    }

    /** 向用户展示模型操作结果（失败时 Toast；文本写入模型状态区）。 */
    private void showModelStatusToUser(@NonNull String message, boolean error) {
        if (tvModelManagement != null) {
            // 第五轮审查（A2' Low）：操作结果文本也必须**纳入号序**。否则一个更早启动、序号更低的
            // 在途刷新回调会因「序号未变」而被放行，把刚写入的操作结果覆盖回旧文本。取号即令其作废。
            modelStatusRefreshSeq.incrementAndGet();
            tvModelManagement.setText(message);
        }
        if (error) {
            Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        }
    }

    /**
     * 未决目录替换的人工了结对话框（2026-09-17 外部审查修复）。
     *
     * <p>约束 1：新旧两份模型目录并存且无提交证据时，**保留现场并报错、不猜测、不删除任一副本**。
     * 决定权交回用户——这也是 {@code resolveAmbiguousSwap} 唯一的调用入口。
     */
    private void showResolveSwapDialog(@NonNull String profileId) {
        new AlertDialog.Builder(this)
                .setTitle(R.string.ui_model_resolve_swap_title)
                .setMessage(getString(R.string.ui_model_resolve_swap_message)
                        + "\n\nprofile=" + profileId)
                .setPositiveButton(R.string.ui_model_resolve_swap_keep_new,
                        (d, w) -> resolveSwap(profileId, true))
                .setNegativeButton(R.string.ui_model_resolve_swap_keep_old,
                        (d, w) -> resolveSwap(profileId, false))
                .setNeutralButton(android.R.string.cancel, null)
                .show();
    }

    /** 后台线程执行了结（含递归删除目录 + fsync），不阻塞 UI；结果如实回显并刷新状态。 */
    private void resolveSwap(@NonNull String profileId, boolean keepNew) {
        new Thread(() -> {
            FaceModelManager.Result r;
            try {
                r = modelManager().resolveAmbiguousSwap(profileId, keepNew);
            } catch (Throwable t) {
                r = null;
            }
            final FaceModelManager.Result res = r;
            final boolean ok = res != null && res.isOk();
            final String detail = (res == null || res.message == null) ? "" : res.message;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (ok) {
                    showModelStatusToUser(getString(R.string.ui_model_resolve_swap) + "：" + profileId, false);
                } else {
                    showModelStatusToUser(getString(R.string.ui_model_resolve_swap_failed) + "：" + detail, true);
                }
                refreshModelManagementStatus();
                refreshRuntimeStatusIfDue(true);
            });
        }, "FaceModelResolveSwap").start();
    }

    /**
     * #1（第二波审查 P1）+ 任务18 冷启动恢复：恢复模型激活态与已注册图库。
     *
     * <p>此前生产 Java 零 {@code activate} 调用，导致「导入过的模型重启后不生效（恒 missing_model
     * 仅预览）」；同时重启后内存图库为空，已注册人员无法被识别。此处：
     * <ol>
     *   <li>先从加密仓库重建内存图库并 {@code replaceGallery}（喂入 native gallery 槽）；</li>
     *   <li>先调 {@code recoverForStartup()} 完成目录事务恢复，**再**取回恢复后的
     *       {@code active_profile}——恢复未完成则停止自动激活并如实报错（约束 2-1，勿改回
     *       「先读指针再 activate」：那样恢复会被随后写回的旧值抵消）。</li>
     *   <li>取到目标后调用 {@code activate(target, currentAbi, true)}
     *       绑定实时管线（Engine 未创建时 native 侧登记待激活，创建后自动补激活）。</li>
     * </ol>
     * 全程后台线程执行、失败不阻塞启动；结果只更新状态区文本，reason 如实展示。
     */
    private void restoreActiveProfileOnStartup() {
        final String abi = currentAbi();
        new Thread(() -> {
            // 步骤 1：重建图库（先于 activate，使 native reload 的 loadGallery 钩子能取到该快照）。
            final boolean galleryOk = refreshGalleryFromRepository();

            // 约束 2-1（外部审查 P1 #2）：**先完成目录事务恢复，再重新读取激活指针**。
            // 原实现先 activeProfileId() 拿目标、再 activate()，而恢复发生在 activate() 内部——
            // 若恢复把指针改回旧配置，手里那个「恢复前的值」会被重新激活并写回，抵消掉恢复。
            // recoverForStartup() 内部即「先恢复、后重读」，并在恢复未完成时返回 blockedReason。
            final FaceModelManager.StartupRecovery sr;
            try {
                sr = modelManager().recoverForStartup();
            } catch (Throwable t) {
                runOnUiThread(() -> {
                    showModelStatusToUser(buildRestoreNote("模型启动恢复失败：状态读取异常", galleryOk), true);
                });
                return;
            }
            if (sr.blockedReason != null) {
                // 恢复未完成 → **停止**自动激活并如实报错，绝不猜测哪个配置该生效。
                runOnUiThread(() -> {
                    showModelStatusToUser(buildRestoreNote(
                            "模型启动恢复未完成：" + sr.blockedReason, galleryOk), true);
                    refreshModelManagementStatus();
                    refreshRuntimeStatusIfDue(true);
                    updateDiagnosticsText();
                });
                return;
            }
            final String active = sr.activeProfileId;
            if (active == null) {
                // 未导入：保持未导入态，交由「导入模型」入口引导。
                runOnUiThread(() -> {
                    refreshModelManagementStatus();
                    if (!galleryOk) {
                        showModelStatusToUser("已注册图库恢复失败（重启后可能无法识别已注册人员）", true);
                    }
                });
                return;
            }
            FaceModelManager.Result result = null;
            try {
                result = modelManager().activate(active, abi, true);
            } catch (Throwable ignored) {
                // 恢复失败不阻塞启动：下面按失败展示 reason。
            }
            final FaceModelManager.Result r = result;
            runOnUiThread(() -> {
                if (r == null) {
                    showModelStatusToUser(buildRestoreNote("模型启动恢复失败：管理器不可用", galleryOk), true);
                } else if (r.isOk()) {
                    // 成功不打扰用户，仅刷新状态区。
                    refreshModelManagementStatus();
                    if (!galleryOk) {
                        showModelStatusToUser("已注册图库恢复失败（重启后可能无法识别已注册人员）", true);
                    }
                } else {
                    showModelStatusToUser(buildRestoreNote(
                            "模型启动恢复未生效（" + r.status + "）："
                                    + (r.message == null ? "" : r.message), galleryOk), true);
                }
                refreshRuntimeStatusIfDue(true);
                updateDiagnosticsText();
            });
        }, "FaceModelRestore").start();
    }

    /** 启动恢复提示：合并模型恢复失败原因与图库恢复失败原因（均为非敏感状态文本）。 */
    @NonNull
    private static String buildRestoreNote(@NonNull String modelNote, boolean galleryOk) {
        if (galleryOk) return modelNote;
        return modelNote + "\n已注册图库恢复失败（重启后可能无法识别已注册人员）";
    }

    /** 模型许可确认 -> SAF 选择 ZIP 包（P1-6：APK 不内置权重，用户显式导入）。 */
    private void confirmLicenseThenPickModelZip() {
        new AlertDialog.Builder(this)
                .setTitle("模型许可确认")
                .setMessage("导入前请确认：你已阅读并同意所选模型包的许可条款。"
                        + "模型权重不随 APK 分发，导入后仅保存在本机。")
                .setPositiveButton("我已知晓并继续", (d, w) -> pickModelZip())
                .setNegativeButton("取消", null)
                .show();
    }

    /** SAF 选择模型 ZIP：ACTION_OPEN_DOCUMENT（不申请存储权限）。 */
    private void pickModelZip() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        // 部分 SAF provider 不按 zip MIME 上报，故放开类型并靠内容（manifest.json）校验。
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/octet-stream"});
        try {
            startActivityForResult(intent, REQUEST_CODE_IMPORT_MODEL);
        } catch (Throwable t) {
            Toast.makeText(this, "无法打开文件选择器", Toast.LENGTH_SHORT).show();
        }
    }

    /** 后台导入 ZIP（FaceModelManager.importZip 内部完成安全校验、原子启用与实时管线激活）。 */
    private void importModelZip(@NonNull final Uri uri) {
        final ProgressDialog dialog = new ProgressDialog(this);
        dialog.setMessage("正在导入模型…");
        dialog.setCancelable(false);
        try {
            dialog.show();
        } catch (Throwable ignored) {
        }
        final String abi = currentAbi();
        new Thread(() -> {
            FaceModelManager.Result result = null;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) {
                    throw new IOException("无法打开所选文件");
                }
                result = modelManager().importZip(in, abi, true);
            } catch (Throwable ignored) {
                // 统一按失败展示，不泄露底层路径/内容。
            }
            final FaceModelManager.Result r = result;
            runOnUiThread(() -> {
                try { dialog.dismiss(); } catch (Throwable ignored) {}
                // #2：Activity 已销毁时不再更新状态区/弹 Toast（进度框仍需先关闭）。
                if (isFinishing() || isDestroyed()) return;
                if (r == null) {
                    showModelStatusToUser("模型导入失败：文件读取异常", true);
                } else if (r.isOk()) {
                    showModelStatusToUser("模型导入并已激活", false);
                    Toast.makeText(this, "模型导入成功", Toast.LENGTH_LONG).show();
                } else {
                    showModelStatusToUser("模型导入失败（" + r.status + "）："
                            + (r.message == null ? "" : r.message), true);
                }
                refreshModelManagementStatus();
                refreshRuntimeStatusIfDue(true);
                updateDiagnosticsText();
            });
        }, "FaceModelImport").start();
    }

    /** 列出已导入 profile，供用户切换（切换即重新校验并激活实时管线）。 */
    private void showSwitchModelDialog() {
        new Thread(() -> {
            final List<String> profiles;
            final String active;
            try {
                profiles = modelManager().listProfiles();
                active = modelManager().activeProfileId();
            } catch (Throwable t) {
                runOnUiThread(() -> Toast.makeText(this, "读取模型列表失败", Toast.LENGTH_SHORT).show());
                return;
            }
            runOnUiThread(() -> {
                // #2：Activity 已销毁时不再弹窗，避免窗口 token 失效（BadTokenException）。
                if (isFinishing() || isDestroyed()) return;
                if (profiles.isEmpty()) {
                    Toast.makeText(this, "暂无已导入模型，请先导入 ZIP", Toast.LENGTH_LONG).show();
                    return;
                }
                String[] items = new String[profiles.size()];
                for (int i = 0; i < profiles.size(); i++) {
                    String id = profiles.get(i);
                    items[i] = id + (id.equals(active) ? "（当前）" : "");
                }
                new AlertDialog.Builder(this)
                        .setTitle("切换模型")
                        .setItems(items, (d, which) -> activateProfile(profiles.get(which)))
                        .setNegativeButton("取消", null)
                        .show();
            });
        }, "FaceModelList").start();
    }

    /** 后台切换 profile；失败保留原配置并在状态区显示 reason。 */
    private void activateProfile(@NonNull final String profileId) {
        final String abi = currentAbi();
        new Thread(() -> {
            FaceModelManager.Result result = null;
            try {
                result = modelManager().activate(profileId, abi, true);
            } catch (Throwable ignored) {
            }
            final FaceModelManager.Result r = result;
            runOnUiThread(() -> {
                if (r == null) {
                    showModelStatusToUser("切换模型失败：管理器不可用", true);
                } else if (r.isOk()) {
                    showModelStatusToUser("已切换到模型：" + profileId, false);
                } else {
                    showModelStatusToUser("切换模型未生效（" + r.status + "）："
                            + (r.message == null ? "" : r.message), true);
                }
                refreshModelManagementStatus();
                refreshRuntimeStatusIfDue(true);
                updateDiagnosticsText();
            });
        }, "FaceModelSwitch").start();
    }

    /**
     * 注册入口：弹出注册对话框（personId/displayName 输入 + 采样进度/拒绝原因 + 取消）。
     * 不允许在既有采集进行中重复打开。
     */
    private void showFaceRegisterDialog() {
        if (isFinishing() || isDestroyed()) return;
        if (enrollmentInProgress) {
            Toast.makeText(this, "注册采集进行中", Toast.LENGTH_SHORT).show();
            return;
        }
        View content = LayoutInflater.from(this).inflate(R.layout.dialog_face_register, null, false);
        regPersonIdInput = content.findViewById(R.id.et_register_person_id);
        regDisplayNameInput = content.findViewById(R.id.et_register_display_name);
        regStatusText = content.findViewById(R.id.tv_register_status);
        regStartButton = content.findViewById(R.id.btn_register_start);
        regCancelButton = content.findViewById(R.id.btn_register_cancel);
        if (regStatusText != null) {
            regStatusText.setText(R.string.ui_register_status_idle);
        }
        if (regStartButton != null) regStartButton.setOnClickListener(v -> startEnrollment());
        if (regCancelButton != null) regCancelButton.setOnClickListener(v -> {
            // 取消会作废尚未完成的准备回调（代次门）。
            enrollmentGeneration++;
            if (enrollmentInProgress) {
                cancelEnrollmentInternal();
            } else {
                updateEnrollmentStatusText("已取消准备");
            }
        });
        enrollmentDialog = new AlertDialog.Builder(this)
                .setTitle("人脸注册（8 样本）")
                .setView(content)
                .setNegativeButton("关闭", null)
                .setOnDismissListener(d -> {
                    // 关闭对话框即作废未完成的准备/轮询；仅当仍在采集时才清理会话。
                    enrollmentGeneration++;
                    if (enrollmentInProgress) cancelEnrollmentInternal();
                })
                .create();
        enrollmentDialog.show();
    }

    /** 校验输入 -> 后台准备 8 样本会话 -> 250ms 轮询进度。 */
    private void startEnrollment() {
        if (regPersonIdInput == null || regStatusText == null || enrollmentInProgress) return;
        final String personId = regPersonIdInput.getText() == null ? "" : regPersonIdInput.getText().toString().trim();
        final String displayName = regDisplayNameInput == null || regDisplayNameInput.getText() == null
                ? "" : regDisplayNameInput.getText().toString().trim();

        // 纯字符串校验（无 IO），在 UI 线程完成。
        if (personId.isEmpty()) {
            regStatusText.setText("校验失败：人员标识不能为空");
            return;
        }
        if (!isValidPersonId(personId)) {
            regStatusText.setText("校验失败：人员标识仅允许字母/数字/._-（含中文等 Unicode），长度 1..64");
            return;
        }

        // 先禁用交互；注册表读取与现有模板快照解密（可能在 Keystore 上）放到后台线程，避免 UI 卡顿。
        regStatusText.setText("校验并准备采集会话…");
        regPersonIdInput.setEnabled(false);
        if (regDisplayNameInput != null) regDisplayNameInput.setEnabled(false);
        if (regStartButton != null) regStartButton.setEnabled(false);

        final AndroidEnrollmentRepository repo = enrollmentRepository();
        final int gen = ++enrollmentGeneration;
        new Thread(() -> {
            String error = null;
            EnrollmentSession prepared = null;
            try {
                if (!repo.isEnrollmentEnabled()) {
                    error = repo.unsupportedReason();
                } else if (repo.get(personId).isOk()) {
                    error = "该人员已注册，请先删除或更换标识";
                } else {
                    prepared = new EnrollmentSession(personId, EnrollmentSession.fromRepository(repo));
                    if (!prepared.start()) {
                        error = "无法开始采集会话";
                    }
                }
            } catch (Throwable t) {
                error = "采集会话准备失败";
            }
            final String err = error;
            final EnrollmentSession session = prepared;
            runOnUiThread(() -> {
                // 代次门：准备期间对话框被关闭/取消则丢弃结果，不复活已关闭的会话。
                if (gen != enrollmentGeneration) {
                    if (session != null) session.cancel();
                    return;
                }
                if (err != null) {
                    regStatusText.setText("校验失败：" + err);
                    if (regPersonIdInput != null) regPersonIdInput.setEnabled(true);
                    if (regDisplayNameInput != null) regDisplayNameInput.setEnabled(true);
                    if (regStartButton != null) regStartButton.setEnabled(true);
                    return;
                }
                // 任务 B：显示名**仅存内存**，注册成功后才加密落盘。
                // 此前是此处立即写 SharedPreferences（明文），而它早于 startNativeEnrollmentCapture，
                // 导致取消/失败都留下孤儿键；改为延迟落盘后该问题从根上消失。
                pendingEnrollmentDisplayName = displayName;
                lastEnrollmentRejectReason = "";
                enrollmentSession = session;
                enrollmentInProgress = true;
                // #1 打通注册闭环：接通 native 逐帧特征源（开启失败不阻断会话，仅提示等待）。
                startNativeEnrollmentCapture();
                regStatusText.setText("采集中 0/8\n（请正对摄像头，按提示缓慢转头采集正/左/右脸）");
                startEnrollmentPoller();
            });
        }, "FaceEnrollPrepare").start();
    }

    /** personId 会作为展示/映射键并进入模板文件名哈希，限制为安全字符集与长度。 */
    private static boolean isValidPersonId(@NonNull String personId) {
        if (personId.isEmpty() || personId.length() > 64) return false;
        for (int i = 0; i < personId.length(); i++) {
            char c = personId.charAt(i);
            // #7a：放宽为任意 Unicode 字母/数字（含中文、全角、带重音拉丁、假名、谚文等），
            // 与提示文案「中英文/数字」对齐。模板文件名是 userId 的 SHA-256、注册表用 hex 编码，
            // 更广的字符集不会进入原始路径，故无路径穿越风险。
            boolean ok = Character.isLetterOrDigit(c) || c == '.' || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    /** 250ms 轮询会话状态；仅在 COMPLETED 时执行「单取模板 -> 加密写入 -> 原子替换图库」。 */
    private void startEnrollmentPoller() {
        stopEnrollmentPoller();
        final Runnable poller = new Runnable() {
            @Override
            public void run() {
                EnrollmentSession session = enrollmentSession;
                if (session == null || !enrollmentInProgress) return;
                EnrollmentSession.State st = session.state();
                if (st == EnrollmentSession.State.COMPLETED) {
                    EnrollmentSession.SessionStatus status = session.status();
                    updateEnrollmentStatusText("已完成 " + status.acceptedCount + "/" + status.target);
                    completeEnrollment();
                    return;
                }
                if (st == EnrollmentSession.State.CANCELLED || st == EnrollmentSession.State.IDLE) {
                    enrollmentInProgress = false;
                    updateEnrollmentStatusText("已取消");
                    return;
                }
                updateEnrollmentStatusText(buildEnrollmentProgressText(session.status()));
                handler.postDelayed(this, ENROLLMENT_POLL_INTERVAL_MS);
            }
        };
        enrollmentPoller = poller;
        handler.post(poller);
    }

    @NonNull
    private String buildEnrollmentProgressText(@NonNull EnrollmentSession.SessionStatus st) {
        String text = "采集中 " + st.acceptedCount + "/" + st.target
                + "（正脸 " + st.frontCount + "/" + EnrollmentSession.QUOTA_FRONT
                + "，左 " + st.leftCount + "/" + EnrollmentSession.QUOTA_LEFT
                + "，右 " + st.rightCount + "/" + EnrollmentSession.QUOTA_RIGHT + "）";
        if (!lastEnrollmentRejectReason.isEmpty()) {
            text += "\n最近拒绝：" + lastEnrollmentRejectReason;
        }
        return text;
    }

    /**
     * #1 开启 native 注册采集：分配/复用 direct ByteBuffer，注册帧监听器并调用
     * {@link NativeFaceBridge#startEnrollmentCapture(ByteBuffer)}。
     * 开启失败不阻断会话（进度区会持续显示 0/8，由用户可见）。
     */
    private void startNativeEnrollmentCapture() {
        try {
            if (enrollmentCaptureBuffer == null
                    || enrollmentCaptureBuffer.capacity() != NativeFaceBridge.EMBEDDING_BYTES) {
                enrollmentCaptureBuffer = ByteBuffer.allocateDirect(NativeFaceBridge.EMBEDDING_BYTES);
                enrollmentCaptureBuffer.order(ByteOrder.LITTLE_ENDIAN);
            } else {
                enrollmentCaptureBuffer.clear();
            }
            NativeFaceBridge.setEnrollmentFrameListener(this::onEnrollmentFrameFromNative);
            String resp = NativeFaceBridge.startEnrollmentCapture(enrollmentCaptureBuffer);
            if (!jsonGetBool(resp, "ok", false)) {
                // 采集源未接通：清除监听器，绝不用占位/合成向量冒充真实样本。
                NativeFaceBridge.setEnrollmentFrameListener(null);
                AppLog.w("MainActivity", "startNativeEnrollmentCapture", "native 采集未开启");
            }
        } catch (Throwable t) {
            NativeFaceBridge.setEnrollmentFrameListener(null);
            AppLog.w("MainActivity", "startNativeEnrollmentCapture", "native 采集开启异常");
        }
    }

    /** #1 停止 native 注册采集并清除监听器/缓冲；幂等（取消/完成/销毁共用）。 */
    private void stopNativeEnrollmentCapture() {
        try {
            NativeFaceBridge.setEnrollmentFrameListener(null);
            NativeFaceBridge.stopEnrollmentCapture();
        } catch (Throwable t) {
            AppLog.w("MainActivity", "stopNativeEnrollmentCapture", "native 采集停止异常");
        } finally {
            // #5 模板红线：丢弃 direct ByteBuffer 前写零，避免原生内存残留最后一帧模板至 GC。
            zeroDirectBuffer(enrollmentCaptureBuffer);
            enrollmentCaptureBuffer = null;
        }
    }

    /**
     * #5 把 direct ByteBuffer 的底层原生内存整体写零后再交还/丢弃。
     *
     * <p>仅置 {@code null} 不会清零 direct buffer 的原生内存，最后写入的 512 维模板会一直
     * 驻留到 GC 回收该 DirectByteBuffer 为止；故释放引用前必须先清零。幂等，失败不影响流程。
     */
    private static void zeroDirectBuffer(@Nullable ByteBuffer buffer) {
        if (buffer == null || !buffer.isDirect()) return;
        try {
            final int floats = buffer.capacity() / Float.BYTES;
            if (floats <= 0) return;
            buffer.rewind();
            FloatBuffer fb = buffer.asFloatBuffer();
            fb.put(new float[floats]);
            fb.rewind();
            buffer.rewind();
        } catch (Throwable t) {
            // 清零失败不阻断停止流程：引用随后即被置空丢弃。
        }
    }

    /**
     * #1 native 帧回调（运行在 native 引擎线程）：把打包指标组为
     * {@link EnrollmentSession.FrameSample} 并喂入会话。
     *
     * <p>约束：绝不触碰 UI；embedding 仅内存传递，局部副本用毕清零（模板红线）。
     */
    void onEnrollmentFrameFromNative(@Nullable ByteBuffer embedding, @Nullable float[] metrics) {
        EnrollmentSession session = enrollmentSession;
        if (session == null || !enrollmentInProgress) return;
        if (embedding == null || metrics == null
                || metrics.length < NativeFaceBridge.ENROLL_METRICS_LEN) {
            return;
        }
        float[] emb = new float[NativeFaceBridge.EMBEDDING_DIM];
        try {
            FloatBuffer fb = embedding.asFloatBuffer();
            fb.rewind();
            fb.get(emb);
        } catch (Throwable t) {
            Arrays.fill(emb, 0f);
            return;
        }
        // 无关键点（native 以 NaN 占位）时 landmarks=null，由姿态判定拒收该帧。
        float[] landmarks = new float[10];
        boolean landmarksOk = true;
        for (int i = 0; i < 10; i++) {
            float v = metrics[12 + i];
            if (Float.isNaN(v) || Float.isInfinite(v)) {
                landmarksOk = false;
                break;
            }
            landmarks[i] = v;
        }
        EnrollmentSession.FrameSample sample = EnrollmentSession.FrameSample.builder(emb)
                .box(metrics[8], metrics[9], metrics[10], metrics[11])
                .frame((int) metrics[0], (int) metrics[1])
                .quality(metrics[4], metrics[5], metrics[6])
                .detection(metrics[2], (int) metrics[3])
                .landmarks(landmarksOk ? landmarks : null)
                .build();
        // builder 已 clone embedding，本地副本立即清零。
        Arrays.fill(emb, 0f);
        feedEnrollmentSample(sample);
    }

    /**
     * 集成接缝：向进行中的注册会话喂入一帧样本并记录拒绝原因。
     *
     * <p>逐帧特征源为 #1 新增的 native 注册采集回调（{@link NativeFaceBridge#startEnrollmentCapture}）：
     * 由 {@link #onEnrollmentFrameFromNative(ByteBuffer, float[])} 组 FrameSample 后调用本方法。
     * 绝不使用占位/合成向量冒充真实样本。
     */
    void feedEnrollmentSample(@Nullable EnrollmentSession.FrameSample sample) {
        EnrollmentSession session = enrollmentSession;
        if (session == null || !enrollmentInProgress) return;
        EnrollmentSession.FeedResult r = session.feed(sample);
        if (!r.accepted) {
            lastEnrollmentRejectReason = (r.message == null || r.message.isEmpty())
                    ? r.reason.name() : r.message;
        } else {
            lastEnrollmentRejectReason = "";
        }
    }

    /** 完成提交：单取均值模板 -> 加密写入仓库 -> 用仓库重建并原子替换运行中图库。 */
    private void completeEnrollment() {
        final EnrollmentSession session = enrollmentSession;
        if (session == null) return;
        enrollmentInProgress = false;
        stopEnrollmentPoller();
        stopNativeEnrollmentCapture();  // #1 先断开 native 特征源，再取模板
        final String personId = session.userId();
        // 单取模板（一次性；LE float32 2048 字节）。返回的是副本，会话随后可安全取消。
        final byte[] template = session.meanTemplateLe();
        if (regStartButton != null) regStartButton.setEnabled(false);
        updateEnrollmentStatusText("模板生成完成，正在加密写入…");
        new Thread(() -> {
            String error = null;
            try {
                if (template == null || template.length != NativeFaceBridge.EMBEDDING_BYTES) {
                    error = "模板生成失败（维度不符）";
                } else {
                    AndroidEnrollmentRepository.Result add = enrollmentRepository().add(
                            personId, template, ENROLL_MODEL_VERSION, ENROLL_TEMPLATE_SCHEMA_VERSION);
                    if (!add.isOk()) {
                        error = "加密写入失败（" + add.status + "）：" + add.message;
                    } else {
                        // 任务 B：显示名与**模板的持久化**对齐，而非与「内存图库替换」对齐——add 成功
                        // 即注册已加密落盘，此时就该写显示名。否则图库替换失败会让显示名**永久丢失**
                        // （模板已在盘中，人已注册，但名字永远显示不出来）。
                        // 失败不阻断注册（可降级为 personId），仅记日志。
                        String pendingName = pendingEnrollmentDisplayName;
                        if (pendingName != null && !pendingName.trim().isEmpty()
                                && !displayNameStore().save(personId, pendingName)) {
                            AppLog.w("MainActivity", "completeEnrollment",
                                    "显示名加密写入失败（注册仍成功，显示名降级为 personId）");
                        }
                        if (!refreshGalleryFromRepository()) {
                            // 失败不更新运行中 gallery：数据已加密落盘，但内存图库保持旧状态。
                            error = "已加密写入本地，但运行中图库替换失败（未更新）";
                        }
                    }
                }
            } catch (Throwable t) {
                error = "注册提交异常";
            } finally {
                if (template != null) {
                    Arrays.fill(template, (byte) 0);
                }
            }
            final String err = error;
            runOnUiThread(() -> {
                if (err == null) {
                    updateEnrollmentStatusText("注册成功：" + personId + "（8 样本均值模板已加密写入并生效）");
                    Toast.makeText(this, "注册成功", Toast.LENGTH_LONG).show();
                    if (enrollmentDialog != null) {
                        try { enrollmentDialog.dismiss(); } catch (Throwable ignored) {}
                    }
                } else {
                    updateEnrollmentStatusText("注册失败：" + err);
                    if (regStartButton != null) regStartButton.setEnabled(true);
                    if (regPersonIdInput != null) regPersonIdInput.setEnabled(true);
                    if (regDisplayNameInput != null) regDisplayNameInput.setEnabled(true);
                }
                // 任务 B：会话无论成败都已结束，清掉采集期的内存显示名（成功时已在后台写穿存储）。
                pendingEnrollmentDisplayName = null;
                refreshModelManagementStatus();
                refreshRuntimeStatusIfDue(true);
                updateDiagnosticsText();
            });
        }, "FaceEnrollCommit").start();
    }

    /** 取消采集：停止轮询、清零会话向量、恢复输入控件。 */
    private void cancelEnrollmentInternal() {
        stopEnrollmentPoller();
        stopNativeEnrollmentCapture();  // #1 断开 native 特征源
        enrollmentInProgress = false;
        // 任务 B：取消即丢弃采集期的内存显示名——从未落盘，故无需清理存储（这是延迟落盘的直接收益）。
        pendingEnrollmentDisplayName = null;
        EnrollmentSession session = enrollmentSession;
        if (session != null) {
            session.cancel();
        }
        if (regStartButton != null) regStartButton.setEnabled(true);
        if (regPersonIdInput != null) regPersonIdInput.setEnabled(true);
        if (regDisplayNameInput != null) regDisplayNameInput.setEnabled(true);
        updateEnrollmentStatusText("已取消");
    }

    private void stopEnrollmentPoller() {
        if (enrollmentPoller != null) {
            handler.removeCallbacks(enrollmentPoller);
            enrollmentPoller = null;
        }
    }

    private void updateEnrollmentStatusText(@NonNull String text) {
        if (regStatusText != null) {
            regStatusText.setText(text);
        }
    }

    /**
     * 用加密仓库重建内存图库并原子替换（{@link NativeFaceBridge#replaceGallery}）。
     *
     * <p>失败语义（#5 逐条隔离）：单条密文损坏/单键失效只跳过该条（计入 skipped），
     * 只要还有 ≥1 条成功就用成功子集 replaceGallery；仅当仓库本有人员条目却全部读取失败
     * 时返回 false 且<b>不调用</b> replaceGallery。既能避免单条损坏让全部健康人员不可识别，
     * 也绝不用空图库覆盖运行中图库。仓库本就没有人员时按空图库替换（保持既有语义）。
     */
    private boolean refreshGalleryFromRepository() {
        try {
            AndroidEnrollmentRepository.ListResult list = enrollmentRepository().list();
            if (!list.isOk()) return false;
            List<String> ids = new ArrayList<>();
            List<ByteBuffer> embeddings = new ArrayList<>();
            // 注册表的**完整**人员集合：与 ids 不同——ids 只含成功解码模板的条目，而本集合含全部
            // 已注册者。显示名的预取与孤儿清理必须用它：若误用 ids，模板损坏（skipped）的人会被
            // 当成「已注销」而删掉其显示名——那是数据丢失。
            List<String> allRegisteredIds = new ArrayList<>();
            int skipped = 0;
            for (final AndroidEnrollmentRepository.Entry e : list.entries) {
                allRegisteredIds.add(e.userId);
                final float[][] decoded = new float[1][];
                AndroidEnrollmentRepository.Result rr;
                try {
                    rr = enrollmentRepository().readTemplate(e.userId, pt -> {
                        float[] emb = EnrollmentSession.decodeLe(pt);
                        if (emb != null) {
                            decoded[0] = emb;
                        }
                    });
                } catch (Throwable t) {
                    rr = null;  // 单条读取抛异常视同该条失败，逐条隔离、不拖垮整体。
                }
                if (rr == null || !rr.isOk() || decoded[0] == null
                        || decoded[0].length != NativeFaceBridge.EMBEDDING_DIM) {
                    skipped++;
                    continue;
                }
                ByteBuffer bb = ByteBuffer.allocateDirect(NativeFaceBridge.EMBEDDING_BYTES);
                bb.order(ByteOrder.LITTLE_ENDIAN);
                bb.asFloatBuffer().put(decoded[0]);
                bb.rewind();
                ids.add(e.userId);
                embeddings.add(bb);
            }
            // 「全失败」判定：仓库本有人员条目，却没有任何一条成功 —— 不替换运行中图库。
            if (ids.isEmpty() && skipped > 0) {
                AppLog.w("MainActivity", "refreshGalleryFromRepository",
                        "图库重建全部失败：skipped=" + skipped + "，保持运行中图库不变");
                return false;
            }
            String resp = NativeFaceBridge.replaceGallery(
                    ids.toArray(new String[0]), embeddings.toArray(new ByteBuffer[0]));
            boolean ok = jsonGetBool(resp, "ok", false);
            if (skipped > 0) {
                AppLog.w("MainActivity", "refreshGalleryFromRepository",
                        "图库重建跳过损坏条目 skipped=" + skipped + "（成功 " + ids.size() + " 条）");
            }
            // 任务 B：显示名的预取与孤儿清理**共用同一权威性前提**（A4 对称化）。
            // 传入的是**注册表完整集合** allRegisteredIds，而非成功解码子集 ids —— 见其声明处注释。
            // 本方法在后台线程调用，符合 PersonDisplayNameStore 的线程约定。
            PersonDisplayNameStore store = displayNameStore();
            // 仅在注册表条目集**权威**（无损坏行、未从 .bak 恢复、且确实存在注册表文件）时才预取 / 清孤儿：
            //   * removeOrphans 是**破坏性**的 —— 非权威时差集里会混入「仍在册但本次没读到」的人，其
            //     显示名会被误删（A2 审计 H2）；
            //   * prefetch 虽**非破坏性**（只清缓存、不删文件），但其读取会经读路径的「阶段 1 恢复基线」
            //     把遗留的 .bak 改名还原为主文件并解密 ⇒ 可把已删除人员的明文**复活进内存缓存**。
            //     故二者同口径门控（A4：此前 prefetch 是无条件的，与紧随其后的门控不对称）。
            // 非权威时的降级：显示名退化为 personId（不再预取）—— 可接受：权威性通常在下一次 list()
            // 即恢复（从 .bak 恢复的路径会顺带把 .bak 改名回主文件，见 AndroidEnrollmentRepository.loadRegistry）。
            if (list.authoritative) {
                store.prefetch(allRegisteredIds);
                store.removeOrphans(new java.util.HashSet<>(allRegisteredIds));
            } else {
                AppLog.w("MainActivity", "refreshGalleryFromRepository",
                        "注册表条目集非权威（含损坏行 / 从备份恢复 / 无注册表），跳过显示名预取与孤儿清理"
                                + "（避免复活遗留备份 / 误删显示名）");
            }
            migrateLegacyDisplayNames();
            return ok;
        } catch (Throwable t) {
            return false;
        }
    }

    /** 本地人员列表；点击人员进入删除二次确认。 */
    private void showPersonManagementDialog() {
        new Thread(() -> {
            final AndroidEnrollmentRepository.ListResult list;
            try {
                list = enrollmentRepository().list();
            } catch (Throwable t) {
                runOnUiThread(() -> Toast.makeText(this, "人员列表读取失败", Toast.LENGTH_SHORT).show());
                return;
            }
            runOnUiThread(() -> {
                // #2：Activity 已销毁时不再弹窗，避免窗口 token 失效（BadTokenException）。
                if (isFinishing() || isDestroyed()) return;
                if (!list.isOk()) {
                    Toast.makeText(this, "人员列表读取失败：" + list.message, Toast.LENGTH_LONG).show();
                    return;
                }
                final List<AndroidEnrollmentRepository.Entry> entries = list.entries;
                // 任务 B 决策：历史明文显示名默认保留、不自动删除，在此处（人员管理入口）提示并提供清除。
                promptClearLegacyDisplayNamesIfAny();
                if (entries.isEmpty()) {
                    Toast.makeText(this, "暂无已注册人员", Toast.LENGTH_SHORT).show();
                    return;
                }
                String[] items = new String[entries.size()];
                for (int i = 0; i < entries.size(); i++) {
                    AndroidEnrollmentRepository.Entry e = entries.get(i);
                    String name = displayNameFor(e.userId);
                    items[i] = (name.equals(e.userId) ? e.userId : (name + "（" + e.userId + "）"))
                            + "  模板 " + e.ciphertextSize + "B";
                }
                new AlertDialog.Builder(this)
                        .setTitle("人员列表（点击删除，需二次确认）")
                        .setItems(items, (d, which) -> confirmDeletePerson(entries.get(which)))
                        .setNegativeButton("关闭", null)
                        .show();
            });
        }, "FacePersonList").start();
    }

    /** 删除二次确认（不可逆操作必须显式确认）。 */
    private void confirmDeletePerson(@NonNull final AndroidEnrollmentRepository.Entry entry) {
        new AlertDialog.Builder(this)
                .setTitle("删除人员")
                .setMessage("确认删除「" + displayNameFor(entry.userId) + "」及其加密模板？\n"
                        + "删除后运行中图库会同步移除该人员，此操作不可恢复。")
                .setPositiveButton("确认删除", (d, w) -> deletePerson(entry.userId))
                .setNegativeButton("取消", null)
                .show();
    }

    /** 后台删除人员；成功后同步从其重建图库（失败不更新运行中图库）。 */
    private void deletePerson(@NonNull final String userId) {
        new Thread(() -> {
            AndroidEnrollmentRepository.Result result = enrollmentRepository().remove(userId);
            boolean galleryOk = false;
            if (result.isOk()) {
                // A4-Low：removeDisplayName 内部只吞 SecurityException（见 PersonDisplayNameStore.remove），
                // 若它抛出**其它**未受检异常，会从本 Runnable 逃逸 ⇒ 线程死掉、其后的 runOnUiThread 不再
                // 执行 ⇒ **成功/失败 Toast 都不弹**（安全无碍，缓存已由 remove 的 finally 清；仅 UX 问题）。
                // 故在此兜住并如实记一条日志，保证后续 Toast 与图库刷新照常执行。
                try {
                    removeDisplayName(userId);
                } catch (Exception t) {
                    AppLog.w("MainActivity", "deletePerson",
                            "显示名清理异常（不影响本地模板删除结果）: " + t);
                }
                galleryOk = refreshGalleryFromRepository();
            }
            final AndroidEnrollmentRepository.Result r = result;
            final boolean gOk = galleryOk;
            runOnUiThread(() -> {
                if (!r.isOk()) {
                    Toast.makeText(this, "删除失败（" + r.status + "）：" + r.message, Toast.LENGTH_LONG).show();
                } else if (!gOk) {
                    Toast.makeText(this, "已删除本地模板，但运行中图库未更新", Toast.LENGTH_LONG).show();
                } else {
                    Toast.makeText(this, "已删除：" + userId, Toast.LENGTH_SHORT).show();
                }
                refreshModelManagementStatus();
            });
        }, "FacePersonDelete").start();
    }

    /**
     * 显示名缓存（惰性 + 双检锁，与 modelManager()/enrollmentRepository() 同法）。
     *
     * <p><b>构造路径不得建目录（FACE-JAVA-002 修复）</b>：本 accessor 也会在「平台不支持」的早退路径上被
     * 取到（{@code migrateLegacyDisplayNames} 的 {@code displayNameStore().isSupported()} 是一次纯能力
     * 查询；{@code refreshGalleryFromRepository} 更是**不受平台支持门禁约束地**取实例）。若在构造路径里 {@code mkdirs()}，
     * API&lt;23 设备会凭空多出一个**永远用不到**的 {@code files/face_names/} 空目录。故此处**只计算路径
     * 对象**（{@code new File} 无文件系统副作用），目录的实际创建推迟到**首次写入**
     * （{@code FeatureTemplateEncryptedStore.writeEnvelopeAtomically} 写到该目录时才 mkdirs）；
     * 读取路径（prefetch / readFromDisk / removeOrphans）在目录缺失时本就按「无文件」正确处理。
     */
    @NonNull
    private PersonDisplayNameStore displayNameStore() {
        PersonDisplayNameStore s = displayNameStore;
        if (s == null) {
            synchronized (this) {
                s = displayNameStore;
                if (s == null) {
                    // 与模板目录分开：孤儿清理只扫本目录，构造上不可能误删模板文件。
                    // 仅构造路径对象；目录由写入路径惰性创建（见方法 javadoc）。
                    s = new PersonDisplayNameStore(new File(getFilesDir(), "face_names"));
                    displayNameStore = s;
                }
            }
        }
        return s;
    }

    /**
     * 展示名查询：**UI 线程安全**（纯读内存缓存，无 IO、无加解密）。
     * 未命中（尚未预取 / 无该人员 / 密钥不可用 / 文件损坏）返回 personId 本身。
     */
    @NonNull
    private String displayNameFor(@Nullable String personId) {
        if (personId == null || personId.isEmpty()) return "";
        return displayNameStore().get(personId);
    }

    private void removeDisplayName(@NonNull String personId) {
        // 返回值 = 显示名文件是否**确认已删**（PersonDisplayNameStore.remove）。本类作为平台层消费它并
        // 落日志：store 侧刻意不依赖 android.util.Log——它在 PersonDisplayNameStoreTest 里是**纯 JVM**
        // 构造的（无 Robolectric、未开 returnDefaultValues），记日志会抛 "not mocked"；故「报告」与
        // 「记录」分居两层，本方法即二者的接缝。
        //
        // 日志**不带 personId**：本类既有日志没有记录人员标识符的先例，不借此新开一个持久化标识面；
        // 需定位时可按时间对齐同一次 deletePerson 流程的其它日志。
        if (!displayNameStore().remove(personId)) {
            AppLog.w("MainActivity", "removeDisplayName",
                    "显示名文件未确认删除（被拒或静默未生效）—— 内存缓存已清；"
                            + "残留为加密件，待后续权威 list() 的孤儿清理回收");
        }
    }

    // ── 历史明文显示名：默认保留 + 用户确认后清除（任务 B 决策，2026-09-16）──────────────
    // 不自动删除的理由：保留是**避免未经同意丢失数据**，不是低估 PII 风险。只要历史键存在，
    // 明文就在——**不得**宣称「设备上已无明文 PII」。

    /** 检测到历史明文显示名时提示（在人员管理入口调用）。用户可保留，也可进入清除流程。 */
    private void promptClearLegacyDisplayNamesIfAny() {
        Map<String, ?> all = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getAll();
        if (!LegacyDisplayNames.hasRetainedNames(all)) return;
        int n = LegacyDisplayNames.retainedCount(all);
        new AlertDialog.Builder(this)
                .setTitle("历史显示名")
                .setMessage("本设备仍保留 " + n + " 条未加密的历史显示名，当前不用于展示，"
                        + "也不会被新增或使用。可手动清除。")
                .setPositiveButton("清除", (d, w) -> confirmClearLegacyDisplayNames())
                .setNegativeButton("保留", null)
                .show();
    }

    /** 清除前的二次确认（不可逆操作必须显式确认，且说明影响范围）。 */
    private void confirmClearLegacyDisplayNames() {
        new AlertDialog.Builder(this)
                .setTitle("确认清除历史显示名")
                .setMessage("显示名记录不可恢复。\n人员 ID 与加密模板不受影响，识别功能不受影响。\n确认清除？")
                .setPositiveButton("确认清除", (d, w) -> clearLegacyDisplayNames())
                .setNegativeButton("取消", null)
                .show();
    }

    /**
     * 只删除显示名映射键，**绝不**清空整个 SharedPreferences（其余 10 余个设置键必须原样保留）。
     *
     * <p>用 {@code commit()} 而非 {@code apply()}：只有前者是同步落盘且**返回是否成功**，符合
     * 「持久化删除成功后才提示完成」的要求。
     */
    private void clearLegacyDisplayNames() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        Set<String> keys = LegacyDisplayNames.selectKeysForClearing(prefs.getAll());
        if (keys.isEmpty()) {
            Toast.makeText(this, "没有需要清除的历史显示名", Toast.LENGTH_SHORT).show();
            return;
        }
        SharedPreferences.Editor ed = prefs.edit();
        for (String k : keys) {
            ed.remove(k);
        }
        boolean ok = ed.commit();
        Toast.makeText(this,
                ok ? ("已清除 " + keys.size() + " 条历史显示名")
                   : "清除失败（未能写入），请重试",
                ok ? Toast.LENGTH_SHORT : Toast.LENGTH_LONG).show();
        AppLog.w("MainActivity", "clearLegacyDisplayNames",
                ok ? ("已清除历史明文显示名 count=" + keys.size())
                   : "历史显示名清除失败（未落盘）");
    }

    /**
     * 一次性迁移（任务 B）：把历史明文 {@code pref_person_name_*} 加密迁入显示名存储。
     *
     * <p>顺序严格为「加密 → **验证密文可读** → 删原值」；任一步失败都**保留原明文**并留待下次重试
     * （不置 {@link #PREF_NAME_MIGRATED}）。可重入：中断后重跑对已迁移项是幂等的（写同值再删键）。
     *
     * <p><b>必须在后台线程调用</b>（含文件 IO 与加解密）。
     */
    private void migrateLegacyDisplayNames() {
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        if (prefs.getBoolean(PREF_NAME_MIGRATED, false)) return;
        if (!displayNameStore().isSupported()) {
            // 本机没有加密能力（API<23 或密钥不可用）：迁移**永远不可能成功**，不做徒劳重试，
            // 也不置完成标志。旧明文键的去留见 docs/designs/android-enrollment-design.md。
            AppLog.w("MainActivity", "migrateLegacyDisplayNames",
                    "本机不支持 Keystore AES，跳过显示名迁移（旧明文键保持原状）");
            return;
        }

        boolean allOk = true;
        for (Map.Entry<String, ?> e : prefs.getAll().entrySet()) {
            String key = e.getKey();
            if (key == null || !key.startsWith(PREF_PERSON_NAME_PREFIX)) continue;
            if (!(e.getValue() instanceof String)) continue;

            String personId = key.substring(PREF_PERSON_NAME_PREFIX.length());
            String legacyName = ((String) e.getValue()).trim();
            if (personId.isEmpty()) continue;
            if (legacyName.isEmpty()) {
                // 空值是旧实现写入的残留（displayName 为空也会 putString ""），直接清掉。
                prefs.edit().remove(key).apply();
                continue;
            }

            PersonDisplayNameStore store = displayNameStore();
            if (!store.save(personId, legacyName) || !legacyName.equals(store.readFromDisk(personId))) {
                allOk = false;   // 迁移或验证失败：保留原明文，下次重试
                AppLog.w("MainActivity", "migrateLegacyDisplayNames",
                        "显示名迁移未完成，保留原明文待重试 personId=" + personId);
                continue;
            }
            prefs.edit().remove(key).apply();
        }
        if (allOk) {
            prefs.edit().putBoolean(PREF_NAME_MIGRATED, true).apply();
        }
    }

    // ==================== 任务18 / #1 结束 ====================

    private void onPermissionStateChanged(@NonNull PermissionStateMachine.State state, @NonNull List<String> missingRuntimePermissions) {
        AppLog.enter("MainActivity", "onPermissionStateChanged");
        boolean granted = (state == PermissionStateMachine.State.GRANTED);
        if (!granted && selectedCameraId != null) {
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
                    availableCameras.add(new CameraFragment.CameraInfo(id, desc, facing != null ? facing : -1));
                    displayNames.add(desc);

                    // Check if this matches selected ID
                    try {
                        if (id.equals(selectedCameraId)) {
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
        availableCameras.add(new CameraFragment.CameraInfo("-1", "Mock Source (File Picker)", -1));
        displayNames.add("Mock Source (File Picker)");
        
        availableCameras.add(new CameraFragment.CameraInfo("-2", "Mock Camera (System App)", -1));
        displayNames.add("Mock Camera (System App)");
        
        // If current selection is -1 (Mock), set index to last
        if (selectedCameraId == null) {
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
        // Release camera resources but keep Engine initialized
        if (activeCapture != null) {
            try {
                activeCapture.stop();
                AppLog.d("MainActivity", "onPause", "Camera released, Engine kept initialized");
            } catch (Exception ignored) {
            }
            activeCapture = null;
        }
        pendingResumeStart = (isRunning && selectedCameraId != null);
    }

    @Override
    protected void onStart() {
        super.onStart();
        // 复位停止门：回到前台后允许轮询与排程重新生效
        isStopped = false;
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
                // Surface-aware resume: poll for surface readiness (max 1s, 100ms intervals)
                final long startWaitMs = SystemClock.elapsedRealtime();
                final long maxWaitMs = 1000;
                handler.post(new Runnable() {
                    @Override
                    public void run() {
                        // 停止门：onStop 后终止轮询，不再自我重排（堵住误启动窗口的关键）
                        if (isStopped) return;
                        long waited = SystemClock.elapsedRealtime() - startWaitMs;
                        if (previewSurfaceReady || waited >= maxWaitMs) {
                            AppLog.i("MainActivity", "onStart",
                                "Surface ready=" + previewSurfaceReady + " waitedMs=" + waited);
                            // 代次化启动：轮询到期统一走 scheduleStartMonitoring(0)，
                            // 由取消门拦截 onStop/onDestroy 后的陈旧触发。
                            scheduleStartMonitoring(0);
                        } else {
                            handler.postDelayed(this, 100);
                        }
                    }
                });
            }
        }
    }

    @Override
    protected void onStop() {
        // 置位停止门：后台后终止 surface 轮询续排与排程回调，闭合误启动窗口
        isStopped = true;
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
        // 任务 B：销毁即丢弃采集期的内存显示名（从未落盘，故无存储残留需要清理）。
        pendingEnrollmentDisplayName = null;
        monitoringCoordinator.onActivityDestroyed(snapshotMonitoringInputs());  // 递增代次使排程任务失效
        stopMonitoring();
        if (statsUpdater != null) {
            handler.removeCallbacks(statsUpdater);
        }
        if (inferenceAutoUpdater != null) {
            handler.removeCallbacks(inferenceAutoUpdater);
            inferenceAutoUpdater = null;
        }
        // 任务18：销毁时停止注册轮询并清零会话保留的向量（模板红线：释放前必须清零）。
        stopEnrollmentPoller();
        stopNativeEnrollmentCapture();  // #1 断开 native 特征源并释放采集缓冲
        enrollmentInProgress = false;
        if (enrollmentSession != null) {
            enrollmentSession.cancel();
            enrollmentSession = null;
        }
        // #4：销毁时若注册对话框仍可见则关闭，避免窗口泄漏与陈旧回调。
        // 此时 enrollmentInProgress 已置 false，onDismiss 监听只会作废代次、不会复活会话。
        if (enrollmentDialog != null) {
            try { enrollmentDialog.dismiss(); } catch (Throwable ignored) {}
            enrollmentDialog = null;
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
