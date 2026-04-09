package com.example.rk3288_opencv;

import android.content.Context;
import android.os.Process;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

final class AppLog {
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_LOG_RETENTION_DAYS = "pref_log_retention_days";
    private static final int LOG_MAX_FILES = 20;
    private static final int[] RETENTION_CHOICES_DAYS = new int[]{1, 7, 14, 30};
    enum Level {
        V(Log.VERBOSE),
        D(Log.DEBUG),
        I(Log.INFO),
        W(Log.WARN),
        E(Log.ERROR),
        F(Log.ASSERT);

        final int androidLevel;

        Level(int androidLevel) {
            this.androidLevel = androidLevel;
        }
    }

    private static final ThreadLocal<SimpleDateFormat> TS = new ThreadLocal<SimpleDateFormat>() {
        @Override
        protected SimpleDateFormat initialValue() {
            return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.CHINA);
        }
    };

    private static volatile FileLogSink sink;
    private static volatile Level minLevel = Level.D;
    private static volatile Set<String> allowedModules;

    private AppLog() {
    }

    static void init(@NonNull Context context) {
        File internalLogs = new File(context.getFilesDir(), "logs");
        File externalLogs = resolveExternalLogsDir(context);

        List<File> dirs = new ArrayList<>();
        dirs.add(internalLogs);
        if (externalLogs != null) {
            dirs.add(externalLogs);
        }

        // Generate Session ID (Timestamp)
        String sessionTs = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.CHINA).format(new Date());
        String sessionFileName = "rk3288_" + sessionTs + ".log";

        sink = new FileLogSink(dirs, sessionFileName);

        // Cleanup old logs
        for (File dir : dirs) {
            cleanupOldLogs(dir, context);
        }

        String internalPath = internalLogs.getAbsolutePath();
        String externalPath = externalLogs == null ? "" : externalLogs.getAbsolutePath();
        try {
            NativeBridge.nativeConfigureLog(internalPath, externalPath, sessionFileName);
        } catch (Throwable ignored) {
        }
    }

    static int getLogRetentionDays(@NonNull Context context) {
        int v = 7;
        try {
            v = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).getInt(PREF_LOG_RETENTION_DAYS, 7);
        } catch (Throwable ignored) {
        }
        for (int allowed : RETENTION_CHOICES_DAYS) {
            if (allowed == v) return v;
        }
        return 7;
    }

    static void setLogRetentionDays(@NonNull Context context, int days) {
        int v = 7;
        for (int allowed : RETENTION_CHOICES_DAYS) {
            if (allowed == days) {
                v = days;
                break;
            }
        }
        try {
            context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit().putInt(PREF_LOG_RETENTION_DAYS, v).apply();
        } catch (Throwable ignored) {
        }
        cleanupLogsNow(context);
    }

    static void cleanupLogsNow(@NonNull Context context) {
        File internalLogs = new File(context.getFilesDir(), "logs");
        File externalLogs = resolveExternalLogsDir(context);
        List<File> dirs = new ArrayList<>();
        dirs.add(internalLogs);
        if (externalLogs != null) dirs.add(externalLogs);
        for (File dir : dirs) {
            cleanupOldLogs(dir, context);
        }
    }

    private static void cleanupOldLogs(File dir, @NonNull Context context) {
        if (dir == null || !dir.exists()) return;
        File[] files = dir.listFiles((d, name) -> name != null && name.endsWith(".log"));
        if (files == null || files.length == 0) return;

        java.util.Arrays.sort(files, (f1, f2) -> Long.compare(f2.lastModified(), f1.lastModified()));

        long now = System.currentTimeMillis();
        long maxAge = (long) getLogRetentionDays(context) * 24L * 3600L * 1000L;

        // Keep max files
        for (int i = 0; i < files.length; i++) {
            boolean delete = false;
            if (i >= LOG_MAX_FILES) {
                delete = true;
            } else if (now - files[i].lastModified() > maxAge) {
                delete = true;
            }

            if (delete) {
                files[i].delete();
                // Also delete rolled files (.1, .2 etc)
                String base = files[i].getName();
                for (int j = 1; j <= 9; j++) {
                    File rolled = new File(dir, base + "." + j);
                    if (rolled.exists()) rolled.delete();
                }
            }
        }
    }

    static void setMinLevel(@NonNull Level level) {
        minLevel = level;
    }

    static void setAllowedModules(@Nullable List<String> modules) {
        if (modules == null || modules.isEmpty()) {
            allowedModules = null;
            return;
        }
        allowedModules = new HashSet<>(modules);
    }

    static void enter(@NonNull String module, @NonNull String function) {
        log(Level.D, module, function, "ENTER", null);
    }

    static void v(@NonNull String module, @NonNull String function, @NonNull String msg) {
        log(Level.V, module, function, msg, null);
    }

    static void d(@NonNull String module, @NonNull String function, @NonNull String msg) {
        log(Level.D, module, function, msg, null);
    }

    static void i(@NonNull String module, @NonNull String function, @NonNull String msg) {
        log(Level.I, module, function, msg, null);
    }

    static void w(@NonNull String module, @NonNull String function, @NonNull String msg) {
        log(Level.W, module, function, msg, null);
    }

    static void w(@NonNull String module, @NonNull String function, @NonNull String msg, @Nullable Throwable t) {
        log(Level.W, module, function, msg, t);
    }

    static void e(@NonNull String module, @NonNull String function, @NonNull String msg) {
        log(Level.E, module, function, msg, null);
    }

    static void e(@NonNull String module, @NonNull String function, @NonNull String msg, @Nullable Throwable t) {
        log(Level.E, module, function, msg, t);
    }

    static void f(@NonNull String module, @NonNull String function, @NonNull String msg, @Nullable Throwable t) {
        log(Level.F, module, function, msg, t);
    }

    static void log(@NonNull Level level,
                    @NonNull String module,
                    @NonNull String function,
                    @NonNull String msg,
                    @Nullable Throwable t) {
        // [RK3288] Log Strategy:
        // DEBUG/VERBOSE: Full details allowed (no masking).
        // INFO/WARN/ERROR: Keep concise. Masking is applied for non-DEBUG builds or if configured.
        // See docs/RK3288_CONSTRAINTS.md and README.md for disclaimer.

        if (level.ordinal() < minLevel.ordinal()) return;
        
        // Filter by module if configured
        if (allowedModules != null && !allowedModules.contains(module)) return;

        int tid = android.os.Process.myTid();
        // Use ThreadLocal formatter
        String ts = TS.get().format(new Date());

        StringBuilder sb = new StringBuilder();
        sb.append(ts).append(" ");
        sb.append(level.name()).append("/");
        sb.append(module).append("(").append(tid).append("): ");
        sb.append("[").append(function).append("] ");
        sb.append(msg);

        if (t != null) {
            sb.append('\n').append(android.util.Log.getStackTraceString(t));
        }

        String logLine = sb.toString();

        // 1. Output to Android Logcat
        android.util.Log.println(level.androidLevel, module, logLine);

        // 2. Write to Disk (FileLogSink)
        FileLogSink s = sink;
        if (s != null) {
            // Apply masking for disk logs unless it's DEBUG/VERBOSE level
            // This aligns with "Allow full details in DEBUG/VERBOSE" policy
            if (level.ordinal() >= Level.I.ordinal()) {
                String maskedLine = SensitiveDataUtil.maskSensitiveData(logLine);
                s.writeLine(maskedLine);
            } else {
                s.writeLine(logLine);
            }
        }
    }

    @Nullable
    private static File resolveExternalLogsDir(@NonNull Context context) {
        try {
            File extFiles = context.getExternalFilesDir(null);
            if (extFiles == null) return null;
            File pkgDir = extFiles.getParentFile();
            if (pkgDir != null) {
                File logs = new File(pkgDir, "logs");
                if (logs.exists() || logs.mkdirs()) return logs;
            }
            File fallback = new File(extFiles, "logs");
            if (fallback.exists() || fallback.mkdirs()) return fallback;
            return null;
        } catch (Throwable ignored) {
            return null;
        }
    }
}

