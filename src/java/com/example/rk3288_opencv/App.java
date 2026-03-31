package com.example.rk3288_opencv;

import android.app.Application;
import android.os.StrictMode;

public class App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();

        AppLog.init(this);
        DeviceProfile profile = DeviceProfile.collect(this);
        DeviceClass deviceClass = DeviceClassifier.classify(profile);
        DeviceRuntime.get().init(profile, deviceClass);
        AppLog.i("App", "onCreate", "设备识别: " + deviceClass + " " + profile.toShortString());
        // PlayIntegrityChecker.get().warmup(this); // Removed: SDK missing
        StatsRepository.getInstance().start(this);

        if (BuildConfig.DEBUG) {
            StrictMode.setThreadPolicy(new StrictMode.ThreadPolicy.Builder()
                    .detectDiskReads()
                    .detectDiskWrites()
                    .detectNetwork()
                    .penaltyLog()
                    .build());

            StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder()
                    .detectLeakedClosableObjects()
                    .penaltyLog()
                    .build());
        }
    }
}

