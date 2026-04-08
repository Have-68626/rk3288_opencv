package com.example.rk3288_opencv;

interface CaptureController {
    boolean start(String cameraId);
    void stop();
    String name();
}
