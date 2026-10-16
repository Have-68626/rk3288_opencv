package com.example.rk3288_opencv;

import java.lang.reflect.Constructor;
import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class RtmpCameraBehaviorHarness {
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static Object create(AtomicInteger reads, AtomicInteger failures,
                                 AtomicInteger closes, CountDownLatch released, int mode) throws Exception {
        Class<?> sourceType = Class.forName("com.example.rk3288_opencv.RtmpCameraInput$FrameSource");
        Object source = Proxy.newProxyInstance(sourceType.getClassLoader(), new Class<?>[]{sourceType},
                (proxy, method, values) -> {
                    if (method.getName().equals("readFrame")) {
                        reads.incrementAndGet();
                        if (mode == 1) throw new IllegalStateException("合成采集失败");
                        if (mode == 3 || (mode == 2 && reads.get() > 1)) return false;
                        Arrays.fill((byte[]) values[0], (byte) 42);
                        return true;
                    }
                    if (method.getName().equals("close")) {
                        closes.incrementAndGet();
                        released.countDown();
                    }
                    return null;
                });
        Class<?> inputType = Class.forName("com.example.rk3288_opencv.RtmpCameraInput");
        Constructor<?> constructor = inputType.getDeclaredConstructor(sourceType, int.class,
                int.class, int.class, Runnable.class);
        return constructor.newInstance(source, 2, 2, 15, (Runnable) failures::incrementAndGet);
    }

    private static String url(Object source) throws Exception {
        return (String) source.getClass().getDeclaredMethod("inputUrl").invoke(source);
    }

    private static void close(Object source) throws Exception {
        source.getClass().getDeclaredMethod("close").invoke(source);
    }

    public static void main(String[] args) throws Exception {
        AtomicInteger reads = new AtomicInteger();
        AtomicInteger failures = new AtomicInteger();
        AtomicInteger closes = new AtomicInteger();
        CountDownLatch released = new CountDownLatch(1);
        Object source;
        try {
            source = create(reads, failures, closes, released, 0);
        } catch (ClassNotFoundException e) {
            throw new AssertionError("相机推流缺少真实帧输入器");
        }
        URI address = URI.create(url(source));
        check("127.0.0.1".equals(address.getHost()) && address.getPort() > 0,
                "相机帧必须仅通过回环随机端口传输");
        try (Socket consumer = new Socket(address.getHost(), address.getPort())) {
            consumer.setSoTimeout(2000);
            byte[] data = consumer.getInputStream().readNBytes(32);
            check(data.length == 32 && data[0] == 42 && data[31] == 42,
                    "生产输入器必须持续发送真实 RGBA 帧");
            check(reads.get() >= 2, "不得只发送占位帧");
            close(source);
            close(source);
            check(released.await(2, TimeUnit.SECONDS), "停止必须释放采集源");
            check(consumer.getInputStream().read() == -1, "停止必须关闭帧连接");
        }
        check(closes.get() == 1 && failures.get() == 0, "可重入停止不得误报失败或重复释放");
        reads.set(0);
        closes.set(0);
        CountDownLatch neverConnected = new CountDownLatch(1);
        source = create(reads, failures, closes, neverConnected, 0);
        close(source);
        check(neverConnected.await(2, TimeUnit.SECONDS), "尚未连接时也必须取消 accept 并释放");
        check(closes.get() == 1 && failures.get() == 0, "尚未连接的主动停止不是采集失败");
        CountDownLatch failedRelease = new CountDownLatch(1);
        source = create(reads, failures, closes, failedRelease, 1);
        address = URI.create(url(source));
        try (Socket consumer = new Socket(address.getHost(), address.getPort())) {
            consumer.setSoTimeout(2000);
            check(consumer.getInputStream().read() == -1, "采集失败必须关闭帧连接");
        }
        check(failedRelease.await(2, TimeUnit.SECONDS), "采集失败必须释放源");
        check(failures.get() == 1, "采集失败只能报告一次");
        reads.set(0);
        failures.set(0);
        closes.set(0);
        CountDownLatch slowRelease = new CountDownLatch(1);
        source = create(reads, failures, closes, slowRelease, 2);
        address = URI.create(url(source));
        try (Socket consumer = new Socket(address.getHost(), address.getPort())) {
            consumer.setSoTimeout(2000);
            check(consumer.getInputStream().readNBytes(32).length == 32,
                    "低帧率相机须维持视频时基，持续发送最近一帧");
            close(source);
        }
        check(slowRelease.await(2, TimeUnit.SECONDS), "低帧率采集也须正常释放");
        check(failures.get() == 0, "低帧率不得误报失败");
        CountDownLatch emptyRelease = new CountDownLatch(1);
        source = create(reads, failures, closes, emptyRelease, 3);
        address = URI.create(url(source));
        try (Socket consumer = new Socket(address.getHost(), address.getPort())) {
            consumer.setSoTimeout(7000);
            check(consumer.getInputStream().read() == -1, "无帧超时须关闭连接");
        }
        check(emptyRelease.await(2, TimeUnit.SECONDS), "无帧超时须释放采集源");
        check(failures.get() == 1, "持续无帧必须报告失败");
        System.out.println("RTMP_CAMERA_BEHAVIOR_PASS");
    }
}
