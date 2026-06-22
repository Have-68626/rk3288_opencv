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
            String yoloModelPath,
            String arcModelPath,
            String galleryDir,
            int topK,
            float threshold,
            boolean fakeDetect,
            boolean fakeEmbedding
    );

    static native void nativeSetInferenceThrottle(String mode, int intervalMs);
    static native void nativeSetDetectionThrottle(String mode, int intervalMs);
    static native void nativeSetRecognitionThrottle(String mode, int intervalMs);
}

