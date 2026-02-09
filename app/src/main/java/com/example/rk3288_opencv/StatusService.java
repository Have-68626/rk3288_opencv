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
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.WindowManager;
import android.widget.TextView;

import androidx.annotation.Nullable;

import java.util.Locale;

public class StatusService extends Service {

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
        windowManager = (WindowManager) getSystemService(WINDOW_SERVICE);
        createStatusView();
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

        windowManager.addView(statusView, params);

        // Start Update Loop
        isRunning = true;
        updateStats();
    }

    private void updateStats() {
        if (!isRunning) return;

        StatsSnapshot s = StatsRepository.getInstance().getSnapshot();
        String fps = s.fps == null ? "--" : String.format(Locale.US, "%.1f", s.fps);
        String cpu = s.cpuPercent == null ? "--" : String.format(Locale.US, "%.1f%%", s.cpuPercent);
        String mem = s.memPssMb == null ? "--" : String.format(Locale.US, "%.0fMB", s.memPssMb);

        if (s.fps == null && s.fpsError != null) {
            AppLog.e("StatusService", "updateStats", "FPS采集失败: " + s.fpsError);
        }
        if (s.cpuPercent == null && s.cpuError != null) {
            AppLog.e("StatusService", "updateStats", "CPU采集失败: " + s.cpuError);
        }
        if (s.memPssMb == null && s.memError != null) {
            AppLog.e("StatusService", "updateStats", "MEM采集失败: " + s.memError);
        }

        String text = String.format(Locale.US, "FPS: %s | CPU: %s | MEM: %s", fps, cpu, mem);
        tvInfo.setText(text);

        handler.postDelayed(this::updateStats, refreshInterval);
    }

    @Override
    public void onDestroy() {
        AppLog.enter("StatusService", "onDestroy");
        super.onDestroy();
        isRunning = false;
        if (statusView != null) {
            windowManager.removeView(statusView);
        }
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
