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

    private static final ThreadLocal<SimpleDateFormat> TS = ThreadLocal.withInitial(
            () -> new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.CHINA)
    );

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
            cleanupOldLogs(dir);
        }

        String internalPath = internalLogs.getAbsolutePath();
        String externalPath = externalLogs == null ? "" : externalLogs.getAbsolutePath();
        try {
            NativeBridge.nativeConfigureLog(internalPath, externalPath, sessionFileName);
        } catch (Throwable ignored) {
        }
    }

    private static void cleanupOldLogs(File dir) {
        if (dir == null || !dir.exists()) return;
        File[] files = dir.listFiles((d, name) -> name.startsWith("rk3288_") && name.endsWith(".log"));
        if (files == null || files.length == 0) return;

        // Sort by name (timestamp) desc
        java.util.Arrays.sort(files, (f1, f2) -> f2.getName().compareTo(f1.getName()));

        long now = System.currentTimeMillis();
        long maxAge = 7L * 24L * 3600L * 1000L; // 7 days

        // Keep max 20 files
        for (int i = 0; i < files.length; i++) {
            boolean delete = false;
            if (i >= 20) {
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
        if (level.ordinal() < minLevel.ordinal()) return;
        Set<String> allow = allowedModules;
        if (allow != null && !allow.contains(module)) return;

        int tid = Process.myTid();
        String ts = TS.get().format(new Date(System.currentTimeMillis()));
        String header = "【" + module + "-" + function + "-" + tid + "-" + ts + "】 ";
        String line = header + msg;

        Log.println(level.androidLevel, module, line);
        if (t != null) {
            Log.println(level.androidLevel, module, Log.getStackTraceString(t));
        }

        FileLogSink s = sink;
        if (s != null) {
            if (t != null) {
                s.writeLine(line + "\n" + Log.getStackTraceString(t));
            } else {
                s.writeLine(line);
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

