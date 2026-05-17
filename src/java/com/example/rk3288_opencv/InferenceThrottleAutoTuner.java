package com.example.rk3288_opencv;

import android.os.SystemClock;

final class InferenceThrottleAutoTuner {
    private static final int DEFAULT_MIN_MS = 80;
    private static final int DEFAULT_MAX_MS = 500;

    private static final int DEFAULT_STEP_UP_MS = 30;
    private static final int DEFAULT_STEP_DOWN_MS = 20;
    private static final long DEFAULT_COOLDOWN_MS = 1500;

    private static final double CPU_HIGH = 78.0;
    private static final double CPU_LOW = 55.0;

    private static final double LAT_HIGH_MS = 130.0;
    private static final double LAT_LOW_MS = 90.0;

    private static final double FPS_LOW = 22.0;
    private static final double FPS_HIGH = 28.0;

    private int minMs = DEFAULT_MIN_MS;
    private int maxMs = DEFAULT_MAX_MS;
    private int stepUpMs = DEFAULT_STEP_UP_MS;
    private int stepDownMs = DEFAULT_STEP_DOWN_MS;
    private long cooldownMs = DEFAULT_COOLDOWN_MS;

    private int intervalMs;
    private long lastDecisionMs;

    InferenceThrottleAutoTuner(int initialIntervalMs) {
        reset(initialIntervalMs);
    }

    void reset(int initialIntervalMs) {
        intervalMs = clamp(initialIntervalMs);
        lastDecisionMs = 0;
    }

    int getIntervalMs() {
        return intervalMs;
    }

    boolean update(StatsSnapshot s) {
        return update(s, SystemClock.elapsedRealtime());
    }

    boolean update(StatsSnapshot s, long nowMs) {
        if (s == null) return false;
        if (lastDecisionMs > 0 && (nowMs - lastDecisionMs) < cooldownMs) return false;

        boolean bad = isBad(s);
        boolean good = isGood(s);
        if (!bad && !good) return false;

        int next = intervalMs;
        if (bad) {
            next = clamp(intervalMs + stepUpMs);
        } else {
            next = clamp(intervalMs - stepDownMs);
        }

        if (next == intervalMs) return false;
        intervalMs = next;
        lastDecisionMs = nowMs;
        return true;
    }

    private boolean isBad(StatsSnapshot s) {
        if (s.cpuPercent != null && s.cpuPercent > CPU_HIGH) return true;
        if (s.latencyMs != null && s.latencyMs > LAT_HIGH_MS) return true;
        if (s.fps != null && s.fps < FPS_LOW) return true;
        return false;
    }

    private boolean isGood(StatsSnapshot s) {
        if (s.cpuPercent == null || s.latencyMs == null || s.fps == null) return false;
        return s.cpuPercent < CPU_LOW && s.latencyMs < LAT_LOW_MS && s.fps > FPS_HIGH;
    }

    private int clamp(int v) {
        if (v < minMs) return minMs;
        if (v > maxMs) return maxMs;
        return v;
    }
}

