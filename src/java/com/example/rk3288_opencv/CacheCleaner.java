package com.example.rk3288_opencv;

import android.content.Context;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.util.ArrayDeque;
import java.util.Deque;

final class CacheCleaner {
    private CacheCleaner() {
    }

    static Result cleanOnExit(@NonNull Context context) {
        File internal = context.getCacheDir();
        File external = context.getExternalCacheDir();

        Result a = cleanDir(internal);
        Result b = cleanDir(external);
        return new Result(a.deletedFiles + b.deletedFiles, a.deletedBytes + b.deletedBytes, a.errors + b.errors);
    }

    private static Result cleanDir(@Nullable File dir) {
        if (dir == null || !dir.exists() || !dir.isDirectory()) {
            return new Result(0, 0, 0);
        }

        long deletedFiles = 0;
        long deletedBytes = 0;
        int errors = 0;

        Deque<File> stack = new ArrayDeque<>();
        stack.push(dir);

        Deque<File> toDeleteDirs = new ArrayDeque<>();
        while (!stack.isEmpty()) {
            File cur = stack.pop();
            File[] children = cur.listFiles();
            if (children == null) continue;

            for (File child : children) {
                if (child.isDirectory()) {
                    stack.push(child);
                    toDeleteDirs.push(child);
                } else {
                    long len = child.length();
                    if (child.delete()) {
                        deletedFiles++;
                        deletedBytes += len;
                    } else {
                        errors++;
                    }
                }
            }
        }

        while (!toDeleteDirs.isEmpty()) {
            File d = toDeleteDirs.pop();
            File[] remaining = d.listFiles();
            if (remaining != null && remaining.length > 0) continue;
            if (!d.delete()) {
                errors++;
            }
        }

        return new Result(deletedFiles, deletedBytes, errors);
    }

    static final class Result {
        final long deletedFiles;
        final long deletedBytes;
        final int errors;

        Result(long deletedFiles, long deletedBytes, int errors) {
            this.deletedFiles = deletedFiles;
            this.deletedBytes = deletedBytes;
            this.errors = errors;
        }
    }
}

