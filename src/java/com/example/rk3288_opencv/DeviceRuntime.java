package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

final class DeviceRuntime {
    private static final DeviceRuntime INSTANCE = new DeviceRuntime();

    static DeviceRuntime get() {
        return INSTANCE;
    }

    private DeviceProfile profile;
    private DeviceClass deviceClass = DeviceClass.OTHER;

    private DeviceRuntime() {
    }

    void init(@NonNull DeviceProfile profile, @NonNull DeviceClass deviceClass) {
        this.profile = profile;
        this.deviceClass = deviceClass;
    }

    @NonNull
    DeviceProfile getProfile() {
        return profile;
    }

    @NonNull
    DeviceClass getDeviceClass() {
        return deviceClass;
    }
}

