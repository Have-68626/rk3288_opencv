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
        
        java.util.List<String> cmdArgs = new java.util.ArrayList<>();
        
        if (!isRaw) {
            // For files/network streams, loop indefinitely and read at native framerate
            cmdArgs.add("-stream_loop");
            cmdArgs.add("-1");
            cmdArgs.add("-re");
            cmdArgs.add("-i");
            cmdArgs.add(input);
        } else {
            // For raw input (e.g. NV21 from camera via pipe)
            // Assumes 640x480 NV21 @ 30fps
            cmdArgs.add("-f");
            cmdArgs.add("rawvideo");
            cmdArgs.add("-vcodec");
            cmdArgs.add("rawvideo");
            cmdArgs.add("-s");
            cmdArgs.add("640x480");
            cmdArgs.add("-r");
            cmdArgs.add("30");
            cmdArgs.add("-pix_fmt");
            cmdArgs.add("nv21");
            cmdArgs.add("-i");
            cmdArgs.add(input);
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
             cmdArgs.add("-c:v");
             cmdArgs.add("libx264");
             cmdArgs.add("-preset");
             cmdArgs.add("ultrafast");
             cmdArgs.add("-tune");
             cmdArgs.add("zerolatency");
             cmdArgs.add("-f");
             cmdArgs.add("flv");
        } else {
             cmdArgs.add("-c");
             cmdArgs.add("copy");
             cmdArgs.add("-f");
             cmdArgs.add("flv");
        }
        
        cmdArgs.add(rtmpUrl);

        // Shell 拼接代码已删除（—— 审计 P2.16）
        // 使用 FFmpeg 参数列表方式传递，避免命令注入
        // 参数已经由 start() 方法的参数列表直接传入 executeAsync(String[]...)
        // (原 StringBuilder 循环构建 cmd 为死代码，从未使用)

        try {
            Class<?> kit = Class.forName("com.arthenica.ffmpegkit.FFmpegKit");
            session = kit.getMethod("executeAsync", String[].class).invoke(null, (Object) cmdArgs.toArray(new String[0]));
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

    // visible for testing
    static String escapeFFmpegArgument(@NonNull String arg) {
        if (arg == null) {
            return "''";
        }
        return "'" + arg.replace("'", "'\\''") + "'";
    }

    interface Callback {
        void onCompleted(boolean success, @NonNull String code);
    }
}
