package com.example.rk3288_opencv;

import android.os.SystemClock;

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
    private final ReentrantLock lock = new ReentrantLock();

    FileLogSink(@NonNull List<File> dirs, @NonNull String fileName) {
        this.dirs = new ArrayList<>(Objects.requireNonNull(dirs));
        this.fileName = Objects.requireNonNull(fileName);
    }

    void writeLine(@NonNull String line) {
        String payload = line.endsWith("\n") ? line : (line + "\n");
        byte[] bytes = payload.getBytes(StandardCharsets.UTF_8);

        lock.lock();
        try {
            for (File dir : dirs) {
                try {
                    if (dir == null) continue;
                    if (!dir.exists() && !dir.mkdirs()) continue;
                    File file = new File(dir, fileName);
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

        SystemClock.sleep(10);
    }
}

