package com.example.rk3288_opencv;

import androidx.annotation.Nullable;

final class StatsSnapshot {
    @Nullable final Double fps;
    @Nullable final Double cpuPercent;
    @Nullable final Double memPssMb;
    @Nullable final Double memPrivateDirtyMb;
    @Nullable final Double memGraphicsMb;

    @Nullable final String fpsError;
    @Nullable final String cpuError;
    @Nullable final String memError;

    StatsSnapshot(@Nullable Double fps,
                  @Nullable Double cpuPercent,
                  @Nullable Double memPssMb,
                  @Nullable Double memPrivateDirtyMb,
                  @Nullable Double memGraphicsMb,
                  @Nullable String fpsError,
                  @Nullable String cpuError,
                  @Nullable String memError) {
        this.fps = fps;
        this.cpuPercent = cpuPercent;
        this.memPssMb = memPssMb;
        this.memPrivateDirtyMb = memPrivateDirtyMb;
        this.memGraphicsMb = memGraphicsMb;
        this.fpsError = fpsError;
        this.cpuError = cpuError;
        this.memError = memError;
    }

    static StatsSnapshot empty() {
        return new StatsSnapshot(null, null, null, null, null, "未采样", "未采样", "未采样");
    }
}

