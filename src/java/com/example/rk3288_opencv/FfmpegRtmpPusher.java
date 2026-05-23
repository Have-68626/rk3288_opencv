package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.util.ArrayList;
import java.util.List;

final class FfmpegRtmpPusher {
    private volatile Object session;

    void start(@NonNull String input, @NonNull String rtmpUrl, @Nullable Callback callback) {
        stop();
        // Check if input is a file/url or a raw pipe
        boolean isRaw = input.startsWith("pipe:");
        
        List<String> args = new ArrayList<>();
        
        if (!isRaw) {
            // For files/network streams, loop indefinitely and read at native framerate
            args.add("-stream_loop");
            args.add("-1");
            args.add("-re");
            args.add("-i");
            args.add(input);
        } else {
            // For raw input (e.g. NV21 from camera via pipe)
            // Assumes 640x480 NV21 @ 30fps
            args.add("-f");
            args.add("rawvideo");
            args.add("-vcodec");
            args.add("rawvideo");
            args.add("-s");
            args.add("640x480");
            args.add("-r");
            args.add("30");
            args.add("-pix_fmt");
            args.add("nv21");
            args.add("-i");
            args.add(input);
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
            args.add("-c:v");
            args.add("libx264");
            args.add("-preset");
            args.add("ultrafast");
            args.add("-tune");
            args.add("zerolatency");
            args.add("-f");
            args.add("flv");
        } else {
            args.add("-c");
            args.add("copy");
            args.add("-f");
            args.add("flv");
        }
        
        args.add(rtmpUrl);

        StringBuilder cmd = new StringBuilder();
        for (int i = 0; i < args.size(); i++) {
            if (i > 0) cmd.append(" ");
            cmd.append(quote(args.get(i)));
        }

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
        return "'" + s.replace("'", "'\\''") + "'";
    }

    interface Callback {
        void onCompleted(boolean success, @NonNull String code);
    }
}

