package com.example.rk3288_opencv;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
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
import android.app.PendingIntent;

import androidx.annotation.Nullable;

import java.util.Locale;

public class StatusService extends Service {
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_OVERLAY_RUNNING = "pref_overlay_running";
    private static final String PREF_OVERLAY_ENABLED = "pref_overlay_enabled";

    static final String ACTION_SET_FOREGROUND = "com.example.rk3288_opencv.action.SET_FOREGROUND";
    static final String EXTRA_FOREGROUND = "foreground";
    private static final String ACTION_STOP = "com.example.rk3288_opencv.action.STOP_OVERLAY";

    private static final String NOTIFICATION_CHANNEL_ID = "rk3288_overlay";
    private static final int NOTIFICATION_ID = 2201;

    private WindowManager windowManager;
    private View statusView;
    private TextView tvInfo;
    
    private Handler handler = new Handler(Looper.getMainLooper());
    private boolean isRunning = false;
    private boolean isForeground = false;
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
        if (intent != null) {
            String action = intent.getAction();
            if (ACTION_STOP.equals(action)) {
                try {
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_OVERLAY_ENABLED, false).apply();
                } catch (Throwable ignored) {
                }
                stopSelf();
                return START_NOT_STICKY;
            }
            if (ACTION_SET_FOREGROUND.equals(action)) {
                boolean fg = intent.getBooleanExtra(EXTRA_FOREGROUND, false);
                updateForeground(fg);
            }
        }
        return START_STICKY;
    }

    private void updateForeground(boolean foreground) {
        if (foreground == isForeground) return;
        if (!foreground) {
            try {
                stopForeground(false);
            } catch (Throwable ignored) {
            }
            isForeground = false;
            return;
        }

        try {
            ensureNotificationChannel();
            Notification n = buildNotification();
            startForeground(NOTIFICATION_ID, n);
            isForeground = true;
        } catch (Throwable t) {
            AppLog.e("StatusService", "updateForeground", "前台服务启动失败: " + t.getMessage(), t);
            isForeground = false;
        }
    }

    private void ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT < 26) return;
        try {
            NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
            if (nm == null) return;
            NotificationChannel ch = nm.getNotificationChannel(NOTIFICATION_CHANNEL_ID);
            if (ch != null) return;
            NotificationChannel created = new NotificationChannel(
                    NOTIFICATION_CHANNEL_ID,
                    "悬浮窗运行状态",
                    NotificationManager.IMPORTANCE_LOW
            );
            created.setShowBadge(false);
            nm.createNotificationChannel(created);
        } catch (Throwable ignored) {
        }
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, MainActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        int piFlags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) piFlags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent openPi = PendingIntent.getActivity(this, 0, open, piFlags);

        Intent stop = new Intent(this, StatusService.class);
        stop.setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(this, 1, stop, piFlags);

        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, NOTIFICATION_CHANNEL_ID)
                : new Notification.Builder(this);
        b.setContentTitle("RK3288 悬浮窗运行中")
                .setContentText("后台保持运行以避免被系统停止")
                .setSmallIcon(android.R.drawable.stat_notify_more)
                .setContentIntent(openPi)
                .setOngoing(true)
                .addAction(android.R.drawable.ic_menu_close_clear_cancel, "关闭悬浮窗", stopPi);
        return b.build();
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
        try {
            stopForeground(true);
        } catch (Throwable ignored) {
        }
        isForeground = false;
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
