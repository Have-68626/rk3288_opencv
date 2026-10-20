package com.arthenica.ffmpegkit;

public final class FFmpegKit {
    public static String[] arguments;
    public static FFmpegSession session;
    public static FFmpegSessionCompleteCallback callback;
    public static boolean fail;
    public static boolean inline;

    public static FFmpegSession executeWithArgumentsAsync(String[] args,
                                                         FFmpegSessionCompleteCallback complete) {
        if (fail) throw new IllegalStateException("合成边界异常");
        arguments = args;
        session = new FFmpegSession();
        callback = complete;
        if (inline) {
            session.result = 0;
            complete.apply(session);
        }
        return session;
    }
}
