package com.example.rk3288_opencv;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.SpannableString;
import android.text.Editable;
import android.text.TextWatcher;
import android.text.style.BackgroundColorSpan;
import android.graphics.Color;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.widget.ScrollView;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.widget.Toolbar;

import java.io.ByteArrayOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import android.os.SystemClock;

public class LogDetailActivity extends AppCompatActivity {

    public static final String EXTRA_LOG_PATH = "extra_log_path";
    public static final String EXTRA_DELETED_LOG_PATH = "extra_deleted_log_path";
    private static final int DEFAULT_TAIL_BYTES = 256 * 1024;
    private static final int PAGE_BYTES = 256 * 1024;
    private static final int MAX_FILTER_OUTPUT_LINES = 5000;
    private static final int MAX_FILTER_OUTPUT_CHARS = 1_200_000;
    private static final int MAX_FILTER_HIGHLIGHTS = 6000;
    private static final long MAX_FILTER_TIME_MS = 1200;
    private static final int MAX_ALL_BYTES = 8 * 1024 * 1024;

    private TextView tvLogContent;
    private TextView tvLoadInfo;
    private Button btnViewTail;
    private Button btnViewMore;
    private Button btnViewAll;
    private EditText etFilter;
    private Button btnFilterMain;
    private Button btnFilterService;
    private Button btnFilterJni;
    private ScrollView scrollView;
    private ImageButton btnDeleteCurrentLog;
    private ImageButton btnToggleLineNumbers;
    private ExecutorService executor = Executors.newSingleThreadExecutor();
    private Handler handler = new Handler(Looper.getMainLooper());
    private File currentFile;
    private long currentStartOffset = 0L;
    private boolean fullyLoaded = false;
    private volatile String rawContent = "";
    private volatile String maskedContent = "";
    private volatile int filterJobSeq = 0;
    private final Runnable debounceFilterRunnable = () -> applyFilterFromUi(false, false);
    private volatile long lastLoadDurationMs = 0L;
    private volatile String lastLoadOp = "";
    private volatile boolean showLineNumbers = true;
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_LOG_LINE_NUMBERS = "pref_log_line_numbers";

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
        tvLoadInfo = findViewById(R.id.tv_load_info);
        btnViewTail = findViewById(R.id.btn_view_tail);
        btnViewMore = findViewById(R.id.btn_view_more);
        btnViewAll = findViewById(R.id.btn_view_all);
        etFilter = findViewById(R.id.et_filter);
        btnFilterMain = findViewById(R.id.btn_filter_mainactivity);
        btnFilterService = findViewById(R.id.btn_filter_statusservice);
        btnFilterJni = findViewById(R.id.btn_filter_jni);
        scrollView = findViewById(R.id.scroll_view);
        btnDeleteCurrentLog = findViewById(R.id.btn_delete_current_log);
        btnToggleLineNumbers = findViewById(R.id.btn_toggle_line_numbers);
        showLineNumbers = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getBoolean(PREF_LOG_LINE_NUMBERS, true);
        syncLineNumberButtonUi();

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
            if (btnDeleteCurrentLog != null) btnDeleteCurrentLog.setOnClickListener(v -> confirmDeleteCurrentLog());
            if (btnToggleLineNumbers != null) btnToggleLineNumbers.setOnClickListener(v -> toggleLineNumbers());
            btnViewTail.setOnClickListener(v -> loadTail());
            btnViewMore.setOnClickListener(v -> loadMore());
            btnViewAll.setOnClickListener(v -> loadAll());
            setupFilterInput();
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
            if (canonicalPath.equals(internalLogs.getCanonicalPath()) || canonicalPath.startsWith(internalLogs.getCanonicalPath() + File.separator)) {
                return true;
            }

            File extFiles = getExternalFilesDir(null);
            if (extFiles != null) {
                File pkgDir = extFiles.getParentFile();
                if (pkgDir != null) {
                    File externalLogs = new File(pkgDir, "logs");
                    if (canonicalPath.equals(externalLogs.getCanonicalPath()) || canonicalPath.startsWith(externalLogs.getCanonicalPath() + File.separator)) {
                        return true;
                    }
                }
                File fallback = new File(extFiles, "logs");
                if (canonicalPath.equals(fallback.getCanonicalPath()) || canonicalPath.startsWith(fallback.getCanonicalPath() + File.separator)) {
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
            long startMs = SystemClock.elapsedRealtime();
            String out = readTailUtf8(file, DEFAULT_TAIL_BYTES);
            lastLoadDurationMs = Math.max(0L, SystemClock.elapsedRealtime() - startMs);
            lastLoadOp = "看末尾";
            rawContent = out;
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            handler.post(() -> {
                applyFilterFromUi(true, true);
                updateLoadInfoUi();
            });
        });
    }

    private void loadMore() {
        File file = currentFile;
        if (file == null) return;
        if (fullyLoaded || currentStartOffset <= 0) return;
        executor.execute(() -> {
            long startMs = SystemClock.elapsedRealtime();
            long newStart = Math.max(0L, currentStartOffset - PAGE_BYTES);
            String prefix = readRangeUtf8(file, newStart, currentStartOffset - newStart);
            rawContent = prefix + rawContent;
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            currentStartOffset = newStart;
            if (currentStartOffset <= 0) {
                fullyLoaded = true;
            }
            lastLoadDurationMs = Math.max(0L, SystemClock.elapsedRealtime() - startMs);
            lastLoadOp = "加载更多";
            handler.post(() -> {
                applyFilterFromUi(false, false);
                updateLoadInfoUi();
            });
        });
    }

    private void loadAll() {
        File file = currentFile;
        if (file == null) return;
        executor.execute(() -> {
            long startMs = SystemClock.elapsedRealtime();
            long len = file.length();
            boolean truncated = len > MAX_ALL_BYTES;
            if (truncated) {
                long start = Math.max(0L, len - MAX_ALL_BYTES);
                rawContent = readRangeUtf8(file, start, len - start);
                currentStartOffset = start;
                fullyLoaded = (start == 0L);
            } else {
                rawContent = readAllUtf8(file);
                fullyLoaded = true;
                currentStartOffset = 0L;
            }
            lastLoadDurationMs = Math.max(0L, SystemClock.elapsedRealtime() - startMs);
            lastLoadOp = truncated ? "看全部(截断)" : "看全部";
            maskedContent = SensitiveDataUtil.maskSensitiveData(rawContent);
            handler.post(() -> {
                applyFilterFromUi(false, false);
                updateLoadInfoUi();
                if (truncated) {
                    Toast.makeText(this, "日志过大：仅加载末尾 " + (MAX_ALL_BYTES / (1024 * 1024)) + "MB，可继续“加载更多”", Toast.LENGTH_LONG).show();
                }
            });
        });
    }

    private void applyQuickFilter(String keyword) {
        if (etFilter == null) return;
        etFilter.setText(keyword);
        scheduleApplyFilter(true);
    }

    private void applyFilterFromUi(boolean scrollToBottom, boolean highlightTailWhenNoFilter) {
        String src = maskedContent == null ? "" : maskedContent;
        String p = etFilter == null ? "" : String.valueOf(etFilter.getText()).trim();
        if (p.isEmpty()) {
            if (highlightTailWhenNoFilter && src != null && !src.isEmpty()) {
                NumberedTextResult numbered = showLineNumbers ? addLineNumbers(src, new ArrayList<>()) : new NumberedTextResult(src, new ArrayList<>());
                SpannableString sp = new SpannableString(numbered.text);
                int n = Math.min(4000, sp.length());
                int start = Math.max(0, sp.length() - n);
                sp.setSpan(new BackgroundColorSpan(Color.argb(70, 0, 255, 120)), start, sp.length(), 0);
                tvLogContent.setText(sp);
            } else {
                if (showLineNumbers) {
                    tvLogContent.setText(addLineNumbers(src, new ArrayList<>()).text);
                } else {
                    tvLogContent.setText(src);
                }
            }
            if (scrollToBottom) {
                scrollToEnd();
            }
            return;
        }
        if (p.length() > 128) {
            Toast.makeText(this, "筛选表达式过长（>128），请缩短后重试", Toast.LENGTH_SHORT).show();
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

        int job = ++filterJobSeq;
        tvLogContent.setText("筛选中…");
        executor.execute(() -> {
            FilterRenderResult r = filterFromBestSource(job, pattern, src);
            handler.post(() -> {
                if (job != filterJobSeq) return;
                NumberedTextResult numbered = showLineNumbers ? addLineNumbers(r.text, r.highlights) : new NumberedTextResult(r.text, r.highlights);
                SpannableString sp = new SpannableString(numbered.text);
                int applied = 0;
                for (IntRange range : numbered.highlights) {
                    if (applied++ >= MAX_FILTER_HIGHLIGHTS) break;
                    int start = Math.max(0, Math.min(range.start, sp.length()));
                    int end = Math.max(start, Math.min(range.end, sp.length()));
                    if (end > start) {
                        sp.setSpan(new BackgroundColorSpan(Color.argb(140, 255, 235, 59)), start, end, 0);
                    }
                }
                tvLogContent.setText(sp);
                if (scrollToBottom) {
                    scrollToEnd();
                }
                if (r.toast != null && !r.toast.isEmpty()) {
                    Toast.makeText(this, r.toast, Toast.LENGTH_LONG).show();
                }
            });
        });
    }

    private void applyFilterFromUi() {
        applyFilterFromUi(false, false);
    }

    private void setupFilterInput() {
        if (etFilter == null) return;
        updateFilterClearIcon();
        etFilter.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                updateFilterClearIcon();
                scheduleApplyFilter(false);
            }

            @Override
            public void afterTextChanged(Editable s) {
            }
        });

        etFilter.setOnTouchListener((v, event) -> {
            if (event.getAction() != MotionEvent.ACTION_UP) return false;
            if (etFilter.getCompoundDrawablesRelative()[2] == null) return false;
            int x = (int) event.getX();
            int width = etFilter.getWidth();
            int paddingEnd = etFilter.getPaddingEnd();
            int iconWidth = etFilter.getCompoundDrawablesRelative()[2].getBounds().width();
            if (x >= width - paddingEnd - iconWidth) {
                etFilter.setText("");
                scheduleApplyFilter(true);
                return true;
            }
            return false;
        });
    }

    private void scheduleApplyFilter(boolean immediate) {
        handler.removeCallbacks(debounceFilterRunnable);
        if (immediate) {
            handler.post(debounceFilterRunnable);
        } else {
            handler.postDelayed(debounceFilterRunnable, 250);
        }
    }

    private void updateFilterClearIcon() {
        if (etFilter == null) return;
        boolean has = etFilter.getText() != null && etFilter.getText().length() > 0;
        if (has) {
            etFilter.setCompoundDrawablesRelativeWithIntrinsicBounds(0, 0, android.R.drawable.ic_menu_close_clear_cancel, 0);
        } else {
            etFilter.setCompoundDrawablesRelativeWithIntrinsicBounds(0, 0, 0, 0);
        }
    }

    private void scrollToEnd() {
        if (scrollView == null) return;
        scrollView.post(() -> scrollView.fullScroll(ScrollView.FOCUS_DOWN));
    }

    private void updateLoadInfoUi() {
        if (btnViewMore != null) {
            btnViewMore.setEnabled(!fullyLoaded && currentStartOffset > 0);
        }
        File f = currentFile;
        if (tvLoadInfo == null || f == null) return;
        long total = -1L;
        try {
            total = f.length();
        } catch (Throwable ignored) {
        }
        long start = currentStartOffset;
        long loaded = total >= 0 ? (total - start) : -1L;
        String range = (total >= 0)
                ? ("已加载 " + formatBytes(loaded) + "（" + formatBytes(start) + " ~ " + formatBytes(total) + "）")
                : "已加载 --";
        String full = fullyLoaded ? "已全量" : "非全量";
        String cost = lastLoadDurationMs > 0 ? (lastLoadDurationMs + "ms") : "--";
        String op = (lastLoadOp == null || lastLoadOp.isEmpty()) ? "" : (lastLoadOp + "  ");
        tvLoadInfo.setText(op + range + "  " + full + "  用时 " + cost);
    }

    private static String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        double b = bytes;
        if (b < 1024) return String.format(java.util.Locale.US, "%.0fB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(java.util.Locale.US, "%.1fKB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(java.util.Locale.US, "%.1fMB", b);
        b /= 1024.0;
        return String.format(java.util.Locale.US, "%.2fGB", b);
    }

    private void toggleLineNumbers() {
        showLineNumbers = !showLineNumbers;
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(PREF_LOG_LINE_NUMBERS, showLineNumbers).apply();
        syncLineNumberButtonUi();
        applyFilterFromUi(false, false);
    }

    private void syncLineNumberButtonUi() {
        if (btnToggleLineNumbers == null) return;
        btnToggleLineNumbers.setAlpha(showLineNumbers ? 1.0f : 0.45f);
    }

    private void confirmDeleteCurrentLog() {
        File file = currentFile;
        if (file == null) return;
        new AlertDialog.Builder(this)
                .setTitle("删除日志")
                .setMessage("确定删除当前日志？\n\n" + file.getName())
                .setPositiveButton("删除", (d, w) -> deleteCurrentLogNow())
                .setNegativeButton("取消", null)
                .show();
    }

    private void deleteCurrentLogNow() {
        File file = currentFile;
        if (file == null) return;
        boolean ok = false;
        String err = null;
        try {
            ok = file.delete();
            if (!ok) {
                err = "删除失败（可能被占用或无权限）";
            }
        } catch (Throwable t) {
            err = t.getMessage();
        }
        if (!ok) {
            Toast.makeText(this, "删除失败: " + err, Toast.LENGTH_LONG).show();
            return;
        }
        Toast.makeText(this, "已删除: " + file.getName(), Toast.LENGTH_SHORT).show();
        Intent data = new Intent();
        data.putExtra(EXTRA_DELETED_LOG_PATH, file.getAbsolutePath());
        setResult(RESULT_OK, data);
        finish();
    }

    private static final class NumberedTextResult {
        final String text;
        final List<IntRange> highlights;

        NumberedTextResult(String text, List<IntRange> highlights) {
            this.text = text;
            this.highlights = highlights;
        }
    }

    private static NumberedTextResult addLineNumbers(String src, List<IntRange> srcHighlights) {
        if (src == null || src.isEmpty()) return new NumberedTextResult("", new ArrayList<>());
        List<IntRange> outHighlights = new ArrayList<>();
        StringBuilder out = new StringBuilder(Math.min(src.length() + 64, 2_000_000));

        List<IntRange> in = srcHighlights == null ? new ArrayList<>() : srcHighlights;
        int hi = 0;

        int lineNo = 1;
        int i = 0;
        while (i <= src.length()) {
            int j = src.indexOf('\n', i);
            boolean hasNl = j >= 0;
            if (!hasNl) j = src.length();

            int srcBase = i;
            int srcEnd = j;
            String line = src.substring(srcBase, srcEnd);

            String prefix = String.format(java.util.Locale.US, "%5d | ", lineNo);
            int dstBase = out.length();
            out.append(prefix).append(line);
            if (hasNl) out.append('\n');

            while (hi < in.size()) {
                IntRange r = in.get(hi);
                if (r.start >= srcEnd) break;
                int shift = (dstBase + prefix.length()) - srcBase;
                outHighlights.add(new IntRange(r.start + shift, r.end + shift));
                hi++;
            }

            lineNo++;
            i = hasNl ? (j + 1) : (src.length() + 1);
        }

        return new NumberedTextResult(out.toString(), outHighlights);
    }

    private FilterRenderResult filterFromBestSource(int job, Pattern pattern, String maskedLoadedContent) {
        File file = currentFile;
        if (file != null) {
            try {
                long len = file.length();
                boolean fileHuge = len > (2L * 1024L * 1024L);
                boolean loadedHuge = maskedLoadedContent != null && maskedLoadedContent.length() > (2 * 1024 * 1024);
                if (fileHuge || loadedHuge) {
                    return filterFileStreaming(job, file, pattern);
                }
            } catch (Throwable ignored) {
            }
        }
        return filterStringStreaming(job, maskedLoadedContent, pattern);
    }

    private static final class IntRange {
        final int start;
        final int end;

        IntRange(int start, int end) {
            this.start = start;
            this.end = end;
        }
    }

    private static final class FilterRenderResult {
        final String text;
        final List<IntRange> highlights;
        final String toast;

        FilterRenderResult(String text, List<IntRange> highlights, String toast) {
            this.text = text;
            this.highlights = highlights;
            this.toast = toast;
        }
    }

    private FilterRenderResult filterStringStreaming(int job, String src, Pattern pattern) {
        if (src == null || src.isEmpty()) {
            return new FilterRenderResult("", new ArrayList<>(), null);
        }
        long startMs = SystemClock.uptimeMillis();
        StringBuilder out = new StringBuilder(Math.min(src.length(), 64 * 1024));
        List<IntRange> spans = new ArrayList<>();
        int linesOut = 0;
        int i = 0;
        while (i < src.length()) {
            if (job != filterJobSeq) {
                return new FilterRenderResult("", new ArrayList<>(), null);
            }
            if (SystemClock.uptimeMillis() - startMs > MAX_FILTER_TIME_MS) {
                break;
            }
            int j = src.indexOf('\n', i);
            if (j < 0) j = src.length();
            String line = src.substring(i, j);
            try {
                java.util.regex.Matcher m = pattern.matcher(line);
                if (m.find()) {
                    int base = out.length();
                    out.append(line).append('\n');
                    linesOut++;
                    m.reset(line);
                    while (m.find() && spans.size() < MAX_FILTER_HIGHLIGHTS) {
                        spans.add(new IntRange(base + m.start(), base + m.end()));
                    }
                }
            } catch (Throwable ignored) {
            }
            if (linesOut >= MAX_FILTER_OUTPUT_LINES || out.length() >= MAX_FILTER_OUTPUT_CHARS) {
                break;
            }
            i = j + 1;
        }
        String toast = null;
        if (linesOut >= MAX_FILTER_OUTPUT_LINES || out.length() >= MAX_FILTER_OUTPUT_CHARS) {
            toast = "筛选结果已截断（命中行数/内容过多）";
        } else if (SystemClock.uptimeMillis() - startMs > MAX_FILTER_TIME_MS) {
            toast = "筛选已提前停止（耗时超限）";
        }
        if (out.length() == 0) {
            return new FilterRenderResult("无匹配结果\n", new ArrayList<>(), null);
        }
        if (toast != null) {
            out.append("\n[提示] ").append(toast).append('\n');
        }
        return new FilterRenderResult(out.toString(), spans, toast);
    }

    private FilterRenderResult filterFileStreaming(int job, File file, Pattern pattern) {
        long startMs = SystemClock.uptimeMillis();
        StringBuilder out = new StringBuilder(64 * 1024);
        List<IntRange> spans = new ArrayList<>();
        int linesOut = 0;

        long len;
        try {
            len = file.length();
        } catch (Throwable t) {
            return new FilterRenderResult("读取失败: " + t.getMessage(), new ArrayList<>(), null);
        }

        long scanStart = currentStartOffset;
        boolean scanTruncated = false;
        if (scanStart <= 0 && len > MAX_ALL_BYTES) {
            scanStart = Math.max(0L, len - MAX_ALL_BYTES);
            scanTruncated = scanStart > 0;
        }

        boolean skipFirstPartialLine = scanStart > 0;
        try (FileInputStream fis = new FileInputStream(file)) {
            long skipped = 0;
            while (skipped < scanStart) {
                long s = fis.skip(scanStart - skipped);
                if (s <= 0) break;
                skipped += s;
            }
            BufferedReader reader = new BufferedReader(new InputStreamReader(fis, StandardCharsets.UTF_8));
            String line;
            while ((line = reader.readLine()) != null) {
                if (job != filterJobSeq) {
                    return new FilterRenderResult("", new ArrayList<>(), null);
                }
                if (SystemClock.uptimeMillis() - startMs > MAX_FILTER_TIME_MS) {
                    break;
                }
                if (skipFirstPartialLine) {
                    skipFirstPartialLine = false;
                    continue;
                }
                try {
                    java.util.regex.Matcher m = pattern.matcher(line);
                    if (m.find()) {
                        String maskedLine = SensitiveDataUtil.maskSensitiveData(line);
                        int base = out.length();
                        out.append(maskedLine).append('\n');
                        linesOut++;
                        java.util.regex.Matcher mm = pattern.matcher(maskedLine);
                        while (mm.find() && spans.size() < MAX_FILTER_HIGHLIGHTS) {
                            spans.add(new IntRange(base + mm.start(), base + mm.end()));
                        }
                    }
                } catch (Throwable ignored) {
                }
                if (linesOut >= MAX_FILTER_OUTPUT_LINES || out.length() >= MAX_FILTER_OUTPUT_CHARS) {
                    break;
                }
            }
        } catch (Throwable t) {
            return new FilterRenderResult("读取失败: " + t.getMessage(), new ArrayList<>(), null);
        }

        String toast = null;
        if (linesOut >= MAX_FILTER_OUTPUT_LINES || out.length() >= MAX_FILTER_OUTPUT_CHARS) {
            toast = "筛选结果已截断（命中行数/内容过多）";
        } else if (SystemClock.uptimeMillis() - startMs > MAX_FILTER_TIME_MS) {
            toast = "筛选已提前停止（耗时超限）";
        } else if (scanTruncated) {
            toast = "仅扫描末尾 " + (MAX_ALL_BYTES / (1024 * 1024)) + "MB（文件过大）";
        }

        if (out.length() == 0) {
            String head = scanTruncated ? ("无匹配结果（仅扫描末尾 " + (MAX_ALL_BYTES / (1024 * 1024)) + "MB）\n") : "无匹配结果\n";
            return new FilterRenderResult(head, new ArrayList<>(), null);
        }
        if (toast != null) {
            out.append("\n[提示] ").append(toast).append('\n');
        }
        return new FilterRenderResult(out.toString(), spans, toast);
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
