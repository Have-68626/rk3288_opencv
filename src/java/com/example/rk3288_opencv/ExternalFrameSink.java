package com.example.rk3288_opencv;

import java.nio.ByteBuffer;

interface ExternalFrameSink {
    boolean onYuv420888Frame(ByteBuffer yBuffer,
                             int yRowStride,
                             ByteBuffer uBuffer,
                             int uRowStride,
                             int uPixelStride,
                             ByteBuffer vBuffer,
                             int vRowStride,
                             int vPixelStride,
                             int width,
                             int height,
                             long timestampNs,
                             int rotationDegrees,
                             boolean mirrored);
}
