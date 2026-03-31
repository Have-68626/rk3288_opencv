package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

final class FfmpegRtmpPusher {
    private volatile Object session;

    void start(@NonNull String input, @NonNull String rtmpUrl, @Nullable Callback callback) {
        stop();
        // Check if input is a file/url or a raw pipe
        boolean isRaw = input.startsWith("pipe:");
        
        StringBuilder cmd = new StringBuilder();
        
        if (!isRaw) {
            // For files/network streams, loop indefinitely and read at native framerate
            cmd.append("-stream_loop -1 ");
            cmd.append("-re ");
            cmd.append("-i ").append(quote(input)).append(" ");
        } else {
            // For raw input (e.g. NV21 from camera via pipe)
            // Assumes 640x480 NV21 @ 30fps
            cmd.append("-f rawvideo -vcodec rawvideo -s 640x480 -r 30 -pix_fmt nv21 ");
            cmd.append("-i ").append(quote(input)).append(" ");
        }

        // Encoding settings for RTMP (FLV container, H.264 video, AAC audio)
        // Note: -c copy is fast for files, but for raw we need -c:v libx264 or similar
        // Since we want to support both, we need logic.
        // For this implementation, we assume Mock Source (File) uses copy if possible, 
        // or re-encodes if necessary. To be safe and loop-able, we might need re-encoding 
        // if the source codecs don't match FLV requirements.
        // But the requirement says "Mock mode... loop playback".
        // Let's stick to -c copy for file inputs to be safe on CPU, 
        // assuming users provide compatible MP4/FLV. 
        // If not, FFmpeg will fail, which is acceptable for "Mock".
        
        if (isRaw) {
             cmd.append("-c:v libx264 -preset ultrafast -tune zerolatency -f flv ");
        } else {
             cmd.append("-c copy -f flv ");
        }
        
        cmd.append(quote(rtmpUrl));

        try {
            Class<?> kit = Class.forName("com.arthenica.ffmpegkit.FFmpegKit");
            session = kit.getMethod("executeAsync", String.class).invoke(null, cmd.toString());
            if (callback != null) {
                callback.onCompleted(true, "STARTED");
            }
        } catch (Throwable e) {
            session = null;
            if (callback != null) {
                callback.onCompleted(false, e.getClass().getSimpleName());
            }
        }
    }

    void stop() {
        Object s = session;
        session = null;
        if (s != null) {
            try {
                s.getClass().getMethod("cancel").invoke(s);
            } catch (Throwable ignored) {
            }
        }
    }

    private static String quote(@NonNull String s) {
        return "\"" + s.replace("\"", "\\\"") + "\"";
    }

    interface Callback {
        void onCompleted(boolean success, @NonNull String code);
    }
}

