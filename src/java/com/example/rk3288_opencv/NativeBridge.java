package com.example.rk3288_opencv;

final class NativeBridge {
    static {
        System.loadLibrary("native-lib");
    }

    private NativeBridge() {
    }

    static native void nativeConfigureLog(String internalDir, String externalDir, String filename);
    static native boolean nativeInitFile(String filePath);

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
}

