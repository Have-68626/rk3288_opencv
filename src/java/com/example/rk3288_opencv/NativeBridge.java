package com.example.rk3288_opencv;

final class NativeBridge {
    static {
        System.loadLibrary("native-lib");
    }

    private NativeBridge() {
    }

    static native void nativeConfigureLog(String internalDir, String externalDir, String filename);

    static native String nativeInferFaceFromImage(
            String imagePath,
            int topK
    );

    /** 兼容入口同时更新检测与识别；分设两项的界面调用后须恢复独立检测设置。 */
    static native void nativeSetInferenceThrottle(String mode, int intervalMs);
    static native void nativeSetDetectionThrottle(String mode, int intervalMs);
    static native void nativeSetRecognitionThrottle(String mode, int intervalMs);
}

