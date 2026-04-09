package com.example.rk3288_opencv;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.Debug;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.Choreographer;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.io.FileDescriptor;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.locks.ReentrantReadWriteLock;

import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;

final class StatsRepository {
    private static final StatsRepository INSTANCE = new StatsRepository();
    private static final long INTERVAL_MS = 500;

    static StatsRepository getInstance() {
        return INSTANCE;
    }

    private final ReentrantReadWriteLock lock = new ReentrantReadWriteLock();
    private StatsSnapshot snapshot = StatsSnapshot.empty();

    private volatile boolean started;
    private Context appContext;
    private ActivityManager activityManager;

    private HandlerThread samplerThread;
    private Handler samplerHandler;

    private final FpsTracker fpsTracker = new FpsTracker();
    private final CaptureTracker captureTracker = new CaptureTracker();
    private final LatencyTracker latencyTracker = new LatencyTracker();
    private final CpuSampler cpuSampler = new CpuSampler();
    private final MemSampler memSampler = new MemSampler();

    void reportCaptureFrameTimestampNs(long timestampNs) {
        if (timestampNs <= 0) return;
        captureTracker.onFrame(timestampNs);
        latencyTracker.onCapture(timestampNs);
    }

    void reportRenderTimeNs(long renderTimeNs) {
        if (renderTimeNs <= 0) return;
        latencyTracker.onRender(renderTimeNs);
    }

    void start(@NonNull Context context) {
        if (started) return;
        started = true;

        appContext = context.getApplicationContext();
        activityManager = (ActivityManager) appContext.getSystemService(Context.ACTIVITY_SERVICE);

        fpsTracker.start();

        samplerThread = new HandlerThread("StatsSampler");
        samplerThread.start();
        samplerHandler = new Handler(samplerThread.getLooper());
        samplerHandler.post(sampleRunnable);
    }

    void stop() {
        started = false;
        fpsTracker.stop();
        if (samplerThread != null) {
            samplerThread.quitSafely();
            samplerThread = null;
        }
        samplerHandler = null;
    }

    @NonNull
    StatsSnapshot getSnapshot() {
        lock.readLock().lock();
        try {
            return snapshot;
        } finally {
            lock.readLock().unlock();
        }
    }

    private final Runnable sampleRunnable = new Runnable() {
        @Override
        public void run() {
            if (!started) return;
            sampleOnce();
            if (samplerHandler != null) {
                samplerHandler.postDelayed(this, INTERVAL_MS);
            }
        }
    };

    private void sampleOnce() {
        Double fps = null;
        String fpsErr = null;
        try {
            fps = fpsTracker.getFps();
            if (fps == null) fpsErr = "FPS不足样本";
        } catch (Exception e) {
            fpsErr = e.getMessage();
            AppLog.e("StatsRepository", "sampleOnce", "FPS采集失败", e);
        }

        Double capFps = null;
        String capErr = null;
        try {
            capFps = captureTracker.getFps();
            if (capFps == null) capErr = "CAP不足样本";
        } catch (Exception e) {
            capErr = e.getMessage();
            AppLog.e("StatsRepository", "sampleOnce", "CAP采集失败", e);
        }

        Double latMs = null;
        String latErr = null;
        try {
            latMs = latencyTracker.getLatencyMs();
            if (latMs == null) latErr = "LAT不足样本";
        } catch (Exception e) {
            latErr = e.getMessage();
            AppLog.e("StatsRepository", "sampleOnce", "LAT采集失败", e);
        }

        CpuSampler.Result cpuRes = cpuSampler.sample();
        Double cpu = cpuRes.cpuPercent;
        String cpuErr = cpuRes.error;

        MemSampler.Result memRes = memSampler.sample(activityManager);
        Double memPss = memRes.pssMb;
        Double memPriv = memRes.privateDirtyMb;
        Double memGfx = memRes.graphicsMb;
        String memErr = memRes.error;

        StatsSnapshot next = new StatsSnapshot(fps, capFps, latMs, cpu, memPss, memPriv, memGfx, fpsErr, capErr, latErr, cpuErr, memErr);
        lock.writeLock().lock();
        try {
            snapshot = next;
        } finally {
            lock.writeLock().unlock();
        }
    }

    private static final class CaptureTracker {
        private final Object guard = new Object();
        private final ArrayDeque<Long> intervalsMs = new ArrayDeque<>();
        private static final int MAX_SAMPLES = 180;
        private long lastTsNs;

        void onFrame(long tsNs) {
            if (tsNs <= 0) return;
            if (lastTsNs != 0) {
                long deltaMs = (tsNs - lastTsNs) / 1_000_000L;
                synchronized (guard) {
                    intervalsMs.addLast(deltaMs);
                    while (intervalsMs.size() > MAX_SAMPLES) intervalsMs.removeFirst();
                }
            }
            lastTsNs = tsNs;
        }

        @Nullable
        Double getFps() {
            synchronized (guard) {
                if (intervalsMs.size() < 10) return null;
                long sum = 0;
                int n = 0;
                for (long v : intervalsMs) {
                    if (v < 5 || v > 500) continue;
                    sum += v;
                    n++;
                }
                if (n < 5) return null;
                double avg = sum / (double) n;
                return 1000.0 / avg;
            }
        }
    }

    private static final class LatencyTracker {
        private final Object guard = new Object();
        private final ArrayDeque<Long> samplesMs = new ArrayDeque<>();
        private static final int MAX_SAMPLES = 120;
        private long lastCaptureTsNs;

        void onCapture(long tsNs) {
            lastCaptureTsNs = tsNs;
        }

        void onRender(long renderNs) {
            long cap = lastCaptureTsNs;
            if (cap <= 0) return;
            long deltaMs = (renderNs - cap) / 1_000_000L;
            if (deltaMs < 0 || deltaMs > 60_000) return;
            synchronized (guard) {
                samplesMs.addLast(deltaMs);
                while (samplesMs.size() > MAX_SAMPLES) samplesMs.removeFirst();
            }
        }

        @Nullable
        Double getLatencyMs() {
            synchronized (guard) {
                if (samplesMs.size() < 5) return null;
                long sum = 0;
                int n = 0;
                for (long v : samplesMs) {
                    if (v < 0 || v > 60_000) continue;
                    sum += v;
                    n++;
                }
                if (n < 3) return null;
                return sum / (double) n;
            }
        }
    }

    private static final class FpsTracker {
        private final Object guard = new Object();
        private final ArrayDeque<Long> intervalsMs = new ArrayDeque<>();
        private static final int MAX_SAMPLES = 180;
        private volatile boolean running;
        private long lastFrameNs;

        void start() {
            if (running) return;
            running = true;
            Handler main = new Handler(Looper.getMainLooper());
            main.post(() -> Choreographer.getInstance().postFrameCallback(frameCallback));
        }

        void stop() {
            running = false;
        }

        @Nullable
        Double getFps() {
            synchronized (guard) {
                if (intervalsMs.size() < 10) return null;
                long sum = 0;
                int n = 0;
                for (long v : intervalsMs) {
                    if (v < 5 || v > 100) continue;
                    sum += v;
                    n++;
                }
                if (n < 5) return null;
                double avg = sum / (double) n;
                return 1000.0 / avg;
            }
        }

        private final Choreographer.FrameCallback frameCallback = new Choreographer.FrameCallback() {
            @Override
            public void doFrame(long frameTimeNanos) {
                if (!running) return;
                if (lastFrameNs != 0) {
                    long deltaMs = (frameTimeNanos - lastFrameNs) / 1_000_000L;
                    synchronized (guard) {
                        intervalsMs.addLast(deltaMs);
                        while (intervalsMs.size() > MAX_SAMPLES) intervalsMs.removeFirst();
                    }
                }
                lastFrameNs = frameTimeNanos;
                Choreographer.getInstance().postFrameCallback(this);
            }
        };
    }

    private static final class CpuSampler {
        private long lastTotalNoIrq = -1;
        private long lastProc = -1;
        private boolean useProcFs = true;
        private long lastAppCpuTimeMs = -1;
        private long lastAppWallTimeMs = -1;

        static final class Result {
            @Nullable final Double cpuPercent;
            @Nullable final String error;

            Result(@Nullable Double cpuPercent, @Nullable String error) {
                this.cpuPercent = cpuPercent;
                this.error = error;
            }
        }

        Result sample() {
            if (useProcFs) {
                try {
                    String stat = Procfs.readFirstLine("/proc/stat");
                    String self = Procfs.readAll("/proc/self/stat");
                    long totalNoIrq = parseTotalNoIrq(stat);
                    long proc = parseProcCpu(self);

                    if (lastTotalNoIrq < 0 || lastProc < 0) {
                        lastTotalNoIrq = totalNoIrq;
                        lastProc = proc;
                        return new Result(null, "CPU首次采样");
                    }

                    long dTotal = totalNoIrq - lastTotalNoIrq;
                    long dProc = proc - lastProc;
                    lastTotalNoIrq = totalNoIrq;
                    lastProc = proc;

                    if (dTotal <= 0 || dProc < 0) {
                        return new Result(null, "CPU delta 异常");
                    }
                    double pct = (dProc / (double) dTotal) * 100.0;
                    if (pct < 0) pct = 0;
                    return new Result(pct, null);
                } catch (ErrnoException e) {
                    if (e.errno == OsConstants.EACCES) {
                        // Suppress warning after first failure
                        if (useProcFs) {
                             AppLog.w("StatsRepository", "CpuSampler.sample", "读取 /proc/stat 权限被拒绝，切换至标准 API (Future warnings suppressed)");
                             useProcFs = false;
                        }
                        return sampleFallback();
                    }
                    String err = "errno=" + e.errno + " " + Os.strerror(e.errno);
                    AppLog.e("StatsRepository", "CpuSampler.sample", err, e);
                    return new Result(null, err);
                } catch (Exception e) {
                    AppLog.e("StatsRepository", "CpuSampler.sample", "CPU采集失败", e);
                    return new Result(null, e.getMessage());
                }
            }
            return sampleFallback();
        }

        private Result sampleFallback() {
            try {
                long appCpuMs = android.os.Process.getElapsedCpuTime();
                long appWallMs = android.os.SystemClock.elapsedRealtime();

                if (lastAppCpuTimeMs < 0 || lastAppWallTimeMs < 0) {
                    lastAppCpuTimeMs = appCpuMs;
                    lastAppWallTimeMs = appWallMs;
                    return new Result(null, "CPU首次采样(Fallback)");
                }

                long dCpu = appCpuMs - lastAppCpuTimeMs;
                long dWall = appWallMs - lastAppWallTimeMs;

                lastAppCpuTimeMs = appCpuMs;
                lastAppWallTimeMs = appWallMs;

                if (dWall <= 0) return new Result(0.0, null);

                // 计算当前应用的 CPU 使用率
                double pct = (dCpu / (double) dWall) * 100.0;
                // 限制最大值为 100% * 核心数，但在 UI 上我们可能只显示百分比
                if (pct < 0) pct = 0;
                
                return new Result(pct, null);
            } catch (Exception e) {
                return new Result(null, "Fallback failed: " + e.getMessage());
            }
        }

        private static long parseTotalNoIrq(@NonNull String line) {
            String[] toks = line.trim().split("\\s+");
            if (toks.length < 8) throw new IllegalArgumentException("stat字段不足");
            long user = parseLong(toks[1]);
            long nice = parseLong(toks[2]);
            long system = parseLong(toks[3]);
            long idle = parseLong(toks[4]);
            long iowait = toks.length > 5 ? parseLong(toks[5]) : 0;
            long steal = toks.length > 8 ? parseLong(toks[8]) : 0;
            long activeNoIrq = user + nice + system + steal;
            return activeNoIrq + idle + iowait;
        }

        private static long parseProcCpu(@NonNull String stat) {
            int r = stat.lastIndexOf(')');
            if (r < 0) throw new IllegalArgumentException("self/stat格式异常");
            String after = stat.substring(r + 1).trim();
            String[] toks = after.split("\\s+");
            if (toks.length < 15) throw new IllegalArgumentException("self/stat字段不足");
            long utime = parseLong(toks[11]);
            long stime = parseLong(toks[12]);
            return utime + stime;
        }

        private static long parseLong(@NonNull String s) {
            return Long.parseLong(s);
        }
    }

    private static final class MemSampler {
        static final class Result {
            @Nullable final Double pssMb;
            @Nullable final Double privateDirtyMb;
            @Nullable final Double graphicsMb;
            @Nullable final String error;

            Result(@Nullable Double pssMb, @Nullable Double privateDirtyMb, @Nullable Double graphicsMb, @Nullable String error) {
                this.pssMb = pssMb;
                this.privateDirtyMb = privateDirtyMb;
                this.graphicsMb = graphicsMb;
                this.error = error;
            }
        }

        Result sample(@Nullable ActivityManager am) {
            try {
                Debug.MemoryInfo mi = new Debug.MemoryInfo();
                Debug.getMemoryInfo(mi);
                double pss = mi.getTotalPss() / 1024.0;
                double priv = mi.getTotalPrivateDirty() / 1024.0;
                Double gfx = null;
                try {
                    if (Build.VERSION.SDK_INT >= 23) {
                        String s = mi.getMemoryStat("summary.graphics");
                        if (s != null) {
                            gfx = Long.parseLong(s) / 1024.0;
                        }
                    }
                } catch (Exception ignored) {
                }
                if (am != null) {
                    ActivityManager.MemoryInfo sys = new ActivityManager.MemoryInfo();
                    am.getMemoryInfo(sys);
                }
                return new Result(pss, priv, gfx, null);
            } catch (Exception e) {
                AppLog.e("StatsRepository", "MemSampler.sample", "MEM采集失败", e);
                return new Result(null, null, null, e.getMessage());
            }
        }
    }

    private static final class Procfs {
        static String readFirstLine(@NonNull String path) throws ErrnoException {
            String all = readAll(path);
            int i = all.indexOf('\n');
            return i >= 0 ? all.substring(0, i) : all;
        }

        static String readAll(@NonNull String path) throws ErrnoException {
            FileDescriptor fd = null;
            try {
                fd = Os.open(path, OsConstants.O_RDONLY, 0);
                byte[] buf = new byte[4096];
                int n = Os.read(fd, buf, 0, buf.length);
                if (n <= 0) return "";
                return new String(buf, 0, n, StandardCharsets.US_ASCII);
            } catch (ErrnoException e) {
                throw e;
            } catch (Exception e) {
                throw new ErrnoException("readAll", OsConstants.EIO);
            } finally {
                if (fd != null) {
                    try {
                        Os.close(fd);
                    } catch (Exception ignored) {
                    }
                }
            }
        }
    }
}

