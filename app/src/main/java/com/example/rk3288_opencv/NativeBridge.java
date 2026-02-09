package com.example.rk3288_opencv;

final class NativeBridge {
    static {
        System.loadLibrary("native-lib");
    }

    private NativeBridge() {
    }

    static native void nativeConfigureLog(String internalDir, String externalDir, String filename);
    static native boolean nativeInitFile(String filePath);
}

