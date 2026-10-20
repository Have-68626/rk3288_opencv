package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.lang.reflect.Proxy;
import java.net.URI;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class FfmpegRtmpPusher {
    private Run active;

    private static final class Run {
        final Callback callback;
        Object session;
        boolean finished;
        boolean cancelRequested;

        Run(Callback callback) { this.callback = callback; }
    }

    void start(@NonNull String input, @NonNull String rtmpUrl, @Nullable Callback callback) {
        startInternal(input, rtmpUrl, 0, 0, 0, callback);
    }

    void startCamera(@NonNull String input, int width, int height, int fps,
                     @NonNull String rtmpUrl, @Nullable Callback callback) {
        startInternal(input, rtmpUrl, width, height, fps, callback);
    }

    private synchronized void startInternal(String input, String rtmpUrl,
                                            int width, int height, int fps, Callback callback) {
        stop();
        Run run = new Run(callback);
        active = run;
        try {
            URI target = new URI(rtmpUrl);
            if (!("rtmp".equalsIgnoreCase(target.getScheme())
                    || "rtmps".equalsIgnoreCase(target.getScheme())) || target.getHost() == null) {
                finish(run, false, "INVALID_RTMP_URL");
                return;
            }
            List<String> arguments = new ArrayList<>(Arrays.asList("-hide_banner", "-loglevel", "quiet"));
            if (width > 0 && height > 0 && fps > 0) {
                // 回环输入由应用独占，明确像素布局和尺寸，避免旧 pipe:0 空读及 NV21 误解码。
                URI camera = new URI(input);
                if (!"tcp".equals(camera.getScheme()) || !"127.0.0.1".equals(camera.getHost())) {
                    finish(run, false, "INVALID_CAMERA_INPUT");
                    return;
                }
                arguments.addAll(Arrays.asList("-f", "rawvideo", "-pixel_format", "rgba",
                        "-video_size", width + "x" + height, "-framerate", Integer.toString(fps),
                        "-i", input, "-an", "-c:v", "libx264", "-preset", "ultrafast",
                        "-tune", "zerolatency", "-pix_fmt", "yuv420p"));
            } else {
                if (input == null || input.isEmpty() || input.startsWith("pipe:")) {
                    finish(run, false, "INVALID_INPUT");
                    return;
                }
                arguments.addAll(Arrays.asList("-stream_loop", "-1", "-re", "-i", input, "-c", "copy"));
            }
            arguments.addAll(Arrays.asList("-f", "flv", rtmpUrl));
            Class<?> kit = Class.forName("com.arthenica.ffmpegkit.FFmpegKit");
            Class<?> complete = Class.forName("com.arthenica.ffmpegkit.FFmpegSessionCompleteCallback");
            Object listener = Proxy.newProxyInstance(complete.getClassLoader(), new Class<?>[]{complete},
                    (proxy, method, values) -> {
                        if ("apply".equals(method.getName())) {
                            complete(run, values[0]);
                            return null;
                        }
                        if ("hashCode".equals(method.getName())) return System.identityHashCode(proxy);
                        if ("equals".equals(method.getName())) return proxy == values[0];
                        return "RtmpSessionCallback";
                    });
            // 官方数组接口保留参数边界；仅实际会话回调可报告完成，返回 session 不代表推流成功。
            Object session = kit.getMethod("executeWithArgumentsAsync", String[].class, complete)
                    .invoke(null, arguments.toArray(new String[0]), listener);
            if (!run.finished && active == run) {
                run.session = session;
                if (run.cancelRequested) cancel(session);
            }
        } catch (java.net.URISyntaxException e) {
            finish(run, false, "INVALID_RTMP_URL");
        } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
            // 底层异常可能携带带鉴权的输入地址，只向 UI 返回固定码。
            finish(run, false, "START_FAILED");
        }
    }

    private void complete(Run run, Object session) {
        try {
            Object result = session.getClass().getMethod("getReturnCode").invoke(session);
            if (result == null) {
                finish(run, false, "EXECUTION_FAILED");
                return;
            }
            int code = ((Number) result.getClass().getMethod("getValue").invoke(result)).intValue();
            finish(run, code == 0, code == 255 ? "CANCELLED" : Integer.toString(code));
        } catch (ReflectiveOperationException | RuntimeException e) {
            finish(run, false, "EXECUTION_FAILED");
        }
    }

    private void finish(Run run, boolean success, String code) {
        synchronized (this) {
            if (active != run || run.finished) return;
            run.finished = true;
            run.session = null;
            active = null;
        }
        if (run.callback != null) run.callback.onCompleted(success, code);
    }

    synchronized void stop() {
        if (active == null || active.finished || active.cancelRequested) return;
        active.cancelRequested = true;
        if (active.session != null) cancel(active.session);
    }

    private static void cancel(Object session) {
        try {
            session.getClass().getMethod("cancel").invoke(session);
        } catch (ReflectiveOperationException | RuntimeException ignored) {
        }
    }

    static String escapeFFmpegArgument(@NonNull String arg) {
        return arg == null ? "''" : "'" + arg.replace("'", "'\\''") + "'";
    }

    interface Callback {
        void onCompleted(boolean success, @NonNull String code);
    }
}
