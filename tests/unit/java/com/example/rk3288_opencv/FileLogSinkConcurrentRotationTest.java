package com.example.rk3288_opencv;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * Q-CQ-B07-007 RED 判别测试：8 线程并发写日志并触发轮转。
 *
 * RED 判别：修复前 FileLogSink.lock 声明后未使用，writeLine/rotateIfNeeded 无锁，
 * 且每行没有 "#seq " 前缀 → 行格式解析直接失败 → RED（确定性，非偶发）。
 * GREEN：修复后序列号在锁内生成，所有行的序列号应恰好为 1..N（无重复/缺口=无丢失/交错），
 * 且每行格式完整（无截断/交错字节）。
 */
public class FileLogSinkConcurrentRotationTest {

    private static final int THREADS = 8;
    private static final int LINES_PER_THREAD = 2625;
    // 每行 ≈515 字节（#seq + T{tid}-{i} 前缀 + 500 字节负载 + \n）。
    // 总量 = 8 × 2625 = 21,000 行 ≈ 10.8MB。
    // 轮转阈值 5MB：第 1 次轮转在累计 >5MB（~10,190 行），第 2 次轮转需累计 >2×5MB ≈ 10.49MB；
    // 10.8MB 超出 10.49MB 约 330KB → 确定性触发恰好 2 次轮转（余量远超线程调度造成的字节级抖动）。
    private static final int PAYLOAD_LEN = 500;

    // 每行完整格式：#seq T{tid}-{i} mmm...(PAYLOAD_LEN)
    private static final Pattern LINE = Pattern.compile("^#(\\d+) T(\\d+)-(\\d+) (m+)$");

    private File dir;
    private String fileName;

    @Before
    public void setUp() throws Exception {
        dir = Files.createTempDirectory("rk3288_log_concurrent").toFile();
        fileName = "concurrent_" + System.nanoTime() + ".log";
    }

    @After
    public void tearDown() {
        deleteRecursively(dir);
    }

    @Test
    public void testConcurrentWritesAndRotationSequenceUniqueAndComplete() throws Exception {
        final FileLogSink sink = new FileLogSink(Collections.singletonList(dir), fileName);
        final String payload = "m".repeat(PAYLOAD_LEN);
        final long expected = (long) THREADS * LINES_PER_THREAD;

        // 栅栏：8 线程就绪后同时开跑，最大化并发写入与轮转竞争
        final CountDownLatch ready = new CountDownLatch(THREADS);
        final CountDownLatch start = new CountDownLatch(1);
        final CountDownLatch done = new CountDownLatch(THREADS);
        final AtomicReference<Throwable> failure = new AtomicReference<>();

        for (int t = 0; t < THREADS; t++) {
            final int tid = t;
            new Thread(() -> {
                ready.countDown();
                try {
                    start.await();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
                try {
                    for (int i = 0; i < LINES_PER_THREAD; i++) {
                        sink.writeLine("T" + tid + "-" + i + " " + payload);
                    }
                } catch (Throwable th) {
                    failure.compareAndSet(null, th);
                } finally {
                    done.countDown();
                }
            }, "log-writer-" + t).start();
        }

        ready.await();
        start.countDown();
        done.await();

        assertNull("写线程不应抛异常", failure.get());

        // 轮转计数判别（A1/S3）：修复前恒 1 次轮转——20,000 行 ≈10.3MB，
        // 第 1 次轮转后仅剩 ~9,820 行 ≈5.06MB < 5MB 阈值，且无 ≥2 次轮转断言
        // （files.size()>=1 平凡成立），级联回滚 .1→.9 路径未被覆盖。
        // 现在总量 10.8MB > 2×5MB → 确定性 2 次轮转：主文件 + .1 + .2 共 3 个文件。
        // files.size() >= 3 使 0 次轮转（1 个文件）或 1 次轮转（2 个文件）均判定失败。
        List<File> files = listLogFiles();
        assertTrue("应触发至少 2 次轮转（级联回滚覆盖 .1→.2）：主文件 + .1 + .2 共 ≥3 个文件",
                files.size() >= 3);

        List<Long> allSeqs = new ArrayList<>();
        for (File f : files) {
            List<Long> seqs = parseSeqs(f);
            assertIncreasingWithinFile(seqs, f.getName());
            allSeqs.addAll(seqs);
        }

        // ① 序列号唯一递增：1..N 恰好出现一次（无重复/缺口 => 无行丢失/交错）
        Collections.sort(allSeqs);
        assertEquals("总行数应等于写入量（无丢失）", expected, allSeqs.size());
        for (int i = 0; i < allSeqs.size(); i++) {
            assertEquals("序列号应连续无缺口/无重复，index=" + i, i + 1L, allSeqs.get(i).longValue());
        }
    }

    /** 解析单文件：每行必须是完整格式，否则视为截断/交错损坏（RED 判别点②）。 */
    private List<Long> parseSeqs(File f) throws Exception {
        List<Long> seqs = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(new FileReader(f, StandardCharsets.UTF_8))) {
            String line;
            int no = 0;
            while ((line = r.readLine()) != null) {
                no++;
                Matcher m = LINE.matcher(line);
                if (!m.matches()) {
                    fail("文件 " + f.getName() + " 第 " + no + " 行格式损坏（截断/交错）: " + line);
                }
                if (m.group(4).length() != PAYLOAD_LEN) {
                    fail("文件 " + f.getName() + " 第 " + no + " 行负载被截断: " + line);
                }
                seqs.add(Long.parseLong(m.group(1)));
            }
        }
        return seqs;
    }

    /** 同一文件内序列号应严格递增（轮转保持每个文件内的连续递增区间）。 */
    private void assertIncreasingWithinFile(List<Long> seqs, String name) {
        for (int i = 1; i < seqs.size(); i++) {
            if (seqs.get(i) <= seqs.get(i - 1)) {
                fail("文件 " + name + " 内序列号非递增: " + seqs.get(i - 1) + " -> " + seqs.get(i));
            }
        }
    }

    private List<File> listLogFiles() {
        List<File> files = new ArrayList<>();
        File main = new File(dir, fileName);
        if (main.exists()) files.add(main);
        for (int i = 1; i <= 9; i++) {
            File rolled = new File(dir, fileName + "." + i);
            if (rolled.exists()) files.add(rolled);
        }
        return files;
    }

    private static void deleteRecursively(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) deleteRecursively(c);
        }
        f.delete();
    }
}
