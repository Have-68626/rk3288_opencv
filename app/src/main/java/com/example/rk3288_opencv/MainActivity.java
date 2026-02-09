package com.example.rk3288_opencv;

import android.Manifest;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
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
    private static final String TAG = "MainActivity";

    private ImageView monitorView;
    private TextView tvFps, tvCpu, tvMemory, tvStatus, tvOverlayStatus;
    private Button btnStartStop, btnViewLogs;
    private RadioGroup rgMode;
    private Spinner spinnerCameras;
    private Switch switchOverlay;
    
    private boolean isRunning = false;
    private boolean engineInitialized = false;
    private boolean firstFrameReceived = false;
    private boolean lastReadyVisible = false;
    private PermissionStateMachine permissionStateMachine;
    private Bitmap frameBitmap;
    private Handler handler = new Handler(Looper.getMainLooper());
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
    public native boolean nativeInit(int cameraId);
    public native boolean nativeInitFile(String filePath);
    public native void nativeStart();
    public native void nativeStop();
    public native void nativeSetMode(int mode);
    public native boolean nativeGetFrame(Bitmap bitmap);

    private static final int REQUEST_CODE_PICK_MEDIA = 1001;
    private static final int REQUEST_CODE_TAKE_PHOTO = 1003;
    private String mockFilePath = null;
    private String currentPhotoPath;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AppLog.enter("MainActivity", "onCreate");
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // Initialize Views
        monitorView = findViewById(R.id.monitor_view);
        tvFps = findViewById(R.id.tv_fps);
        tvCpu = findViewById(R.id.tv_cpu);
        tvMemory = findViewById(R.id.tv_memory);
        tvStatus = findViewById(R.id.tv_app_status);
        tvOverlayStatus = findViewById(R.id.tv_overlay_status);
        btnStartStop = findViewById(R.id.btn_start_stop);
        btnViewLogs = findViewById(R.id.btn_view_logs);
        rgMode = findViewById(R.id.rg_mode);
        spinnerCameras = findViewById(R.id.spinner_cameras);
        switchOverlay = findViewById(R.id.switch_overlay);
        tvOverlayStatus.setVisibility(View.GONE);

        // Initialize Bitmap (VGA)
        frameBitmap = Bitmap.createBitmap(640, 480, Bitmap.Config.ARGB_8888);
        monitorView.setImageBitmap(frameBitmap);

        // Load Preferences
        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        selectedCameraId = prefs.getInt(PREF_CAMERA_ID, 0);

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
                if (Settings.canDrawOverlays(this)) {
                    startService(new Intent(this, StatusService.class));
                } else {
                    Intent intent = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                            Uri.parse("package:" + getPackageName()));
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
                if (isRunning) {
                    if (nativeGetFrame(frameBitmap)) {
                        monitorView.invalidate(); // Request redraw
                        if (!firstFrameReceived) {
                            firstFrameReceived = true;
                            updateSystemReadyUi("首帧到达");
                        }
                    }
                    handler.postDelayed(this, 33); // ~30 FPS
                }
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
        btnStartStop.setText("STOP MONITORING");
        btnStartStop.setBackgroundColor(0xFFFF0000); // Red
        tvStatus.setText("Status: Running");
        handler.post(frameUpdater);
        updateSystemReadyUi("已启动监控");
    }

    private void stopMonitoring() {
        AppLog.enter("MainActivity", "stopMonitoring");
        isRunning = false;
        handler.removeCallbacks(frameUpdater);
        nativeStop();
        btnStartStop.setText("START MONITORING");
        btnStartStop.setBackgroundColor(0xFF00FF00); // Green
        tvStatus.setText("Status: Stopped");
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
        
        if (selectedCameraId == -1 && mockFilePath != null) {
            engineInitialized = nativeInitFile(mockFilePath);
            if (engineInitialized) {
                tvStatus.setText("Status: Engine Initialized (Mock Mode)");
                updateSystemReadyUi("引擎初始化成功 (Mock)");
            } else {
                tvStatus.setText("Status: Engine Init FAILED (Mock)");
                updateSystemReadyUi("引擎初始化失败 (Mock)");
            }
        } else {
            engineInitialized = nativeInit(selectedCameraId);
            if (engineInitialized) {
                tvStatus.setText("Status: Engine Initialized (Cam " + selectedCameraId + ")");
                updateSystemReadyUi("引擎初始化成功");
            } else {
                tvStatus.setText("Status: Engine Init FAILED");
                updateSystemReadyUi("引擎初始化失败");
            }
        }
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
            AppLog.e("MainActivity", "updateSystemReadyUi", "SYSTEM NOT READY: " + reason);
        }
        tvOverlayStatus.setVisibility(View.GONE);
        lastReadyVisible = false;
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
            initEngine();
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
                    } catch (NumberFormatException ignored) {}

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
    protected void onDestroy() {
        super.onDestroy();
        stopMonitoring();
        if (statsUpdater != null) {
            handler.removeCallbacks(statsUpdater);
        }
        unregisterReceiver(usbReceiver);
    }
}
