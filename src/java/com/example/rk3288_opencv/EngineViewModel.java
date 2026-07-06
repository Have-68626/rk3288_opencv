package com.example.rk3288_opencv;

import android.app.Application;
import android.content.SharedPreferences;

import androidx.annotation.NonNull;
import androidx.lifecycle.AndroidViewModel;
import androidx.lifecycle.LiveData;
import androidx.lifecycle.MutableLiveData;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

/**
 * Engine lifecycle state management.
 * Extracted from MainActivity as preliminary split (P1.7).
 */
public class EngineViewModel extends AndroidViewModel {

    private final MutableLiveData<Boolean> engineInitialized = new MutableLiveData<>(false);
    private final MutableLiveData<Boolean> isRunning = new MutableLiveData<>(false);
    private final MutableLiveData<Boolean> firstFrameReceived = new MutableLiveData<>(false);
    public volatile boolean cancelInitMock = false;
    public boolean lastReadyVisible = false;

    public static final String PREFS_NAME = "RK3288_Prefs";

    public LiveData<Boolean> getEngineInitialized() { return engineInitialized; }
    public LiveData<Boolean> getIsRunning() { return isRunning; }
    public LiveData<Boolean> getFirstFrameReceived() { return firstFrameReceived; }

    public EngineViewModel(@NonNull Application application) {
        super(application);
    }

    /**
     * Get app storage path for model files and configuration.
     */
    public String getAppStoragePath() {
        File f = getApplication().getExternalFilesDir(null);
        if (f != null) return f.getAbsolutePath() + "/";
        return getApplication().getFilesDir().getAbsolutePath() + "/";
    }

    /**
     * Copy an asset file from APK to internal storage cache.
     */
    public String copyAssetToCache(String fileName) {
        File cacheFile = new File(getApplication().getFilesDir(), fileName);
        if (!cacheFile.exists()) {
            try (InputStream is = getApplication().getAssets().open(fileName);
                 FileOutputStream fos = new FileOutputStream(cacheFile)) {
                byte[] buffer = new byte[1024];
                int size;
                while ((size = is.read(buffer)) != -1) {
                    fos.write(buffer, 0, size);
                }
            } catch (IOException e) {
                AppLog.e("EngineViewModel", "copyAssetToCache",
                    "Failed to copy asset: " + fileName, e);
                return "";
            }
        }
        return cacheFile.getAbsolutePath();
    }

    /**
     * Set environment variables for acceleration features via setenv.
     */
    public void setAccelerationEnv(boolean useOpencl, boolean useLibyuv,
                                   boolean useMpp, boolean useQualcomm) {
        try {
            android.system.Os.setenv("RK_USE_OPENCL", useOpencl ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_LIBYUV", useLibyuv ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_MPP", useMpp ? "1" : "0", true);
            android.system.Os.setenv("RK_USE_QUALCOMM", useQualcomm ? "1" : "0", true);
        } catch (Exception e) {
            AppLog.e("EngineViewModel", "setAccelerationEnv",
                "Failed to setenv for acceleration", e);
        }
    }

    /**
     * Parse a CaptureScheme from string.
     */
    public static CaptureScheme parseCaptureScheme(String raw, CaptureScheme fallback) {
        if (raw == null || raw.trim().isEmpty()) return fallback;
        try {
            return CaptureScheme.valueOf(raw.trim());
        } catch (Exception ignored) {
            return fallback;
        }
    }
}
