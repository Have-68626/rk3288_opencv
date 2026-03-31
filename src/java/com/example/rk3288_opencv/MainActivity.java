package com.example.rk3288_opencv;

import android.Manifest;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.Settings;
import android.animation.ValueAnimator;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.animation.LinearInterpolator;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
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
import java.text.SimpleDateFormat;
import java.util.Date;
import java.io.File;
import java.io.IOException;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class MainActivity extends AppCompatActivity {

    // Load native library
    static {
        System.loadLibrary("native-lib");
    }
    
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_CAMERA_ID = "pref_camera_id";
    private static final String PREF_EVENTS_VISIBLE = "pref_events_visible";
    private static final String TAG = "MainActivity";

    private ImageView monitorView;
    private OsdOverlayView osdView;
    private TextView tvFps, tvCpu, tvMemory, tvStatus, tvOverlayStatus;
    private View recognitionTitle;
    private Button btnStartStop, btnViewLogs;
    private Button btnNavPlayback, btnNavSettings, btnNavAbout;
    private RadioGroup rgMode;
    private Spinner spinnerCameras;
    private Switch switchOverlay;
    private View panelSettings;
    private View videoWrapper;
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
    private boolean firstFrameReceived = false;
    private boolean lastReadyVisible = false;
    private boolean restartMonitoringOnStart = false;
    private PermissionStateMachine permissionStateMachine;
    private Bitmap frameBitmap;
    private Bitmap backBitmap;
    private final Object bitmapSwapLock = new Object();
    private Handler handler = new Handler(Looper.getMainLooper());
    private HandlerThread frameThread;
    private Handler frameHandler;
    private Runnable frameUpdater;
    private Runnable statsUpdater;
    
    private int selectedCameraId = 0;
    private List<CameraInfo> availableCameras = new ArrayList<>();
    private ArrayAdapter<String> cameraAdapter;
    private boolean isSpinnerInitialized = false;

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
    public native void nativeSetMode(int mode);
    public native boolean nativeGetFrame(Bitmap bitmap);

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

        // Initialize Views
        monitorView = findViewById(R.id.monitor_view);
        videoWrapper = findViewById(R.id.video_wrapper);
        osdView = findViewById(R.id.osd_view);
        tvFps = findViewById(R.id.tv_fps);
        tvCpu = findViewById(R.id.tv_cpu);
        tvMemory = findViewById(R.id.tv_memory);
        tvStatus = findViewById(R.id.tv_app_status);
        tvOverlayStatus = findViewById(R.id.tv_overlay_status);
        recognitionTitle = findViewById(R.id.recognition_title);
        btnStartStop = findViewById(R.id.btn_start_stop);
        btnViewLogs = findViewById(R.id.btn_view_logs);
        btnNavPlayback = findViewById(R.id.btn_nav_playback);
        btnNavSettings = findViewById(R.id.btn_nav_settings);
        btnNavAbout = findViewById(R.id.btn_nav_about);
        rgMode = findViewById(R.id.rg_mode);
        spinnerCameras = findViewById(R.id.spinner_cameras);
        switchOverlay = findViewById(R.id.switch_overlay);
        panelSettings = findViewById(R.id.panel_settings);
        etMockUrl = findViewById(R.id.et_mock_url);
        btnSetMockUrl = findViewById(R.id.btn_set_mock_url);
        etRtmpUrl = findViewById(R.id.et_rtmp_url);
        btnPushRtmp = findViewById(R.id.btn_push_rtmp);
        btnStopRtmp = findViewById(R.id.btn_stop_rtmp);
        panelRecognitionEvents = findViewById(R.id.panel_recognition_events);
        rvRecognitionEvents = findViewById(R.id.rv_recognition_events);
        fabToggleEvents = findViewById(R.id.fab_toggle_events);
        tvOverlayStatus.setVisibility(View.GONE);

        if (osdView != null) {
            osdView.setWatermark("RK3288");
        }

        frameThread = new HandlerThread("FrameWorker");
        frameThread.start();
        frameHandler = new Handler(frameThread.getLooper());

        // Initialize Bitmap (VGA)
        frameBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888);
        backBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888);
        monitorView.setImageBitmap(frameBitmap);
        monitorGestureDetector = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                toggleFullscreen();
                return true;
            }
        });
        monitorView.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));

        // Load Preferences
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        selectedCameraId = prefs.getInt(PREF_CAMERA_ID, 0);
        recognitionEventsVisible = prefs.getBoolean(PREF_EVENTS_VISIBLE, true);

        rvRecognitionEvents.setLayoutManager(new LinearLayoutManager(this));
        recognitionEventAdapter = new RecognitionEventAdapter();
        rvRecognitionEvents.setAdapter(recognitionEventAdapter);
        setRecognitionPanelVisible(recognitionEventsVisible, false);

        fabToggleEvents.setOnClickListener(v -> toggleRecognitionPanel());

        // Setup Spinner
        cameraAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        cameraAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerCameras.setAdapter(cameraAdapter);
        
        spinnerCameras.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (!isSpinnerInitialized) {
                    isSpinnerInitialized = true;
                    return;
                }
                
                CameraInfo info = availableCameras.get(position);
                
                // Check for Mock Source
                if ("-1".equals(info.id)) {
                    AppLog.i("MainActivity", "onItemSelected", "Selected Mock Source");
                    pickMediaFile();
                    return;
                }
                
                // Check for System Camera Mock
                if ("-2".equals(info.id)) {
                    AppLog.i("MainActivity", "onItemSelected", "Selected System Camera Mock");
                    dispatchTakePictureIntent();
                    return;
                }

                try {
                    int newId = Integer.parseInt(info.id);
                    if (newId != selectedCameraId) {
                        selectedCameraId = newId;
                        mockFilePath = null; // Clear mock path
                        AppLog.i("MainActivity", "onItemSelected", "切换 Camera ID: " + selectedCameraId);
                        
                        // Save preference
                        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                            .edit()
                            .putInt(PREF_CAMERA_ID, selectedCameraId)
                            .apply();

                        // Restart Engine if running
                        if (isRunning) {
                            stopMonitoring();
                            // Small delay to ensure native cleanup
                            handler.postDelayed(() -> startMonitoring(), 500);
                        } else {
                            // Re-init engine with new ID
                            initEngine();
                        }
                    }
                } catch (NumberFormatException e) {
                    AppLog.e("MainActivity", "onItemSelected", "Camera ID 非法: " + info.id, e);
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) { }
        });

        // Register USB Receiver
        IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        registerReceiver(usbReceiver, filter);
        
        // Setup Overlay Switch
        switchOverlay.setOnCheckedChangeListener((buttonView, isChecked) -> {
            if (isChecked) {
                boolean granted = Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(this);
                if (granted) {
                    startService(new Intent(this, StatusService.class));
                } else {
                    if (Build.VERSION.SDK_INT < 23) {
                        Toast.makeText(this, "当前系统版本不支持悬浮窗权限设置入口", Toast.LENGTH_SHORT).show();
                        buttonView.setChecked(false);
                        return;
                    }
                    Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + getPackageName()));
                    new AlertDialog.Builder(this)
                            .setTitle("需要悬浮窗权限")
                            .setMessage("开启悬浮窗将显示 FPS/CPU/MEM 指标，需要在系统设置授予“在其他应用上层显示”。")
                            .setPositiveButton("打开设置", (d, w) -> startActivityForResult(intent, 1002))
                            .setNegativeButton("取消", null)
                            .show();
                    buttonView.setChecked(false);
                }
            } else {
                stopService(new Intent(this, StatusService.class));
            }
        });

        // Discover Cameras
        discoverCameras();

        permissionStateMachine = new PermissionStateMachine(this, this::onPermissionStateChanged);
        permissionStateMachine.evaluate();
        if (!permissionStateMachine.isRuntimeGranted()) {
            permissionStateMachine.requestWithUserConfirmation();
        }

        // Button Listeners
        btnStartStop.setOnClickListener(v -> {
            if (isRunning) {
                stopMonitoring();
            } else {
                startMonitoring();
            }
        });

        if (btnNavPlayback != null) {
            btnNavPlayback.setOnClickListener(v -> pickMediaFile());
        }
        if (btnNavSettings != null) {
            btnNavSettings.setOnClickListener(v -> toggleSettingsPanel());
        }
        if (btnNavAbout != null) {
            btnNavAbout.setOnClickListener(v -> showAboutDialog());
        }

        if (btnSetMockUrl != null) {
            btnSetMockUrl.setOnClickListener(v -> applyMockUrl());
        }
        if (btnPushRtmp != null) {
            btnPushRtmp.setOnClickListener(v -> startRtmpPush());
        }
        if (btnStopRtmp != null) {
            btnStopRtmp.setOnClickListener(v -> stopRtmpPush());
        }

        rgMode.setOnCheckedChangeListener((group, checkedId) -> {
            int mode = (checkedId == R.id.rb_continuous) ? 0 : 1; // 0=Continuous, 1=Motion
            nativeSetMode(mode);
        });

        btnViewLogs.setOnClickListener(v -> {
            Intent intent = new Intent(MainActivity.this, LogViewerActivity.class);
            startActivity(intent);
        });

        // Frame Update Loop
        frameUpdater = new Runnable() {
            @Override
            public void run() {
                if (!isRunning) return;
                Bitmap target;
                synchronized (bitmapSwapLock) {
                    target = backBitmap;
                }
                boolean ok = nativeGetFrame(target);
                if (!ok) {
                    if (frameHandler != null) {
                        frameHandler.postDelayed(this, 33);
                    }
                    return;
                }

                handler.post(() -> {
                    if (!isRunning) return;
                    synchronized (bitmapSwapLock) {
                        Bitmap tmp = frameBitmap;
                        frameBitmap = backBitmap;
                        backBitmap = tmp;
                    }
                    if (monitorView != null) {
                        monitorView.setImageBitmap(frameBitmap);
                        monitorView.invalidate();
                    }
                    if (!firstFrameReceived) {
                        firstFrameReceived = true;
                        applyMonitorLayoutRule(frameBitmap.getWidth(), frameBitmap.getHeight());
                        updateSystemReadyUi("首帧到达");
                    }
                    if (frameHandler != null) {
                        frameHandler.postDelayed(frameUpdater, 33);
                    }
                });
            }
        };

        statsUpdater = new Runnable() {
            @Override
            public void run() {
                StatsSnapshot s = StatsRepository.getInstance().getSnapshot();
                if (s.fps == null) {
                    tvFps.setText("FPS: --");
                } else {
                    tvFps.setText(String.format(Locale.US, "FPS: %.1f", s.fps));
                }
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
        monitorView = findViewById(R.id.monitor_view);
        videoWrapper = findViewById(R.id.video_wrapper);
        osdView = findViewById(R.id.osd_view);
        tvFps = findViewById(R.id.tv_fps);
        tvCpu = findViewById(R.id.tv_cpu);
        tvMemory = findViewById(R.id.tv_memory);
        tvStatus = findViewById(R.id.tv_app_status);
        tvOverlayStatus = findViewById(R.id.tv_overlay_status);
        recognitionTitle = findViewById(R.id.recognition_title);
        btnStartStop = findViewById(R.id.btn_start_stop);
        btnViewLogs = findViewById(R.id.btn_view_logs);
        btnNavPlayback = findViewById(R.id.btn_nav_playback);
        btnNavSettings = findViewById(R.id.btn_nav_settings);
        btnNavAbout = findViewById(R.id.btn_nav_about);
        rgMode = findViewById(R.id.rg_mode);
        spinnerCameras = findViewById(R.id.spinner_cameras);
        switchOverlay = findViewById(R.id.switch_overlay);
        panelSettings = findViewById(R.id.panel_settings);
        etMockUrl = findViewById(R.id.et_mock_url);
        btnSetMockUrl = findViewById(R.id.btn_set_mock_url);
        etRtmpUrl = findViewById(R.id.et_rtmp_url);
        btnPushRtmp = findViewById(R.id.btn_push_rtmp);
        btnStopRtmp = findViewById(R.id.btn_stop_rtmp);
        panelRecognitionEvents = findViewById(R.id.panel_recognition_events);
        rvRecognitionEvents = findViewById(R.id.rv_recognition_events);
        fabToggleEvents = findViewById(R.id.fab_toggle_events);
        tvOverlayStatus.setVisibility(View.GONE);
        if (osdView != null) {
            osdView.setWatermark("RK3288");
        }

        if (frameBitmap != null) {
            monitorView.setImageBitmap(frameBitmap);
        }
        monitorGestureDetector = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onDoubleTap(MotionEvent e) {
                toggleFullscreen();
                return true;
            }
        });
        monitorView.setOnTouchListener((v, event) -> monitorGestureDetector != null && monitorGestureDetector.onTouchEvent(event));

        if (rvRecognitionEvents != null) {
            rvRecognitionEvents.setLayoutManager(new LinearLayoutManager(this));
            if (recognitionEventAdapter != null) {
                rvRecognitionEvents.setAdapter(recognitionEventAdapter);
            }
        }
        setRecognitionPanelVisible(recognitionEventsVisible, false);
        if (fabToggleEvents != null) {
            fabToggleEvents.setOnClickListener(v -> toggleRecognitionPanel());
        }

        if (cameraAdapter != null && spinnerCameras != null) {
            spinnerCameras.setAdapter(cameraAdapter);
            isSpinnerInitialized = false;
            spinnerCameras.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                    if (!isSpinnerInitialized) {
                        isSpinnerInitialized = true;
                        return;
                    }

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
                        if (newId != selectedCameraId) {
                            selectedCameraId = newId;
                            mockFilePath = null;
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
                            }
                        }
                    } catch (NumberFormatException e) {
                        AppLog.e("MainActivity", "onItemSelected", "Camera ID 非法: " + info.id, e);
                    }
                }

                @Override
                public void onNothingSelected(AdapterView<?> parent) {
                }
            });
        }

        if (switchOverlay != null) {
            switchOverlay.setOnCheckedChangeListener((buttonView, isChecked) -> {
                if (isChecked) {
                    boolean granted = Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(this);
                    if (granted) {
                        startService(new Intent(this, StatusService.class));
                    } else {
                        if (Build.VERSION.SDK_INT < 23) {
                            Toast.makeText(this, "当前系统版本不支持悬浮窗权限设置入口", Toast.LENGTH_SHORT).show();
                            buttonView.setChecked(false);
                            return;
                        }
                        Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:" + getPackageName()));
                        new AlertDialog.Builder(this)
                                .setTitle("需要悬浮窗权限")
                                .setMessage("开启悬浮窗将显示 FPS/CPU/MEM 指标，需要在系统设置授予“在其他应用上层显示”。")
                                .setPositiveButton("打开设置", (d, w) -> startActivityForResult(intent, 1002))
                                .setNegativeButton("取消", null)
                                .show();
                        buttonView.setChecked(false);
                    }
                } else {
                    stopService(new Intent(this, StatusService.class));
                }
            });
        }

        if (btnStartStop != null) {
            btnStartStop.setOnClickListener(v -> {
                if (isRunning) {
                    stopMonitoring();
                } else {
                    startMonitoring();
                }
            });
        }
        if (btnViewLogs != null) {
            btnViewLogs.setOnClickListener(v -> {
                Intent intent = new Intent(MainActivity.this, LogViewerActivity.class);
                startActivity(intent);
            });
        }
        if (btnNavPlayback != null) {
            btnNavPlayback.setOnClickListener(v -> pickMediaFile());
        }
        if (btnNavSettings != null) {
            btnNavSettings.setOnClickListener(v -> toggleSettingsPanel());
        }
        if (btnNavAbout != null) {
            btnNavAbout.setOnClickListener(v -> showAboutDialog());
        }
        if (btnSetMockUrl != null) {
            btnSetMockUrl.setOnClickListener(v -> applyMockUrl());
        }
        if (btnPushRtmp != null) {
            btnPushRtmp.setOnClickListener(v -> startRtmpPush());
        }
        if (btnStopRtmp != null) {
            btnStopRtmp.setOnClickListener(v -> stopRtmpPush());
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
        // Show loading dialog
        ProgressDialog progressDialog = new ProgressDialog(this);
        progressDialog.setMessage("Loading mock file...");
        progressDialog.setCancelable(false);
        progressDialog.show();

        new Thread(() -> {
            try {
                // Get original filename and extension
                String tempFileName = "mock_source.dat"; // Fallback
                Cursor cursor = getContentResolver().query(uri, null, null, null, null);
                if (cursor != null && cursor.moveToFirst()) {
                    int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (nameIndex >= 0) {
                        tempFileName = cursor.getString(nameIndex);
                    }
                    cursor.close();
                }
                final String fileName = tempFileName;

                // Ensure unique name or fixed name with correct extension?
                // We prefer fixed name prefix to avoid clutter, but MUST keep extension for OpenCV
                String extension = "";
                int dotIndex = fileName.lastIndexOf(".");
                if (dotIndex >= 0) {
                    extension = fileName.substring(dotIndex);
                }
                String saveName = "mock_source" + extension;

                java.io.InputStream is = getContentResolver().openInputStream(uri);
                if (is == null) {
                    runOnUiThread(() -> {
                        progressDialog.dismiss();
                        Toast.makeText(MainActivity.this, "Failed to open input stream", Toast.LENGTH_SHORT).show();
                    });
                    return;
                }
                
                java.io.File cacheFile = new java.io.File(getCacheDir(), saveName);
                java.io.FileOutputStream fos = new java.io.FileOutputStream(cacheFile);
                
                byte[] buffer = new byte[8192];
                int bytesRead;
                while ((bytesRead = is.read(buffer)) != -1) {
                    fos.write(buffer, 0, bytesRead);
                }
                fos.flush();
                fos.close();
                is.close();
                
                mockFilePath = cacheFile.getAbsolutePath();
                selectedCameraId = -1; // Flag for Mock
                
                runOnUiThread(() -> {
                    progressDialog.dismiss();
                    AppLog.i("MainActivity", "handleMockFileSelection", "Mock file ready: " + mockFilePath);
                    Toast.makeText(MainActivity.this, "Mock Source Selected: " + fileName, Toast.LENGTH_SHORT).show();
                    
                    // Re-init engine
                    if (isRunning) {
                        stopMonitoring();
                        handler.postDelayed(MainActivity.this::startMonitoring, 500);
                    } else {
                        initEngine();
                    }
                });
                
            } catch (Exception e) {
                runOnUiThread(() -> {
                    progressDialog.dismiss();
                    AppLog.e("MainActivity", "handleMockFileSelection", "Failed to process file", e);
                    Toast.makeText(MainActivity.this, "Failed to load file: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                });
            }
        }).start();
    }

    private void startMonitoring() {
        AppLog.enter("MainActivity", "startMonitoring");
        if (permissionStateMachine != null && !permissionStateMachine.isRuntimeGranted()) {
            Toast.makeText(this, "权限不足：已进入安全模式", Toast.LENGTH_LONG).show();
            permissionStateMachine.requestWithUserConfirmation();
            return;
        }

        if (!engineInitialized) {
            initEngine();
            if (!engineInitialized) {
                Toast.makeText(this, "引擎初始化失败，无法启动监控", Toast.LENGTH_LONG).show();
                return;
            }
        }
        firstFrameReceived = false;
        updateSystemReadyUi("开始监控");
        nativeStart();
        isRunning = true;
        if (btnStartStop != null) {
            btnStartStop.setText("STOP MONITORING");
            btnStartStop.setBackgroundColor(0xFFFF0000); // Red
        }
        if (tvStatus != null) {
            tvStatus.setText("Status: Running");
        }
        if (frameHandler != null) {
            frameHandler.post(frameUpdater);
        }
        updateSystemReadyUi("已启动监控");
    }

    private void stopMonitoring() {
        AppLog.enter("MainActivity", "stopMonitoring");
        isRunning = false;
        if (frameHandler != null) {
            frameHandler.removeCallbacks(frameUpdater);
        }
        nativeStop();
        rtmpPusher.stop();
        if (btnStartStop != null) {
            btnStartStop.setText("START MONITORING");
            btnStartStop.setBackgroundColor(0xFF00FF00); // Green
        }
        if (tvStatus != null) {
            tvStatus.setText("Status: Stopped");
        }
        updateSystemReadyUi("停止监控");
    }

    private void initEngine() {
        AppLog.enter("MainActivity", "initEngine");
        if (permissionStateMachine != null && !permissionStateMachine.isRuntimeGranted()) {
            engineInitialized = false;
            tvStatus.setText("Status: SAFE MODE (Permissions Missing)");
            updateSystemReadyUi("权限不足，禁止初始化引擎");
            return;
        }

        String cascadePath = copyAssetToCache("lbpcascade_frontalface.xml");
        String storagePath = getAppStoragePath();
        
        if (selectedCameraId == -1 && mockFilePath != null) {
            engineInitialized = nativeInitFile(mockFilePath, cascadePath, storagePath);
            if (engineInitialized) {
                tvStatus.setText("Status: Engine Initialized (Mock Mode)");
                updateSystemReadyUi("引擎初始化成功 (Mock)");
            } else {
                tvStatus.setText("Status: Engine Init FAILED (Mock)");
                updateSystemReadyUi("引擎初始化失败 (Mock)");
            }
        } else {
            engineInitialized = nativeInit(selectedCameraId, cascadePath, storagePath);
            if (engineInitialized) {
                tvStatus.setText("Status: Engine Initialized (Cam " + selectedCameraId + ")");
                updateSystemReadyUi("引擎初始化成功");
            } else {
                tvStatus.setText("Status: Engine Init FAILED");
                updateSystemReadyUi("引擎初始化失败");
            }
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

    private void updateSystemReadyUi(String reason) {
        boolean runtimeGranted = permissionStateMachine != null && permissionStateMachine.isRuntimeGranted();
        boolean ready = runtimeGranted && engineInitialized && isRunning && firstFrameReceived;
        if (ready) {
            if (!lastReadyVisible) {
                AppLog.i("MainActivity", "updateSystemReadyUi", "SYSTEM READY runtimeGranted=" + runtimeGranted + " engineInitialized=" + engineInitialized + " firstFrameReceived=" + firstFrameReceived);
            }
            tvOverlayStatus.setVisibility(View.VISIBLE);
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
        tvOverlayStatus.setVisibility(View.GONE);
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
                .setMessage("RK3288 AI Engine\\n\\n设备分类: " + c + "\\n" + p.toShortString())
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
        btnStartStop.setEnabled(granted);
        if (!granted) {
            if (isRunning) {
                stopMonitoring();
            }
            engineInitialized = false;
            firstFrameReceived = false;
            tvStatus.setText("Status: SAFE MODE (Missing Permissions)");
            updateSystemReadyUi("安全模式: " + state + " missing=" + missingRuntimePermissions);
            permissionStateMachine.showGoToSettingsDialogIfNeeded();
        } else {
            tvStatus.setText("Status: Permissions Granted");
            // 延迟初始化以确保权限已广播至系统服务
            handler.postDelayed(() -> {
                if (!isFinishing()) {
                    initEngine();
                }
            }, 500);
        }
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
        if (osdView != null) {
            osdView.onResume();
        }
        if (permissionStateMachine != null) {
            permissionStateMachine.evaluate();
        }
    }

    @Override
    protected void onPause() {
        if (osdView != null) {
            osdView.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onStart() {
        super.onStart();
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
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopMonitoring();
        if (statsUpdater != null) {
            handler.removeCallbacks(statsUpdater);
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
