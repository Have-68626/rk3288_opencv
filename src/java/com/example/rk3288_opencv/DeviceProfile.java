package com.example.rk3288_opencv;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.StatFs;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.util.Locale;

final class DeviceProfile {
    final String manufacturer;
    final String brand;
    final String model;
    final String device;
    final String hardware;
    final String board;
    final String fingerprint;
    final String buildType;
    final int sdkInt;
    final long totalMemBytes;
    final long dataTotalBytes;
    final boolean hasGms;

    private DeviceProfile(String manufacturer,
                          String brand,
                          String model,
                          String device,
                          String hardware,
                          String board,
                          String fingerprint,
                          String buildType,
                          int sdkInt,
                          long totalMemBytes,
                          long dataTotalBytes,
                          boolean hasGms) {
        this.manufacturer = manufacturer;
        this.brand = brand;
        this.model = model;
        this.device = device;
        this.hardware = hardware;
        this.board = board;
        this.fingerprint = fingerprint;
        this.buildType = buildType;
        this.sdkInt = sdkInt;
        this.totalMemBytes = totalMemBytes;
        this.dataTotalBytes = dataTotalBytes;
        this.hasGms = hasGms;
    }

    static DeviceProfile forTest(String manufacturer,
                                 String brand,
                                 String model,
                                 String device,
                                 String hardware,
                                 String board,
                                 String fingerprint,
                                 String buildType,
                                 int sdkInt,
                                 long totalMemBytes,
                                 long dataTotalBytes,
                                 boolean hasGms) {
        return new DeviceProfile(manufacturer, brand, model, device, hardware, board, fingerprint, buildType, sdkInt, totalMemBytes, dataTotalBytes, hasGms);
    }

    @NonNull
    static DeviceProfile collect(@NonNull Context context) {
        String manufacturer = nullToEmpty(Build.MANUFACTURER);
        String brand = nullToEmpty(Build.BRAND);
        String model = nullToEmpty(Build.MODEL);
        String device = nullToEmpty(Build.DEVICE);
        String hardware = nullToEmpty(Build.HARDWARE);
        String board = nullToEmpty(Build.BOARD);
        String fingerprint = nullToEmpty(Build.FINGERPRINT);
        String buildType = nullToEmpty(Build.TYPE);
        int sdkInt = Build.VERSION.SDK_INT;

        long totalMem = readTotalMemBytes(context);
        long dataTotal = readDataTotalBytes();
        boolean hasGms = false; // Simplified for basic profiling

        return new DeviceProfile(manufacturer, brand, model, device, hardware, board, fingerprint, buildType, sdkInt, totalMem, dataTotal, hasGms);
    }

    @NonNull
    String toShortString() {
        return String.format(Locale.US,
                "manu=%s brand=%s model=%s hw=%s board=%s sdk=%d type=%s mem=%dB data=%dB gms=%s",
                manufacturer, brand, model, hardware, board, sdkInt, buildType, totalMemBytes, dataTotalBytes, hasGms);
    }

    @NonNull
    private static String nullToEmpty(@Nullable String s) {
        return s == null ? "" : s;
    }

    private static long readTotalMemBytes(@NonNull Context context) {
        try {
            ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) return -1;
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            return mi.totalMem;
        } catch (Throwable ignored) {
            return -1;
        }
    }

    private static long readDataTotalBytes() {
        try {
            File data = new File("/data");
            StatFs stat = new StatFs(data.getAbsolutePath());
            return stat.getTotalBytes();
        } catch (Throwable ignored) {
            return -1;
        }
    }
}
