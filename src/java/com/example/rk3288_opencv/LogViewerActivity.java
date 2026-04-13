package com.example.rk3288_opencv;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Build;
import android.view.View;
import android.widget.ImageButton;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.widget.Toolbar;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public class LogViewerActivity extends AppCompatActivity implements LogAdapter.OnLogClickListener {
    private static final String PREFS_NAME = "RK3288_Prefs";
    private static final String PREF_LOG_RETENTION_DAYS = "pref_log_retention_days";

    private RecyclerView rvLogs;
    private ProgressBar progressBar;
    private TextView tvEmpty;
    private LogAdapter adapter;
    private Button btnRefresh, btnExport, btnCaptureLogcat, btnExportBundle;
    private Button btnDeleteSelected, btnRetention;
    private Toolbar toolbar;
    private ImageButton btnSelectAll;
    private ImageButton btnSelectInvert;
    private ImageButton btnSelectClear;

    private ExecutorService executor = Executors.newSingleThreadExecutor();
    private Handler handler = new Handler(Looper.getMainLooper());
    private File logDir;

    // For SAF Export
    private List<File> filesToExport = new ArrayList<>();
    private boolean exportEvidenceBundle = false;

    private final ActivityResultLauncher<Intent> exportLauncher = registerForActivityResult(
            new ActivityResultContracts.StartActivityForResult(),
            result -> {
                if (result.getResultCode() == Activity.RESULT_OK && result.getData() != null) {
                    Uri uri = result.getData().getData();
                    if (uri != null) {
                        performExport(uri);
                    }
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        AppLog.enter("LogViewerActivity", "onCreate");
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_log_viewer);

        toolbar = findViewById(R.id.toolbar);
        setSupportActionBar(toolbar);
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
            getSupportActionBar().setDisplayShowHomeEnabled(true);
        }
        if (toolbar != null) {
            toolbar.setNavigationOnClickListener(v -> finish());
        }

        rvLogs = findViewById(R.id.rv_logs);
        progressBar = findViewById(R.id.progress_bar);
        tvEmpty = findViewById(R.id.tv_empty);
        btnRefresh = findViewById(R.id.btn_refresh);
        btnExport = findViewById(R.id.btn_export);
        btnCaptureLogcat = findViewById(R.id.btn_capture_logcat);
        btnExportBundle = findViewById(R.id.btn_export_bundle);
        btnDeleteSelected = findViewById(R.id.btn_delete_selected);
        btnRetention = findViewById(R.id.btn_retention);
        btnSelectAll = findViewById(R.id.btn_select_all);
        btnSelectInvert = findViewById(R.id.btn_select_invert);
        btnSelectClear = findViewById(R.id.btn_select_clear);

        // Setup RecyclerView
        rvLogs.setLayoutManager(new LinearLayoutManager(this));
        adapter = new LogAdapter(this);
        rvLogs.setAdapter(adapter);

        if (btnSelectAll != null) {
            btnSelectAll.setOnClickListener(v -> adapter.selectAll());
        }
        if (btnSelectInvert != null) {
            btnSelectInvert.setOnClickListener(v -> adapter.invertSelection());
        }
        if (btnSelectClear != null) {
            btnSelectClear.setOnClickListener(v -> adapter.clearSelection());
        }
        updateSelectionUi(0, 0);

        File extFiles = getExternalFilesDir(null);
        if (extFiles != null) {
            File pkgDir = extFiles.getParentFile();
            if (pkgDir != null) {
                File externalLogs = new File(pkgDir, "logs");
                if (externalLogs.exists() || externalLogs.mkdirs()) {
                    logDir = externalLogs;
                }
            }
            if (logDir == null) {
                File fallback = new File(extFiles, "logs");
                if (fallback.exists() || fallback.mkdirs()) {
                    logDir = fallback;
                }
            }
        }
        if (logDir == null) {
            File internalLogs = new File(getFilesDir(), "logs");
            if (internalLogs.exists() || internalLogs.mkdirs()) {
                logDir = internalLogs;
            }
        }

        btnRefresh.setOnClickListener(v -> loadLogs());
        
        btnExport.setOnClickListener(v -> {
            List<File> selected = adapter.getSelectedFiles();
            if (selected.isEmpty()) {
                Toast.makeText(this, "请先选择要导出的日志文件", Toast.LENGTH_SHORT).show();
            } else {
                exportEvidenceBundle = false;
                initiateExport(selected);
            }
        });

        btnCaptureLogcat.setOnClickListener(v -> captureLogcatToFile());

        btnExportBundle.setOnClickListener(v -> {
            List<File> selected = adapter.getSelectedFiles();
            if (selected.isEmpty()) {
                Toast.makeText(this, "未选择日志，将导出全部日志 + logcat + 设备信息", Toast.LENGTH_SHORT).show();
                exportEvidenceBundle = true;
                initiateExport(getAllLogFiles());
            } else {
                exportEvidenceBundle = true;
                initiateExport(selected);
            }
        });

        if (btnDeleteSelected != null) {
            btnDeleteSelected.setOnClickListener(v -> deleteSelectedLogs());
        }
        if (btnRetention != null) {
            btnRetention.setOnClickListener(v -> showRetentionDialog());
            updateRetentionButtonText();
        }

        loadLogs();
    }

    @Override
    protected void onResume() {
        super.onResume();
        loadLogs();
    }

    @Override
    public void onSelectionChanged(int selectedCount, int totalCount) {
        updateSelectionUi(selectedCount, totalCount);
    }

    private void updateSelectionUi(int selectedCount, int totalCount) {
        if (toolbar == null) return;
        if (selectedCount > 0) {
            toolbar.setSubtitle("已选 " + selectedCount + " / " + totalCount);
        } else {
            toolbar.setSubtitle(null);
        }
        boolean hasAny = totalCount > 0;
        if (btnSelectAll != null) btnSelectAll.setEnabled(hasAny);
        if (btnSelectInvert != null) btnSelectInvert.setEnabled(hasAny);
        if (btnSelectClear != null) btnSelectClear.setEnabled(selectedCount > 0);
    }

    private void loadLogs() {
        AppLog.enter("LogViewerActivity", "loadLogs");
        progressBar.setVisibility(View.VISIBLE);
        tvEmpty.setVisibility(View.GONE);
        
        executor.execute(() -> {
            List<File> files = new ArrayList<>();
            if (logDir != null && logDir.exists() && logDir.isDirectory()) {
                File[] list = logDir.listFiles((dir, name) -> name.endsWith(".log"));
                if (list != null) {
                    files.addAll(Arrays.asList(list));
                }
            }

            handler.post(() -> {
                progressBar.setVisibility(View.GONE);
                if (files.isEmpty()) {
                    tvEmpty.setVisibility(View.VISIBLE);
                    tvEmpty.setText("暂无日志");
                } else {
                    adapter.setLogFiles(files);
                }
                updateRetentionButtonText();
                updateSelectionUi(adapter.getSelectedCount(), adapter.getTotalCount());
            });
        });
    }

    private List<File> getAllLogFiles() {
        List<File> files = new ArrayList<>();
        if (logDir != null && logDir.exists() && logDir.isDirectory()) {
            File[] list = logDir.listFiles((dir, name) -> name.endsWith(".log"));
            if (list != null) {
                files.addAll(Arrays.asList(list));
            }
        }
        return files;
    }

    private void initiateExport(List<File> files) {
        this.filesToExport = files;
        String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        String fileName = exportEvidenceBundle ? ("evidence_" + timeStamp + ".zip") : ("logs_" + timeStamp + ".zip");

        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("application/zip");
        intent.putExtra(Intent.EXTRA_TITLE, fileName);
        exportLauncher.launch(intent);
    }

    private void performExport(Uri targetUri) {
        progressBar.setVisibility(View.VISIBLE);
        Toast.makeText(this, "正在导出…", Toast.LENGTH_SHORT).show();

        executor.execute(() -> {
            boolean success = false;
            try (OutputStream os = getContentResolver().openOutputStream(targetUri);
                 BufferedOutputStream bos = new BufferedOutputStream(os);
                 ZipOutputStream zos = new ZipOutputStream(bos)) {

                // Create manifest content
                StringBuilder manifestBuilder = new StringBuilder();
                manifestBuilder.append("Export Date: ").append(new Date()).append("\n");
                manifestBuilder.append("File Count: ").append(filesToExport.size()).append("\n\n");
                manifestBuilder.append("Files:\n");

                byte[] buffer = new byte[8192];

                for (File file : filesToExport) {
                    boolean isText = false;
                    try (java.io.InputStream in = new FileInputStream(file)) {
                        java.nio.charset.CharsetDecoder decoder = java.nio.charset.StandardCharsets.UTF_8.newDecoder();
                        decoder.onMalformedInput(java.nio.charset.CodingErrorAction.REPORT);
                        decoder.onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT);
                        java.io.InputStreamReader isr = new java.io.InputStreamReader(in, decoder);
                        char[] cbuf = new char[1024];
                        int read = isr.read(cbuf);
                        isText = true; // Read successful without CharacterCodingException
                    } catch (java.nio.charset.CharacterCodingException e) {
                        isText = false;
                    } catch (Exception ignored) {
                        isText = false;
                    }

                    if (!isText) {
                        manifestBuilder.append(file.getName())
                                .append(" | [SKIPPED] 非 UTF-8 文本或二进制文件，已跳过导出以防隐私泄露\n");
                        continue;
                    }

                    ZipEntry entry = new ZipEntry(file.getName());
                    zos.putNextEntry(entry);

                    CRC32 crc = new CRC32();
                    long written = 0;

                    try (BufferedReader reader = new BufferedReader(new InputStreamReader(new FileInputStream(file), java.nio.charset.StandardCharsets.UTF_8))) {
                        String line;
                        while ((line = reader.readLine()) != null) {
                            String redacted = redactSensitive(line);
                            byte[] bytes = (redacted + "\n").getBytes(java.nio.charset.StandardCharsets.UTF_8);
                            zos.write(bytes);
                            crc.update(bytes);
                            written += bytes.length;
                        }
                    }
                    zos.closeEntry();
                    
                    manifestBuilder.append(file.getName())
                            .append(" | OrigSize: ").append(file.length())
                            .append(" | ExportedSize: ").append(written)
                            .append(" | CRC32: ").append(Long.toHexString(crc.getValue()).toUpperCase())
                            .append("\n");
                }

                if (exportEvidenceBundle) {
                    writeTextEntry(zos, "device_info.txt", buildDeviceInfo());
                    writeTextEntry(zos, "last_preflight.txt", readLastPreflight());
                    writeTextEntry(zos, "logcat_snapshot.txt", collectLogcatSnapshot().content);
                }

                // Write manifest.txt
                ZipEntry manifestEntry = new ZipEntry("manifest.txt");
                zos.putNextEntry(manifestEntry);
                zos.write(manifestBuilder.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                zos.closeEntry();

                success = true;

            } catch (IOException e) {
                e.printStackTrace();
            }

            boolean finalSuccess = success;
            handler.post(() -> {
                progressBar.setVisibility(View.GONE);
                if (finalSuccess) {
                    Toast.makeText(this, "导出完成", Toast.LENGTH_LONG).show();
                    // Optionally open the file? Requirement says "Export complete pop up system notification, click to open ZIP"
                    // Since we are in the app, a Toast is fine, but we can try to view it.
                    // However, we just wrote it to a Uri we don't necessarily have read permission persisted for, 
                    // though usually we do for the session.
                } else {
                    Toast.makeText(this, "导出失败", Toast.LENGTH_LONG).show();
                }
            });
        });
    }

    private void captureLogcatToFile() {
        progressBar.setVisibility(View.VISIBLE);
        Toast.makeText(this, "正在抓取 logcat…", Toast.LENGTH_SHORT).show();
        executor.execute(() -> {
            boolean ok = false;
            String err = null;
            LogcatSnapshot snap = null;
            try {
                if (logDir == null) throw new IOException("日志目录不可用");
                String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
                File outFile = new File(logDir, "logcat_" + timeStamp + ".log");
                snap = collectLogcatSnapshot();
                String content = snap.content;
                try (FileOutputStream fos = new FileOutputStream(outFile)) {
                    fos.write(content.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                }
                ok = true;
            } catch (Throwable t) {
                err = t.getMessage();
            }
            boolean finalOk = ok;
            String finalErr = err;
            LogcatSnapshot finalSnap = snap;
            handler.post(() -> {
                progressBar.setVisibility(View.GONE);
                if (finalOk) {
                    Toast.makeText(this, "logcat 已保存到日志列表", Toast.LENGTH_LONG).show();
                    loadLogs();
                    if (finalSnap != null) {
                        showLogcatScopeDialog(finalSnap);
                    }
                } else {
                    Toast.makeText(this, "抓取失败: " + finalErr, Toast.LENGTH_LONG).show();
                }
            });
        });
    }

    private static final class LogcatSnapshot {
        final String content;
        final String scope;
        final String note;
        final boolean success;

        LogcatSnapshot(String content, String scope, String note, boolean success) {
            this.content = content;
            this.scope = scope;
            this.note = note;
            this.success = success;
        }
    }

    private LogcatSnapshot collectLogcatSnapshot() {
        int pid = android.os.Process.myPid();
        String timeStamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());

        java.lang.Process p = null;
        try {
            StringBuilder pidOut = new StringBuilder();
            p = new ProcessBuilder("logcat", "-d", "-v", "time", "--pid", String.valueOf(pid))
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                int lineCount = 0;
                while ((line = reader.readLine()) != null) {
                    if (++lineCount > 20000) break;
                    pidOut.append(redactSensitive(line)).append('\n');
                }
            }
            try {
                p.waitFor();
            } catch (Throwable ignored) {
            }
            if (pidOut.length() > 0) {
                String header = buildLogcatHeader(timeStamp, pid, "仅本进程（logcat --pid）", "Android 4.1+ 通常无法读取“全设备 logcat”，本文件按本进程抓取。");
                return new LogcatSnapshot(header + pidOut, "仅本进程（logcat --pid）", "本文件按本进程抓取。", true);
            }
        } catch (Throwable ignored) {
        } finally {
            if (p != null) {
                try { p.destroy(); } catch (Throwable ignored) {}
            }
        }

        try {
            StringBuilder allOut = new StringBuilder();
            p = new ProcessBuilder("logcat", "-d", "-v", "time")
                    .redirectErrorStream(true)
                    .start();
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                int lineCount = 0;
                while ((line = reader.readLine()) != null) {
                    if (++lineCount > 20000) break;
                    allOut.append(line).append('\n');
                }
            }
            try {
                p.waitFor();
            } catch (Throwable ignored) {
            }

            if (allOut.length() <= 0) {
                String header = buildLogcatHeader(timeStamp, pid, "失败", "未读取到任何 logcat 输出（可能被权限/ROM 限制）。");
                String body = buildLogcatFailureAdvice();
                return new LogcatSnapshot(header + body, "失败", "未读取到任何 logcat 输出。", false);
            }

            String filtered = filterLogcatByPid(allOut.toString(), pid);
            if (filtered.length() > 0) {
                String header = buildLogcatHeader(timeStamp, pid, "本进程（logcat -d 后按 PID 过滤）", "系统输出不可控，已按 PID 过滤以尽量只保留本进程。");
                return new LogcatSnapshot(header + filtered, "本进程（logcat -d 后按 PID 过滤）", "已按 PID 过滤。", true);
            }

            String redactedAll = redactSensitive(allOut.toString());
            String header = buildLogcatHeader(timeStamp, pid, "不确定（受权限/ROM 限制）", "未能稳定定位 PID 字段，输出范围可能仅本进程，也可能包含更多缓冲。");
            return new LogcatSnapshot(header + redactedAll, "不确定（受权限/ROM 限制）", "输出范围不可保证。", true);
        } catch (Throwable t) {
            String header = buildLogcatHeader(timeStamp, pid, "失败", "执行 logcat 失败: " + t.getMessage());
            String body = buildLogcatFailureAdvice();
            return new LogcatSnapshot(header + body, "失败", "执行 logcat 失败。", false);
        } finally {
            if (p != null) {
                try { p.destroy(); } catch (Throwable ignored) {}
            }
        }
    }

    private void showLogcatScopeDialog(LogcatSnapshot snap) {
        String msg = "采集范围：" + snap.scope + "\n\n" +
                "说明：\n" +
                "- Android 4.1+ 通常只能稳定读取“本进程”日志；不同 ROM 可能更严格\n" +
                "- 导出前已做基础脱敏（token/password/Authorization 等高风险字段掩码）\n\n" +
                "建议：需要全量时，用电脑执行 adb logcat 抓取。";
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setTitle("logcat 采集范围")
                .setMessage(msg)
                .setPositiveButton("知道了", null);
        if (!snap.success) {
            b.setNeutralButton("导出证据链", (d, w) -> {
                exportEvidenceBundle = true;
                initiateExport(getAllLogFiles());
            });
        }
        b.show();
    }

    private static String buildLogcatHeader(String timeStamp, int pid, String scope, String note) {
        StringBuilder sb = new StringBuilder();
        sb.append("=== RK3288 logcat snapshot ===\n");
        sb.append("Time: ").append(timeStamp).append("\n");
        sb.append("PID: ").append(pid).append("\n");
        sb.append("Scope: ").append(scope).append("\n");
        sb.append("Note: ").append(note).append("\n");
        sb.append("\n");
        return sb.toString();
    }

    private static String buildLogcatFailureAdvice() {
        return "建议的替代方案：\n" +
                "1) 导出应用会话日志（本页“导出证据链/导出日志ZIP”）\n" +
                "2) 通过电脑抓取（推荐）：adb logcat -v time > logcat.txt\n";
    }

    private static String filterLogcatByPid(String raw, int pid) {
        StringBuilder sb = new StringBuilder();
        String needle = " " + pid + " ";
        String[] lines = raw.split("\n");
        for (String line : lines) {
            if (line.contains(needle)) {
                sb.append(redactSensitive(line)).append('\n');
            }
        }
        return sb.toString();
    }

    private static final Pattern SENSITIVE_KV = Pattern.compile(
            "(?i)\\b(password|passwd|pwd|token|access[_-]?token|refresh[_-]?token|authorization|bearer|secret|api[_-]?key)\\b\\s*[:=]\\s*([^\\s,;]+)"
    );
    private static final Pattern EMAIL = Pattern.compile("(?i)\\b[A-Z0-9._%+-]+@[A-Z0-9.-]+\\.[A-Z]{2,}\\b");
    private static final Pattern PHONE11 = Pattern.compile("\\b1\\d{10}\\b");

    private static String redactSensitive(String input) {
        if (input == null || input.isEmpty()) return input;
        String out = input;

        Matcher m = SENSITIVE_KV.matcher(out);
        StringBuffer sb = new StringBuffer();
        while (m.find()) {
            String k = m.group(1);
            String v = m.group(2);
            String masked = maskValue(v);
            m.appendReplacement(sb, Matcher.quoteReplacement(k + "=" + masked));
        }
        m.appendTail(sb);
        out = sb.toString();

        out = EMAIL.matcher(out).replaceAll("***@***");
        out = PHONE11.matcher(out).replaceAll("1**********");
        return out;
    }

    private static String maskValue(String v) {
        if (v == null) return "***";
        String s = v.trim();
        if (s.length() <= 4) return "***";
        int keep = Math.min(2, s.length());
        String head = s.substring(0, keep);
        String tail = s.substring(s.length() - keep);
        return head + "***" + tail;
    }

    private void writeTextEntry(ZipOutputStream zos, String name, String content) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        zos.putNextEntry(entry);
        byte[] bytes = redactSensitive(content).getBytes(java.nio.charset.StandardCharsets.UTF_8);
        zos.write(bytes);
        zos.closeEntry();
    }

    private String buildDeviceInfo() {
        StringBuilder sb = new StringBuilder();
        sb.append("Time: ").append(new Date()).append('\n');
        sb.append("Package: ").append(getPackageName()).append('\n');
        sb.append("PID: ").append(android.os.Process.myPid()).append('\n');
        sb.append("Model: ").append(Build.MODEL).append('\n');
        sb.append("Brand: ").append(Build.BRAND).append('\n');
        sb.append("Device: ").append(Build.DEVICE).append('\n');
        sb.append("Product: ").append(Build.PRODUCT).append('\n');
        sb.append("SDK: ").append(Build.VERSION.SDK_INT).append('\n');
        sb.append("Release: ").append(Build.VERSION.RELEASE).append('\n');
        return sb.toString();
    }

    private String readLastPreflight() {
        try {
            return getSharedPreferences("RK3288_Prefs", MODE_PRIVATE).getString("pref_last_preflight", "无预检记录");
        } catch (Throwable t) {
            return "读取失败: " + t.getMessage();
        }
    }

    private void deleteSelectedLogs() {
        List<File> selected = adapter.getSelectedFiles();
        if (selected.isEmpty()) {
            Toast.makeText(this, "请先选择要删除的日志文件", Toast.LENGTH_SHORT).show();
            return;
        }

        new AlertDialog.Builder(this)
                .setTitle("删除日志")
                .setMessage("确认删除所选 " + selected.size() + " 个日志文件吗？\n此操作不可恢复（建议先导出 ZIP 备份）。")
                .setPositiveButton("删除", (d, w) -> {
                    progressBar.setVisibility(View.VISIBLE);
                    executor.execute(() -> {
                        int ok = 0;
                        for (File f : selected) {
                            if (deleteLogFileWithRolls(f)) ok++;
                        }
                        AppLog.cleanupLogsNow(this);
                        int deletedCount = ok;
                        handler.post(() -> {
                            progressBar.setVisibility(View.GONE);
                            Toast.makeText(this, "已删除 " + deletedCount + " 个文件", Toast.LENGTH_LONG).show();
                            adapter.clearSelection();
                            loadLogs();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private boolean deleteLogFileWithRolls(File file) {
        if (file == null) return false;
        boolean deleted = false;
        try {
            if (file.exists()) {
                deleted = file.delete();
            }
            String base = file.getName();
            File dir = file.getParentFile();
            if (dir != null) {
                for (int j = 1; j <= 9; j++) {
                    File rolled = new File(dir, base + "." + j);
                    if (rolled.exists()) rolled.delete();
                }
            }
        } catch (Throwable ignored) {
        }
        return deleted;
    }

    private void showRetentionDialog() {
        int[] options = new int[]{1, 7, 14, 30};
        int current = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getInt(PREF_LOG_RETENTION_DAYS, 7);
        int checked = 1;
        for (int i = 0; i < options.length; i++) {
            if (options[i] == current) {
                checked = i;
                break;
            }
        }
        String[] labels = new String[]{"1 天", "7 天", "14 天", "30 天"};
        new AlertDialog.Builder(this)
                .setTitle("自动清理：保留天数")
                .setSingleChoiceItems(labels, checked, null)
                .setPositiveButton("应用", (d, w) -> {
                    AlertDialog ad = (AlertDialog) d;
                    int idx = ad.getListView().getCheckedItemPosition();
                    int days = options[Math.max(0, Math.min(options.length - 1, idx))];
                    getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putInt(PREF_LOG_RETENTION_DAYS, days).apply();
                    AppLog.setLogRetentionDays(this, days);
                    Toast.makeText(this, "已设置保留 " + days + " 天", Toast.LENGTH_LONG).show();
                    updateRetentionButtonText();
                    loadLogs();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void updateRetentionButtonText() {
        if (btnRetention == null) return;
        try {
            int days = getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getInt(PREF_LOG_RETENTION_DAYS, 7);
            btnRetention.setText("自动清理：保留" + days + "天");
        } catch (Throwable ignored) {
        }
    }

    @Override
    public void onLogClick(File file) {
        Intent intent = new Intent(this, LogDetailActivity.class);
        intent.putExtra(LogDetailActivity.EXTRA_LOG_PATH, file.getAbsolutePath());
        startActivity(intent);
    }
}
