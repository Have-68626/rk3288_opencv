package com.example.rk3288_opencv;

import android.app.Application;
import android.os.StrictMode;

public class App extends Application {
    @Override
    public void onCreate() {
        super.onCreate();

        AppLog.init(this);
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

