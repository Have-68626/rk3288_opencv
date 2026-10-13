package com.example.rk3288_opencv;

interface CaptureController {
    boolean start(String cameraId);
    void stop();
    String name();

    /** 当前 source epoch（CaptureSourceEpochGate.current），供状态区诊断展示；无实现返回 0。 */
    default long currentSourceEpoch() {
        return 0L;
    }
}
