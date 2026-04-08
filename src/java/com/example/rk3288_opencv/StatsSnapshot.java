package com.example.rk3288_opencv;

import androidx.annotation.Nullable;

final class StatsSnapshot {
    @Nullable final Double fps;
    @Nullable final Double captureFps;
    @Nullable final Double latencyMs;
    @Nullable final Double cpuPercent;
    @Nullable final Double memPssMb;
    @Nullable final Double memPrivateDirtyMb;
    @Nullable final Double memGraphicsMb;

    @Nullable final String fpsError;
    @Nullable final String captureError;
    @Nullable final String latencyError;
    @Nullable final String cpuError;
    @Nullable final String memError;

    StatsSnapshot(@Nullable Double fps,
                  @Nullable Double captureFps,
                  @Nullable Double latencyMs,
                  @Nullable Double cpuPercent,
                  @Nullable Double memPssMb,
                  @Nullable Double memPrivateDirtyMb,
                  @Nullable Double memGraphicsMb,
                  @Nullable String fpsError,
                  @Nullable String captureError,
                  @Nullable String latencyError,
                  @Nullable String cpuError,
                  @Nullable String memError) {
        this.fps = fps;
        this.captureFps = captureFps;
        this.latencyMs = latencyMs;
        this.cpuPercent = cpuPercent;
        this.memPssMb = memPssMb;
        this.memPrivateDirtyMb = memPrivateDirtyMb;
        this.memGraphicsMb = memGraphicsMb;
        this.fpsError = fpsError;
        this.captureError = captureError;
        this.latencyError = latencyError;
        this.cpuError = cpuError;
        this.memError = memError;
    }

    static StatsSnapshot empty() {
        return new StatsSnapshot(null, null, null, null, null, null, null, "未采样", "未采样", "未采样", "未采样", "未采样");
    }
}

