package com.arthenica.ffmpegkit;

public final class FFmpegSession {
    public Integer result;
    public int cancels;
    public ReturnCode getReturnCode() { return result == null ? null : new ReturnCode(result); }
    public void cancel() { cancels++; }
}
