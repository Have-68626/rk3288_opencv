package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Arrays;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * S-SEC-B09-006：SAF/Mock 导入有界拷贝辅助（CameraFragment.BoundedStreamCopy）的纯 JVM 负面测试。
 * 覆盖：超长输入流在字节上限处受控中断、涓流流在时长上限处受控中断、取消即停、写盘不超过上限。
 */
public class BoundedStreamCopyTest {

    private static final long NO_LIMIT = Long.MAX_VALUE;

    /** 可编程时钟：依次返回预设值，越界后重复最后一个（用于模拟耗时推进，不真实 sleep）。 */
    private static final class SeqClock implements CameraFragment.BoundedStreamCopy.Clock {
        private final long[] values;
        private int i;

        SeqClock(long... values) {
            this.values = values;
        }

        @Override
        public long elapsedMs() {
            if (i >= values.length) return values[values.length - 1];
            return values[i++];
        }
    }

    private static final class Flag implements CameraFragment.BoundedStreamCopy.Cancellation {
        boolean value;

        @Override
        public boolean isCancelled() {
            return value;
        }
    }

    private static byte[] bytesOf(int n) {
        byte[] b = new byte[n];
        for (int i = 0; i < n; i++) b[i] = (byte) (i & 0xFF);
        return b;
    }

    @Test
    public void testOkCopiesWholeStream() throws IOException {
        byte[] data = bytesOf(5 * 1024);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        CameraFragment.BoundedStreamCopy.Result r =
                CameraFragment.BoundedStreamCopy.copy(
                        new ByteArrayInputStream(data), out, new byte[1024],
                        NO_LIMIT, NO_LIMIT, () -> false, new SeqClock(0L), null);
        assertEquals(CameraFragment.BoundedStreamCopy.Status.OK, r.status);
        assertEquals(data.length, r.bytesCopied);
        assertTrue(Arrays.equals(data, out.toByteArray()));
    }

    @Test
    public void testByteLimitInterruptsWithoutExceedingCap() throws IOException {
        byte[] data = bytesOf(5 * 1024 * 1024); // 5MB 无界输入，模拟恶意 provider
        long maxBytes = 4096;                   // 4KB 上限
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        CameraFragment.BoundedStreamCopy.Result r =
                CameraFragment.BoundedStreamCopy.copy(
                        new ByteArrayInputStream(data), out, new byte[1024],
                        maxBytes, NO_LIMIT, () -> false, new SeqClock(0L), null);
        assertEquals("超长输入必须在字节上限处受控中断", CameraFragment.BoundedStreamCopy.Status.BYTE_LIMIT, r.status);
        assertTrue("写入磁盘的字节数不得超过上限", out.toByteArray().length <= maxBytes);
        assertEquals(out.toByteArray().length, r.bytesCopied);
    }

    @Test
    public void testByteLimitExactBoundaryIsOk() throws IOException {
        byte[] data = bytesOf(4096);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        CameraFragment.BoundedStreamCopy.Result r =
                CameraFragment.BoundedStreamCopy.copy(
                        new ByteArrayInputStream(data), out, new byte[1024],
                        4096, NO_LIMIT, () -> false, new SeqClock(0L), null);
        assertEquals("恰好等于上限应正常完成", CameraFragment.BoundedStreamCopy.Status.OK, r.status);
        assertEquals(4096, r.bytesCopied);
    }

    @Test
    public void testTimeLimitInterruptsSlowStream() throws IOException {
        byte[] data = bytesOf(2 * 1024 * 1024); // 2MB 长流
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        // 时钟：起始 0，首个 read 返回后跳到 2000ms，超过 1000ms 上限 → TIME_LIMIT
        CameraFragment.BoundedStreamCopy.Result r =
                CameraFragment.BoundedStreamCopy.copy(
                        new ByteArrayInputStream(data), out, new byte[1024],
                        NO_LIMIT, 1000L, () -> false, new SeqClock(0L, 2000L), null);
        assertEquals("耗时超过上限必须受控中断", CameraFragment.BoundedStreamCopy.Status.TIME_LIMIT, r.status);
        assertTrue("超时中断时已拷贝但未读至 EOF", r.bytesCopied > 0 && r.bytesCopied < data.length);
    }

    @Test
    public void testCancelledInterruptsImmediately() throws IOException {
        Flag cancelled = new Flag();
        cancelled.value = true;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        CameraFragment.BoundedStreamCopy.Result r =
                CameraFragment.BoundedStreamCopy.copy(
                        new ByteArrayInputStream(bytesOf(1024)), out, new byte[1024],
                        NO_LIMIT, NO_LIMIT, cancelled, new SeqClock(0L), null);
        assertEquals(CameraFragment.BoundedStreamCopy.Status.CANCELLED, r.status);
        assertEquals(0, r.bytesCopied);
    }

    @Test
    public void testMessageForMapsStatuses() {
        assertEquals(null, CameraFragment.BoundedStreamCopy.messageFor(CameraFragment.BoundedStreamCopy.Status.OK));
        assertTrue(CameraFragment.BoundedStreamCopy.messageFor(CameraFragment.BoundedStreamCopy.Status.BYTE_LIMIT)
                .contains("大小上限"));
        assertTrue(CameraFragment.BoundedStreamCopy.messageFor(CameraFragment.BoundedStreamCopy.Status.TIME_LIMIT)
                .contains("超时"));
        assertTrue(CameraFragment.BoundedStreamCopy.messageFor(CameraFragment.BoundedStreamCopy.Status.CANCELLED)
                .contains("取消"));
    }

    @Test
    public void testProgressCallbackInvokedPerChunk() throws IOException {
        byte[] data = bytesOf(3000);
        final long[] lastBytes = {-1};
        CameraFragment.BoundedStreamCopy.copy(
                new ByteArrayInputStream(data), new ByteArrayOutputStream(), new byte[1024],
                NO_LIMIT, NO_LIMIT, () -> false, new SeqClock(0L, 1L, 2L),
                p -> lastBytes[0] = p.bytesCopied);
        assertEquals(3000, lastBytes[0]);
    }
}
