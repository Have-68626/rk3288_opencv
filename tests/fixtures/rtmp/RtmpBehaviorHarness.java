package com.example.rk3288_opencv;

import com.arthenica.ffmpegkit.FFmpegKit;
import com.arthenica.ffmpegkit.FFmpegSession;
import com.arthenica.ffmpegkit.FFmpegSessionCompleteCallback;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class RtmpBehaviorHarness {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        FfmpegRtmpPusher pusher = new FfmpegRtmpPusher();
        List<String> events = new ArrayList<>();
        FfmpegRtmpPusher.Callback callback = (success, code) -> events.add(success + ":" + code);
        String input = "合成 source ' ; file.mp4";
        String destination = "rtmp://synthetic-user:synthetic-value@localhost/live/synthetic-key?token=synthetic";
        pusher.start(input, destination, callback);
        check(events.isEmpty(), "异步返回不得提前报告完成");
        check(Arrays.asList(FFmpegKit.arguments).contains(input), "输入参数必须保持完整");
        check(destination.equals(FFmpegKit.arguments[FFmpegKit.arguments.length - 1]), "目标参数必须保持完整");
        check(Arrays.asList(FFmpegKit.arguments).contains("quiet"), "底层日志必须静默，避免媒体凭据泄漏");
        FFmpegKit.session.result = 7;
        FFmpegKit.callback.apply(FFmpegKit.session);
        check(events.equals(Arrays.asList("false:7")), "真实失败必须报告实际返回码");
        FFmpegKit.callback.apply(FFmpegKit.session);
        check(events.size() == 1, "完成只能通知一次");
        events.clear();
        pusher.start(input, destination, callback);
        FFmpegKit.session.result = 0;
        FFmpegKit.callback.apply(FFmpegKit.session);
        check(events.equals(Arrays.asList("true:0")), "真实成功必须报告成功");
        events.clear();
        pusher.start(input, destination, callback);
        FFmpegKit.callback.apply(FFmpegKit.session);
        check(events.equals(Arrays.asList("false:EXECUTION_FAILED")), "无返回码不得报告成功");
        events.clear();
        pusher.start(input, destination, callback);
        FFmpegSession oldSession = FFmpegKit.session;
        FFmpegSessionCompleteCallback oldCallback = FFmpegKit.callback;
        pusher.start(input, destination, callback);
        check(oldSession.cancels == 1, "替换会话必须取消旧会话");
        oldSession.result = 0;
        oldCallback.apply(oldSession);
        check(events.isEmpty(), "旧完成不得污染新会话");
        FFmpegSession active = FFmpegKit.session;
        pusher.stop();
        pusher.stop();
        check(active.cancels == 1, "停止必须可重入且只取消当前会话");
        active.result = 255;
        FFmpegKit.callback.apply(active);
        check(events.equals(Arrays.asList("false:CANCELLED")), "取消不是成功完成");
        FFmpegKit.inline = true;
        events.clear();
        pusher.start(input, destination, callback);
        pusher.stop();
        check(events.equals(Arrays.asList("true:0")), "同步完成竞争不得丢失回调");
        check(FFmpegKit.session.cancels == 0, "已完成会话不得复活");
        FFmpegKit.inline = false;
        FFmpegKit.fail = true;
        events.clear();
        pusher.start(input, destination, callback);
        check(events.equals(Arrays.asList("false:START_FAILED")), "边界异常仅返回固定错误码");
        FFmpegKit.fail = false;
        events.clear();
        pusher.start(input, "https://localhost/live", callback);
        check(events.equals(Arrays.asList("false:INVALID_RTMP_URL")), "目标必须限制为 RTMP 或 RTMPS");
        events.clear();
        pusher.start("pipe:0", destination, callback);
        check(events.equals(Arrays.asList("false:INVALID_INPUT")), "没有帧来源的标准输入不得伪装为可推流源");
        events.clear();
        pusher.startCamera("tcp://192.0.2.1:1234", 640, 480, 15, destination, callback);
        check(events.equals(Arrays.asList("false:INVALID_CAMERA_INPUT")), "相机帧输入不得使用外部地址");
        System.out.println("RTMP_BEHAVIOR_PASS");
    }
}
