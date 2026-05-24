package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

public class FfmpegRtmpPusherTest {

    @Test
    public void testEscapeFFmpegArgument() {
        assertEquals("''", FfmpegRtmpPusher.escapeFFmpegArgument(""));
        assertEquals("'hello'", FfmpegRtmpPusher.escapeFFmpegArgument("hello"));
        assertEquals("'hello world'", FfmpegRtmpPusher.escapeFFmpegArgument("hello world"));
        assertEquals("'test'\\''quote'", FfmpegRtmpPusher.escapeFFmpegArgument("test'quote"));
        assertEquals("'test\"quote'", FfmpegRtmpPusher.escapeFFmpegArgument("test\"quote"));
        assertEquals("'dummy; rm dummy /; #'", FfmpegRtmpPusher.escapeFFmpegArgument("dummy; rm dummy /; #"));
        assertEquals("'multi'\\''ple '\\'' quotes'", FfmpegRtmpPusher.escapeFFmpegArgument("multi'ple ' quotes"));
    }
}
