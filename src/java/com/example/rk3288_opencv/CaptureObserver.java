package com.example.rk3288_opencv;

interface CaptureObserver {
    void onFramePushed(boolean ok, long timestampNs);
    void onError(String stage, String message);
}
