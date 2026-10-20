package com.example.rk3288_opencv;

public final class RtmpMissingKitHarness {
    public static void main(String[] args) {
        final int[] calls = {0};
        new FfmpegRtmpPusher().start("synthetic.mp4", "rtmp://localhost/live/synthetic", (success, code) -> {
            calls[0]++;
            if (success || !"START_FAILED".equals(code)) {
                throw new AssertionError("缺少可选 AAR 时必须明确报告启动失败");
            }
        });
        if (calls[0] != 1) throw new AssertionError("缺少可选 AAR 时必须仅报告一次失败");
        System.out.println("RTMP_MISSING_KIT_PASS");
    }
}
