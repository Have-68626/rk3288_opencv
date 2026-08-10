package com.example.rk3288_opencv;

import androidx.annotation.NonNull;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.locks.ReentrantLock;

final class FileLogSink {
    private static final long MAX_BYTES = 5L * 1024L * 1024L;
    private static final int MAX_ROLL_FILES = 9;
    
    private final String fileName;
    private final List<File> dirs;
    // Q-CQ-B07-007：既有锁，修复前声明后从未使用。
    // 现在用它串行化「序列号分配 + 轮转 + 追加」，保证并发写时序列号全局唯一递增，
    // 且轮转与追加互斥（杜绝 rename/append 交错造成的截断或缺行）。
    private final ReentrantLock lock = new ReentrantLock();
    // 序列号在锁内自增，跨线程唯一且单调递增，可据此跨轮转文件校验完整无丢失。
    private long nextSeq = 1;

    FileLogSink(@NonNull List<File> dirs, @NonNull String fileName) {
        this.dirs = new ArrayList<>(Objects.requireNonNull(dirs));
        this.fileName = Objects.requireNonNull(fileName);
    }

    void writeLine(@NonNull String line) {
        // Q-CQ-B07-007：整段在 lock 内串行化（序列号生成 + 轮转 + 追加）。
        // 修复前无锁：两个线程同时进入 rotateIfNeeded，renameTo 互相覆盖导致轮转文件截断/丢失；
        // 追加与 rename 交错时，部分行写入已轮转走的旧 inode，主文件缺行/交错。
        // 日志写入非热路径，跨目录串行化可接受。
        lock.lock();
        try {
            long seq = nextSeq++;
            String suffix = line.endsWith("\n") ? "" : "\n";
            byte[] bytes = ("#" + seq + " " + line + suffix).getBytes(StandardCharsets.UTF_8);

            for (File dir : dirs) {
                File file = null;
                try {
                    if (dir == null) continue;
                    if (!dir.exists() && !dir.mkdirs()) continue;
                    file = new File(dir, fileName);
                    rotateIfNeeded(file);
                    try (FileOutputStream fos = new FileOutputStream(file, true);
                         BufferedOutputStream bos = new BufferedOutputStream(fos)) {
                        bos.write(bytes);
                        bos.flush();
                    }
                } catch (Exception ignored) {
                }
            }
        } finally {
            lock.unlock();
        }
    }

    private void rotateIfNeeded(@NonNull File file) {
        // Q-CQ-B07-007：本方法仅在 writeLine 持锁期间调用，轮转与后续追加互斥，
        // 锁内不存在其他写者持有旧文件句柄，故移除原无锁补丁的 SystemClock.sleep(10)（纯 JVM 测试亦不可用）。
        if (!file.exists()) return;
        if (file.length() <= MAX_BYTES) return;

        File dir = file.getParentFile();
        if (dir == null) return;

        for (int i = MAX_ROLL_FILES - 1; i >= 1; i--) {
            File older = new File(dir, fileName + "." + i);
            File newer = new File(dir, fileName + "." + (i + 1));
            if (older.exists()) {
                if (newer.exists()) {
                    newer.delete();
                }
                older.renameTo(newer);
            }
        }

        File first = new File(dir, fileName + ".1");
        if (first.exists()) {
            first.delete();
        }
        file.renameTo(first);
    }
}

