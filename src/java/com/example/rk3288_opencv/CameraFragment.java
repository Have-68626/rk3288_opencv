package com.example.rk3288_opencv;

import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Bundle;
import android.os.SystemClock;
import android.app.ProgressDialog;
import android.provider.MediaStore;
import android.provider.OpenableColumns;
import android.util.Range;
import android.util.Size;
import android.widget.ArrayAdapter;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.content.FileProvider;
import androidx.fragment.app.Fragment;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Camera discovery, state management, and mock file handling.
 * Extracted from MainActivity as preliminary split (P1.7).
 */
public class CameraFragment extends Fragment {

    public interface Callback {
        void onCameraChanged();
        void onInitEngine();
        void onStopMonitoring();
        void onStartMonitoring(long delayMs);
        void onCameraSpinnerInitialized(boolean initialized);
    }

    private Callback callback;
    private Context appContext;

    // Camera state
    public String selectedCameraId = "0";
    public List<CameraInfo> availableCameras = new ArrayList<>();
    public ArrayAdapter<String> cameraAdapter;
    public boolean isSpinnerInitialized = false;
    public String mockFilePath = null;
    public String currentPhotoPath;

    // Flip state
    public boolean flipXEnabled = false;
    public boolean flipYEnabled = false;
    public boolean flipXHasOverride = false;
    public boolean flipYHasOverride = false;

    public static final int REQUEST_CODE_PICK_MEDIA = 2001;
    public static final int REQUEST_CODE_TAKE_PHOTO = 1003;

    // Camera Info Wrapper
    public static class CameraInfo {
        public String id;
        public String description;
        public int facing;

        CameraInfo(String id, String description, int facing) {
            this.id = id;
            this.description = description;
            this.facing = facing;
        }

        @Override
        public String toString() {
            return description;
        }
    }

    @Override
    public void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        appContext = requireContext();
    }

    public void setCallback(Callback cb) {
        this.callback = cb;
    }

    // ---- Camera discovery ----

    public void discoverCameras() {
        AppLog.enter("CameraFragment", "discoverCameras");
        Context ctx = appContext;
        if (ctx == null) return;
        CameraManager manager = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
        availableCameras.clear();
        List<String> displayNames = new ArrayList<>();
        int selectionIndex = 0;

        try {
            if (manager == null) return;
            String[] cameraIds = manager.getCameraIdList();

            for (int i = 0; i < cameraIds.length; i++) {
                String id = cameraIds[i];
                try {
                    CameraCharacteristics chars = manager.getCameraCharacteristics(id);
                    Integer facing = chars.get(CameraCharacteristics.LENS_FACING);

                    String facingStr = "Unknown";
                    if (facing != null) {
                        if (facing == CameraCharacteristics.LENS_FACING_BACK) facingStr = "Back";
                        else if (facing == CameraCharacteristics.LENS_FACING_FRONT) facingStr = "Front";
                        else if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) facingStr = "External/USB";
                    }

                    String desc = String.format("Cam %s (%s)", id, facingStr);
                    availableCameras.add(new CameraInfo(id, desc, facing != null ? facing : -1));
                    displayNames.add(desc);

                    try {
                        if (id.equals(selectedCameraId)) {
                            selectionIndex = i;
                        }
                    } catch (NumberFormatException e) {
                        AppLog.w("CameraFragment", "discoverCameras", "Invalid camera ID: " + id, e);
                    }

                } catch (Exception e) {
                    AppLog.e("CameraFragment", "discoverCameras", "Query camera failed id=" + id, e);
                }
            }
        } catch (Exception e) {
            AppLog.e("CameraFragment", "discoverCameras", "Discover cameras failed", e);
            Toast.makeText(ctx, "Failed to discover cameras: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }

        // Add Mock Option
        availableCameras.add(new CameraInfo("-1", "Mock Source (File Picker)", -1));
        displayNames.add("Mock Source (File Picker)");
        availableCameras.add(new CameraInfo("-2", "Mock Camera (System App)", -1));
        displayNames.add("Mock Camera (System App)");

        if (selectedCameraId == null) {
            selectionIndex = availableCameras.size() - 1;
        }

        int finalSelectionIndex = selectionIndex;
        if (getActivity() != null) {
            getActivity().runOnUiThread(() -> {
                if (cameraAdapter != null) {
                    cameraAdapter.clear();
                    cameraAdapter.addAll(displayNames);
                    cameraAdapter.notifyDataSetChanged();
                }
                if (callback != null) {
                    callback.onCameraSpinnerInitialized(!availableCameras.isEmpty());
                }
            });
        }
    }

    // ---- Camera utilities ----

    public boolean isFrontCamera(String camId) {
        Context ctx = appContext;
        if (ctx == null) return false;
        try {
            CameraManager mgr = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) return false;
            CameraCharacteristics chars = mgr.getCameraCharacteristics(camId);
            Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
            return facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT;
        } catch (Throwable ignored) {
            return false;
        }
    }

    public String getFlipSourceKey() {
        boolean wantCamera = (selectedCameraId != null && mockFilePath == null);
        return wantCamera ? ("cam_" + (selectedCameraId != null ? selectedCameraId : "null")) : "mock";
    }

    public void refreshFlipFromPrefs(boolean applyToNativeIfReady, String prefsName,
                                     boolean flipXEnabledRef, boolean flipYEnabledRef,
                                     boolean flipXHasOverrideRef, boolean flipYHasOverrideRef) {
        Context ctx = appContext;
        if (ctx == null) return;
        SharedPreferences prefs = ctx.getSharedPreferences(prefsName, Context.MODE_PRIVATE);
        String sourceKey = getFlipSourceKey();

        boolean defaultX = false;
        if (selectedCameraId != null && mockFilePath == null) {
            defaultX = isFrontCamera(selectedCameraId);
        }
        boolean hasX = prefs.contains("pref_flip_x_" + sourceKey);
        boolean hasY = prefs.contains("pref_flip_y_" + sourceKey);
        boolean nextX = hasX ? prefs.getBoolean("pref_flip_x_" + sourceKey, defaultX) : defaultX;
        boolean nextY = hasY ? prefs.getBoolean("pref_flip_y_" + sourceKey, false) : false;

        flipXHasOverride = hasX;
        flipYHasOverride = hasY;
        flipXEnabled = nextX;
        flipYEnabled = nextY;
    }

    // ---- Preflight ----

    public String runCameraPreflight() {
        Context ctx = appContext;
        if (ctx == null) return "相机预检失败：Context 不可用";
        try {
            CameraManager mgr = (CameraManager) ctx.getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) return "相机预检失败：CameraManager 不可用";
            CameraCharacteristics chars = mgr.getCameraCharacteristics(selectedCameraId);
            StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes = map != null ? map.getOutputSizes(ImageFormat.YUV_420_888) : null;
            Range<Integer>[] fpsRanges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);

            Size chosen = chooseBestSizeNoMoreThan1080p(sizes);
            Range<Integer> fps = chooseBestFpsNoMoreThan60(fpsRanges);

            StringBuilder sb = new StringBuilder();
            sb.append("相机预检: Cam ").append(selectedCameraId);
            sb.append("\n输出: ").append(chosen == null ? "未知" : (chosen.getWidth() + "x" + chosen.getHeight()));
            sb.append("\n帧率: ").append(fps == null ? "未知" : (fps.getLower() + "-" + fps.getUpper()));
            sb.append("\n策略: 输入≤1080p60；推理固定640x480");
            return sb.toString();
        } catch (Throwable t) {
            return "相机预检失败: " + t.getMessage();
        }
    }

    public String runMockPreflight() {
        try {
            String path = mockFilePath;
            if (path == null || path.isEmpty()) return "Mock预检失败：未选择输入";
            if (path.startsWith("http://") || path.startsWith("https://")) {
                return "Mock预检: URL 源（无法预读元数据，运行时将自动降档到≤1080p60，推理640x480）";
            }

            File f = new File(path);
            if (!f.exists()) return "Mock预检: 文件不存在";

            Integer w = null;
            Integer h = null;
            Integer rot = null;
            Integer bitrate = null;
            Float fps = null;

            try {
                MediaMetadataRetriever mmr = new MediaMetadataRetriever();
                mmr.setDataSource(f.getAbsolutePath());
                String sw = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH);
                String sh = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT);
                String sr = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
                String sb = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_BITRATE);
                String sfps = mmr.extractMetadata(MediaMetadataRetriever.METADATA_KEY_CAPTURE_FRAMERATE);
                if (sw != null) w = Integer.parseInt(sw);
                if (sh != null) h = Integer.parseInt(sh);
                if (sr != null) rot = Integer.parseInt(sr);
                if (sb != null) bitrate = Integer.parseInt(sb);
                if (sfps != null) fps = Float.parseFloat(sfps);
                mmr.release();
            } catch (Throwable t) {
                AppLog.w("MockMode", "runMockPreflight", "MediaMetadataRetriever failed: " + t.getMessage());
            }

            if (w == null || h == null) {
                try {
                    BitmapFactory.Options opt = new BitmapFactory.Options();
                    opt.inJustDecodeBounds = true;
                    BitmapFactory.decodeFile(f.getAbsolutePath(), opt);
                    if (opt.outWidth > 0 && opt.outHeight > 0) {
                        w = opt.outWidth;
                        h = opt.outHeight;
                    }
                } catch (Throwable t) {
                    AppLog.w("MockMode", "runMockPreflight", "BitmapFactory decodeBounds failed: " + t.getMessage());
                }
            }

            boolean oversize = (w != null && h != null) && (Math.max(w, h) > 1920 || Math.min(w, h) > 1080);
            boolean overFps = fps != null && fps > 60.5f;

            StringBuilder sb = new StringBuilder();
            sb.append("Mock预检: ").append(f.getName());
            sb.append("\n尺寸: ").append(w == null ? "未知" : (w + "x" + h));
            if (rot != null) sb.append(" rot=").append(rot);
            if (fps != null) sb.append("\n帧率: ").append(String.format(Locale.US, "%.2f", fps));
            if (bitrate != null) sb.append("\n码率: ").append(bitrate);
            if (oversize || overFps) {
                sb.append("\n提示: 输入超规格，将自动降档到≤1080p60，推理640x480");
            } else {
                sb.append("\n策略: 输入≤1080p60；推理固定640x480");
            }
            return sb.toString();
        } catch (Throwable t) {
            return "Mock预检失败: " + t.getMessage();
        }
    }

    // ---- Size/Fps selection ----

    public static Size chooseBestSizeNoMoreThan1080p(Size[] sizes) {
        if (sizes == null || sizes.length == 0) return null;
        Size best = null;
        long bestArea = -1;
        for (Size s : sizes) {
            if (s == null) continue;
            int w = s.getWidth();
            int h = s.getHeight();
            boolean ok = (w <= 1920 && h <= 1080) || (w <= 1080 && h <= 1920);
            if (!ok) continue;
            long area = (long) w * (long) h;
            if (area > bestArea) {
                bestArea = area;
                best = s;
            }
        }
        if (best != null) return best;
        for (Size s : sizes) {
            if (s == null) continue;
            int w = s.getWidth();
            int h = s.getHeight();
            long area = (long) w * (long) h;
            if (area > bestArea) {
                bestArea = area;
                best = s;
            }
        }
        return best;
    }

    public static Range<Integer> chooseBestFpsNoMoreThan60(Range<Integer>[] ranges) {
        if (ranges == null || ranges.length == 0) return null;
        Range<Integer> best = null;
        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            Integer lower = r.getLower();
            Integer upper = r.getUpper();
            if (lower == null || upper == null) continue;
            if (upper > 60) continue;
            if (best == null) {
                best = r;
                continue;
            }
            int bestUpper = best.getUpper();
            int bestLower = best.getLower();
            if (upper > bestUpper) {
                best = r;
            } else if (upper == bestUpper && lower < bestLower) {
                best = r;
            }
        }
        if (best != null) return best;
        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            Integer upper = r.getUpper();
            if (upper != null && upper <= 60) return r;
        }
        return ranges[0];
    }

    // ---- Mock file handling ----

    public void pickMediaFile() {
        if (getActivity() == null) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        String[] mimetypes = {"image/*", "video/*"};
        intent.putExtra(Intent.EXTRA_MIME_TYPES, mimetypes);
        getActivity().startActivityForResult(intent, REQUEST_CODE_PICK_MEDIA);
    }

    public void dispatchTakePictureIntent() {
        if (getActivity() == null) return;
        Intent takePictureIntent = new Intent(MediaStore.ACTION_IMAGE_CAPTURE);
        if (takePictureIntent.resolveActivity(appContext.getPackageManager()) != null) {
            File photoFile = null;
            try {
                photoFile = createImageFile();
            } catch (java.io.IOException ex) {
                Toast.makeText(appContext, "Error creating file", Toast.LENGTH_SHORT).show();
                AppLog.e("CameraFragment", "dispatchTakePictureIntent", "Create file failed", ex);
            }
            if (photoFile != null) {
                Uri photoURI = FileProvider.getUriForFile(appContext,
                        BuildConfig.APPLICATION_ID + ".fileprovider",
                        photoFile);
                takePictureIntent.putExtra(MediaStore.EXTRA_OUTPUT, photoURI);
                getActivity().startActivityForResult(takePictureIntent, REQUEST_CODE_TAKE_PHOTO);
            }
        } else {
            Toast.makeText(appContext, "No Camera App Found", Toast.LENGTH_SHORT).show();
        }
    }

    private File createImageFile() throws java.io.IOException {
        String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        String imageFileName = "mock_capture_" + timeStamp + "_";
        File storageDir = appContext.getCacheDir();
        File image = File.createTempFile(
            imageFileName,
            ".jpg",
            storageDir
        );
        currentPhotoPath = image.getAbsolutePath();
        return image;
    }

    public void handleSystemCameraResult() {
        if (currentPhotoPath == null) return;
        mockFilePath = currentPhotoPath;
        selectedCameraId = null; // Mock Flag
        AppLog.i("CameraFragment", "handleSystemCameraResult", "Photo captured: " + mockFilePath);
        Toast.makeText(appContext, "Photo Captured", Toast.LENGTH_SHORT).show();
        if (callback != null) {
            callback.onCameraChanged();
        }
    }

    public void handleMockFileSelection(Uri uri) {
        if (uri == null) return;
        Context ctx = appContext;
        if (ctx == null) return;

        String direct = null;
        try {
            if ("file".equalsIgnoreCase(uri.getScheme())) {
                File f = new File(uri.getPath());
                if (f.exists() && f.isFile() && f.canRead()) {
                    direct = f.getAbsolutePath();
                }
            }
        } catch (Throwable ignored) {
        }

        if (direct != null && !direct.isEmpty()) {
            mockFilePath = direct;
            selectedCameraId = null;
            isSpinnerInitialized = true;
            if (callback != null) callback.onCameraChanged();
            return;
        }

        if (getActivity() == null) return;
        ProgressDialog progressDialog = new ProgressDialog(getActivity());
        progressDialog.setTitle("加载 Mock 文件");
        progressDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        progressDialog.setIndeterminate(true);
        progressDialog.setCancelable(true);
        progressDialog.setMessage("准备读取…");
        progressDialog.show();

        final java.util.concurrent.atomic.AtomicBoolean cancelled = new java.util.concurrent.atomic.AtomicBoolean(false);
        progressDialog.setOnCancelListener(d -> cancelled.set(true));

        new Thread(() -> {
            try {
                String displayName = "mock_source";
                long totalBytes = -1L;
                try {
                    Cursor c = ctx.getContentResolver().query(uri, null, null, null, null);
                    if (c != null) {
                        try {
                            if (c.moveToFirst()) {
                                int nameIndex = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                                if (nameIndex >= 0) {
                                    String n = c.getString(nameIndex);
                                    if (n != null && !n.trim().isEmpty()) displayName = n.trim();
                                }
                                int sizeIndex = c.getColumnIndex(OpenableColumns.SIZE);
                                if (sizeIndex >= 0) {
                                    long s = c.getLong(sizeIndex);
                                    if (s > 0) totalBytes = s;
                                }
                            }
                        } finally {
                            try { c.close(); } catch (Throwable ignored) {}
                        }
                    }
                } catch (Throwable ignored) {
                }

                String ext = "";
                int dot = displayName.lastIndexOf('.');
                if (dot >= 0 && dot < displayName.length() - 1) {
                    ext = displayName.substring(dot + 1);
                }
                ext = sanitizeExtension(ext);

                // Magic number pre-check
                String magicCheckErr = null;
                try {
                    java.io.InputStream magicIs = ctx.getContentResolver().openInputStream(uri);
                    if (magicIs != null) {
                        try {
                            byte[] magic = new byte[16];
                            int magicN = magicIs.read(magic);
                            String hexMagic = bytesToHex(magic, Math.max(0, magicN));
                            AppLog.i("MockMode", "handleMockFileSelection", "Magic bytes: " + hexMagic + " ext=" + ext);
                            if (!isValidMagicNumber(magic, magicN, ext)) {
                                magicCheckErr = "不支持的文件格式或文件已损坏（魔数校验失败）";
                            } else {
                                AppLog.i("MockMode", "handleMockFileSelection",
                                    "Magic check PASSED: " + hexMagic + " ext=" + ext);
                            }
                        } finally {
                            try { magicIs.close(); } catch (Throwable ignored) {}
                        }
                    }
                } catch (Throwable t) {
                    AppLog.w("MockMode", "handleMockFileSelection", "Magic read error: " + t.getMessage());
                }
                if (magicCheckErr != null) {
                    String finalErrMsg = magicCheckErr;
                    if (getActivity() != null) {
                        getActivity().runOnUiThread(() -> {
                            try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                            Toast.makeText(ctx, finalErrMsg, Toast.LENGTH_LONG).show();
                        });
                    }
                    return;
                }

                File root = null;
                try {
                    root = ctx.getExternalCacheDir();
                } catch (Throwable ignored) {
                }
                if (root == null) root = ctx.getCacheDir();

                String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
                String saveName = "mock_source_" + timeStamp + (ext.isEmpty() ? ".dat" : ("." + ext));
                File outFile = new File(root, saveName);

                long copied = 0L;
                long copyStartMs = SystemClock.elapsedRealtime();
                long lastUiMs = 0L;
                boolean ok = false;
                String err = null;

                java.io.InputStream is = null;
                java.io.BufferedInputStream bis = null;
                java.io.FileOutputStream fos = null;
                try {
                    is = ctx.getContentResolver().openInputStream(uri);
                    if (is == null) throw new java.io.IOException("无法打开输入流");
                    bis = new java.io.BufferedInputStream(is, 1024 * 1024);
                    fos = new java.io.FileOutputStream(outFile);

                    if (totalBytes > 0) {
                        final int maxKb = (int) Math.min(Integer.MAX_VALUE, Math.max(1L, totalBytes / 1024L));
                        int finalMaxKb = maxKb;
                        if (getActivity() != null) {
                            getActivity().runOnUiThread(() -> {
                                progressDialog.setIndeterminate(false);
                                progressDialog.setMax(finalMaxKb);
                            });
                        }
                    }

                    byte[] buffer = new byte[1024 * 1024];
                    int n;
                    while ((n = bis.read(buffer)) != -1) {
                        if (cancelled.get()) {
                            err = "用户取消";
                            break;
                        }
                        fos.write(buffer, 0, n);
                        copied += n;

                        long nowMs = SystemClock.elapsedRealtime();
                        if (nowMs - lastUiMs >= 350) {
                            lastUiMs = nowMs;
                            long elapsedMs = Math.max(1L, nowMs - copyStartMs);
                            double speed = (copied / 1024.0) / (elapsedMs / 1000.0);
                            String msg = "已复制 " + formatBytes(copied) +
                                    (totalBytes > 0 ? (" / " + formatBytes(totalBytes)) : "") +
                                    "  速度 " + String.format(Locale.US, "%.1f", speed) + " KB/s";
                            int progressKb = (int) Math.min(Integer.MAX_VALUE, Math.max(0L, copied / 1024L));
                            String finalMsg = msg;
                            int finalProgressKb = progressKb;
                            if (getActivity() != null) {
                                getActivity().runOnUiThread(() -> {
                                    try {
                                        progressDialog.setMessage(finalMsg);
                                        if (!progressDialog.isIndeterminate()) {
                                            progressDialog.setProgress(finalProgressKb);
                                        }
                                    } catch (Throwable ignored) {}
                                });
                            }
                        }
                    }

                    fos.flush();
                    if (!cancelled.get() && (err == null || err.isEmpty())) {
                        ok = true;
                    }
                } catch (Throwable t) {
                    err = t.getMessage();
                } finally {
                    try { if (fos != null) fos.close(); } catch (Throwable ignored) {}
                    try { if (bis != null) bis.close(); } catch (Throwable ignored) {}
                    try { if (is != null) is.close(); } catch (Throwable ignored) {}
                }

                long elapsedMs = Math.max(1L, SystemClock.elapsedRealtime() - copyStartMs);

                boolean finalOk = ok;
                String finalErr = err;
                long finalCopied = copied;
                String finalDisplayName = displayName;
                String finalOutPath = outFile.getAbsolutePath();
                if (getActivity() != null) {
                    getActivity().runOnUiThread(() -> {
                        try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                        if (!finalOk) {
                            try { if (outFile.exists()) outFile.delete(); } catch (Throwable ignored) {}
                            Toast.makeText(ctx, "加载失败: " + finalErr, Toast.LENGTH_LONG).show();
                            return;
                        }
                        mockFilePath = finalOutPath;
                        selectedCameraId = null;
                        isSpinnerInitialized = true;
                        Toast.makeText(ctx, "Mock 源已就绪: " + finalDisplayName, Toast.LENGTH_SHORT).show();
                        if (callback != null) callback.onCameraChanged();
                    });
                }
            } catch (Throwable t) {
                AppLog.e("CameraFragment", "handleMockFileSelection", "未捕获异常", t);
                if (getActivity() != null) {
                    getActivity().runOnUiThread(() -> {
                        try { progressDialog.dismiss(); } catch (Throwable ignored) {}
                        Toast.makeText(ctx, "加载文件遇到严重异常: " + t.getMessage(), Toast.LENGTH_LONG).show();
                    });
                }
            }
        }).start();
    }

    // ---- Utility helpers ----

    public static String sanitizeExtension(String ext) {
        if (ext == null) return "";
        String s = ext.trim();
        if (s.isEmpty()) return "";
        if (s.length() > 10) s = s.substring(0, 10);
        if (!s.matches("^[A-Za-z0-9]+$")) return "";
        return s.toLowerCase(Locale.ROOT);
    }

    public static String bytesToHex(byte[] bytes, int len) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < len && i < bytes.length; i++) {
            sb.append(String.format(Locale.US, "%02X", bytes[i] & 0xFF));
            if ((i + 1) % 4 == 0 && i < len - 1) sb.append(' ');
        }
        return sb.toString();
    }

    public static boolean isValidMagicNumber(byte[] magic, int len, String ext) {
        if (len < 2) return false;
        if (len >= 3 && (magic[0] & 0xFF) == 0xFF && (magic[1] & 0xFF) == 0xD8 && (magic[2] & 0xFF) == 0xFF) return true;
        if (len >= 4 && (magic[0] & 0xFF) == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) return true;
        if (len >= 2 && magic[0] == 0x42 && magic[1] == 0x4D) return true;
        if (len >= 12 && magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46
            && magic[8] == 0x57 && magic[9] == 0x45 && magic[10] == 0x42 && magic[11] == 0x50) return true;
        if (len >= 8 && magic[4] == 0x66 && magic[5] == 0x74 && magic[6] == 0x79 && magic[7] == 0x70) return true;
        if (len >= 12 && magic[0] == 0x52 && magic[1] == 0x49 && magic[2] == 0x46 && magic[3] == 0x46
            && magic[8] == 0x41 && magic[9] == 0x56 && magic[10] == 0x49 && magic[11] == 0x20) return true;
        String knownExts = "jpg,jpeg,png,bmp,webp,mp4,m4v,avi,mov,3gp,mkv,wmv,flv,dat";
        return knownExts.contains(ext);
    }

    public static String formatBytes(long bytes) {
        if (bytes < 0) return "--";
        double b = bytes;
        if (b < 1024) return String.format(Locale.US, "%.0fB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(Locale.US, "%.1fKB", b);
        b /= 1024.0;
        if (b < 1024) return String.format(Locale.US, "%.1fMB", b);
        b /= 1024.0;
        return String.format(Locale.US, "%.2fGB", b);
    }
}
