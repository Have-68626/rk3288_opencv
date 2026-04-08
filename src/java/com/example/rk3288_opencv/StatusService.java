package com.example.rk3288_opencv;

import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

import androidx.annotation.Nullable;

import java.util.Locale;

public class StatusService extends Service {
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_OVERLAY_RUNNING = "pref_overlay_running";

    private WindowManager windowManager;
    private View statusView;
    private TextView tvInfo;
    
    private Handler handler = new Handler(Looper.getMainLooper());
    private boolean isRunning = false;
    private int refreshInterval = 500; // ms

    @Override
    public void onCreate() {
        AppLog.enter("StatusService", "onCreate");
        super.onCreate();
        setOverlayRunning(true);
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        createStatusView();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            stopSelf();
            return START_NOT_STICKY;
        }
        return START_STICKY;
    }

    private void createStatusView() {
        AppLog.enter("StatusService", "createStatusView");
        statusView = LayoutInflater.from(this).inflate(R.layout.layout_status_overlay, null);
        tvInfo = statusView.findViewById(R.id.tv_info);

        int layoutType;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            layoutType = WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY;
        } else {
            layoutType = WindowManager.LayoutParams.TYPE_PHONE;
        }

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                layoutType,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | 
                WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL |
                WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);

        params.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
        params.y = 0; // Top of screen

        if (windowManager != null) {
            windowManager.addView(statusView, params);
        }

        // Start Update Loop
        isRunning = true;
        updateStats();
    }

    private void updateStats() {
        if (!isRunning) return;

        StatsSnapshot s = StatsRepository.getInstance().getSnapshot();
        String fps = s.fps == null ? "--" : String.format(Locale.US, "%.1f", s.fps);
        String cap = s.captureFps == null ? "--" : String.format(Locale.US, "%.1f", s.captureFps);
        String lat = s.latencyMs == null ? "--" : String.format(Locale.US, "%.0fms", s.latencyMs);
        String cpu = s.cpuPercent == null ? "--" : String.format(Locale.US, "%.1f%%", s.cpuPercent);
        String mem = s.memPssMb == null ? "--" : String.format(Locale.US, "%.0fMB", s.memPssMb);

        if (s.fps == null && s.fpsError != null) {
            AppLog.e("StatusService", "updateStats", "FPS采集失败: " + s.fpsError);
        }
        if (s.captureFps == null && s.captureError != null) {
            AppLog.e("StatusService", "updateStats", "CAP采集失败: " + s.captureError);
        }
        if (s.latencyMs == null && s.latencyError != null) {
            AppLog.e("StatusService", "updateStats", "LAT采集失败: " + s.latencyError);
        }
        if (s.cpuPercent == null && s.cpuError != null) {
            AppLog.e("StatusService", "updateStats", "CPU采集失败: " + s.cpuError);
        }
        if (s.memPssMb == null && s.memError != null) {
            AppLog.e("StatusService", "updateStats", "MEM采集失败: " + s.memError);
        }

        String text = String.format(Locale.US, "FPS: %s | CAP: %s | LAT: %s | CPU: %s | MEM: %s", fps, cap, lat, cpu, mem);
        tvInfo.setText(text);

        handler.postDelayed(this::updateStats, refreshInterval);
    }

    @Override
    public void onDestroy() {
        AppLog.enter("StatusService", "onDestroy");
        super.onDestroy();
        isRunning = false;
        handler.removeCallbacksAndMessages(null);
        if (statusView != null) {
            try {
                if (windowManager != null) windowManager.removeView(statusView);
            } catch (Throwable ignored) {
            }
        }
        setOverlayRunning(false);
    }

    private void setOverlayRunning(boolean running) {
        try {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_OVERLAY_RUNNING, running).apply();
        } catch (Throwable ignored) {
        }
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
