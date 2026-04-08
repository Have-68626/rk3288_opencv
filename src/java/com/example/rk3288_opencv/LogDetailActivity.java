package com.example.rk3288_opencv;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.SpannableString;
import android.text.style.BackgroundColorSpan;
import android.graphics.Color;
import android.view.MenuItem;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class LogDetailActivity extends AppCompatActivity {

    public static final String EXTRA_LOG_PATH = "extra_log_path";
    private static final int DEFAULT_TAIL_BYTES = 256 * 1024;
    private static final int PAGE_BYTES = 256 * 1024;

    private TextView tvLogContent;
    private Button btnViewTail;
    private Button btnViewMore;
    private Button btnViewAll;
    private EditText etFilter;
    private Button btnFilterApply;
    private Button btnFilterClear;
    private Button btnFilterMain;
    private Button btnFilterService;
    private Button btnFilterJni;
    private ExecutorService executor = Executors.newSingleThreadExecutor();
    private Handler handler = new Handler(Looper.getMainLooper());
    private File currentFile;
    private long currentStartOffset = 0L;
    private boolean fullyLoaded = false;
    private volatile String rawContent = "";
    private volatile String maskedContent = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_log_detail);

        Toolbar toolbar = findViewById(R.id.toolbar_detail);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setDisplayShowHomeEnabled(true);
        }

        tvLogContent = findViewById(R.id.tv_log_content);
        btnViewTail = findViewById(R.id.btn_view_tail);
        btnViewMore = findViewById(R.id.btn_view_more);
        btnViewAll = findViewById(R.id.btn_view_all);
        etFilter = findViewById(R.id.et_filter);
        btnFilterApply = findViewById(R.id.btn_filter_apply);
        btnFilterClear = findViewById(R.id.btn_filter_clear);
        btnFilterMain = findViewById(R.id.btn_filter_mainactivity);
        btnFilterService = findViewById(R.id.btn_filter_statusservice);
        btnFilterJni = findViewById(R.id.btn_filter_jni);

        String path = getIntent().getStringExtra(EXTRA_LOG_PATH);
        if (path != null) {
            File file = new File(path);
            if (!isValidLogPath(file)) {
                Toast.makeText(this, "Invalid log path", Toast.LENGTH_SHORT).show();
                finish();
                return;
            }
            getSupportActionBar().setTitle(file.getName());
            currentFile = file;
            btnViewTail.setOnClickListener(v -> loadTail());
            btnViewMore.setOnClickListener(v -> loadMore());
            btnViewAll.setOnClickListener(v -> loadAll());
            if (btnFilterApply != null) btnFilterApply.setOnClickListener(v -> applyFilterFromUi());
            if (btnFilterClear != null) btnFilterClear.setOnClickListener(v -> clearFilter());
            if (btnFilterMain != null) btnFilterMain.setOnClickListener(v -> applyQuickFilter("MainActivity"));
            if (btnFilterService != null) btnFilterService.setOnClickListener(v -> applyQuickFilter("StatusService"));
            if (btnFilterJni != null) btnFilterJni.setOnClickListener(v -> applyQuickFilter("RK3288_JNI"));
            loadTail();
        } else {
            Toast.makeText(this, "File not found", Toast.LENGTH_SHORT).show();
            finish();
        }
    }

    private boolean isValidLogPath(File file) {
        try {
            String canonicalPath = file.getCanonicalPath();

            File internalLogs = new File(getFilesDir(), "logs");
            if (canonicalPath.startsWith(internalLogs.getCanonicalPath() + File.separator)) {
                return true;
            }

            File extFiles = getExternalFilesDir(null);
            if (extFiles != null) {
                File pkgDir = extFiles.getParentFile();
                if (pkgDir != null) {
                    File externalLogs = new File(pkgDir, "logs");
                    if (canonicalPath.startsWith(externalLogs.getCanonicalPath() + File.separator)) {
                        return true;
                    }
                }
                File fallback = new File(extFiles, "logs");
                if (canonicalPath.startsWith(fallback.getCanonicalPath() + File.separator)) {
                    return true;
                }
            }

            return false;
        } catch (IOException e) {
            return false;
        }
    }

    private void loadTail() {
        File file = currentFile;
        if (file == null) return;
        executor.execute(() -> {
            String out = readTailUtf8(file, DEFAULT_TAIL_BYTES);
            rawContent = out;
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            handler.post(() -> {
                applyFilterFromUi();
                btnViewMore.setEnabled(!fullyLoaded && currentStartOffset > 0);
            });
        });
    }

    private void loadMore() {
        File file = currentFile;
        if (file == null) return;
        if (fullyLoaded || currentStartOffset <= 0) return;
        executor.execute(() -> {
            long newStart = Math.max(0L, currentStartOffset - PAGE_BYTES);
            String prefix = readRangeUtf8(file, newStart, currentStartOffset - newStart);
            rawContent = prefix + rawContent;
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            handler.post(() -> {
                applyFilterFromUi();
                btnViewMore.setEnabled(!fullyLoaded && currentStartOffset > 0);
            });
            currentStartOffset = newStart;
            if (currentStartOffset <= 0) {
                fullyLoaded = true;
            }
        });
    }

    private void loadAll() {
        File file = currentFile;
        if (file == null) return;
        executor.execute(() -> {
            rawContent = readAllUtf8(file);
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            handler.post(() -> {
                applyFilterFromUi();
                btnViewMore.setEnabled(false);
            });
            fullyLoaded = true;
            currentStartOffset = 0L;
        });
    }

    private void applyQuickFilter(String keyword) {
        if (etFilter == null) return;
        etFilter.setText(keyword);
        applyFilterFromUi();
    }

    private void clearFilter() {
        if (etFilter != null) {
            etFilter.setText("");
        }
        applyFilterFromUi();
    }

    private void applyFilterFromUi() {
        String src = maskedContent == null ? "" : maskedContent;
        String p = etFilter == null ? "" : String.valueOf(etFilter.getText()).trim();
        if (p.isEmpty()) {
            tvLogContent.setText(src);
            return;
        }

        final Pattern pattern;
        try {
            pattern = Pattern.compile(p);
        } catch (PatternSyntaxException e) {
            Toast.makeText(this, "正则错误: " + e.getDescription(), Toast.LENGTH_SHORT).show();
            tvLogContent.setText(src);
            return;
        }

        executor.execute(() -> {
            String filtered = filterLines(src, pattern);
            SpannableString sp = new SpannableString(filtered);
            try {
                java.util.regex.Matcher m = pattern.matcher(filtered);
                while (m.find()) {
                    sp.setSpan(new BackgroundColorSpan(Color.argb(140, 255, 235, 59)), m.start(), m.end(), 0);
                }
            } catch (Throwable ignored) {
            }
            handler.post(() -> tvLogContent.setText(sp));
        });
    }

    private static String filterLines(String src, Pattern pattern) {
        if (src == null || src.isEmpty()) return "";
        String[] lines = src.split("\n", -1);
        StringBuilder sb = new StringBuilder();
        for (String line : lines) {
            if (pattern.matcher(line).find()) {
                sb.append(line).append('\n');
            }
        }
        return sb.toString();
    }

    private String readAllUtf8(File file) {
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            StringBuilder sb = new StringBuilder();
            char[] buf = new char[8192];
            int n;
            while ((n = reader.read(buf)) != -1) {
                sb.append(buf, 0, n);
            }
            return sb.toString();
        } catch (IOException e) {
            return "读取失败: " + e.getMessage();
        }
    }

    private String readTailUtf8(File file, int maxBytes) {
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            long len = raf.length();
            long start = Math.max(0L, len - maxBytes);
            currentStartOffset = start;
            fullyLoaded = (start == 0L);
            return readRangeUtf8(file, start, len - start);
        } catch (IOException e) {
            return "读取失败: " + e.getMessage();
        }
    }

    private String readRangeUtf8(File file, long offset, long length) {
        if (length <= 0) return "";
        try (RandomAccessFile raf = new RandomAccessFile(file, "r")) {
            raf.seek(offset);
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            long remaining = length;
            while (remaining > 0) {
                int read = raf.read(buf, 0, (int) Math.min(buf.length, remaining));
                if (read <= 0) break;
                bos.write(buf, 0, read);
                remaining -= read;
            }
            return new String(bos.toByteArray(), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "读取失败: " + e.getMessage();
        }
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
