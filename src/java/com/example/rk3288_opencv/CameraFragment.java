package com.example.rk3288_opencv;

import android.content.Context;
import android.content.Intent;
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

    // S-SEC-B09-006: SAF/Mock 导入上限（冻结依据，不得凭空改动）
    // mock 源是替身相机输入（native 侧按 ≤1080p60 解码），典型为几秒~几分钟短视频或图片（数十 MB）。
    // 字节上限 512MiB —— 覆盖最长约 15 分钟 1080p（~4 Mbps ≈ 450MB）的极端合法用例，同时把单次导入对
    //   cache 分区的占用封顶在 512MiB（RK3288 cache 分区通常 ≥1GB），防止无界流灌满分区。
    // 时长上限 60 秒 —— 本地 content:// 读取通常在亚秒~数秒内完成；60s 是宽裕上限，兜底"涓流/停滞"型
    //   provider（读而不止的 DoS 型流），超限受控失败并走既有失败路径清理临时文件。
    public static final long MAX_MOCK_IMPORT_BYTES = 512L * 1024L * 1024L;
    public static final long MAX_MOCK_IMPORT_MILLIS = 60_000L;

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
    private String selectedCameraId = "0";
    private List<CameraInfo> availableCameras = new ArrayList<>();
    private ArrayAdapter<String> cameraAdapter;
    private boolean isSpinnerInitialized = false;
    private String mockFilePath = null;
    private String currentPhotoPath;
    // 拍照输出 URI：授予跨 UID 相机后，消费完成须及时撤销（Q-CQ-B07-009）
    private Uri currentPhotoUri;

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

    // 相机切换时由 MainActivity 同步：getFlipSourceKey 依赖 selectedCameraId，
    // 此前从不更新恒为 "cam_0"，导致翻转预置不随相机切换
    public void setSelectedCameraId(String cameraId) {
        this.selectedCameraId = cameraId;
    }

    public String getFlipSourceKey() {
        boolean wantCamera = (selectedCameraId != null && mockFilePath == null);
        return wantCamera ? ("cam_" + (selectedCameraId != null ? selectedCameraId : "null")) : "mock";
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
                currentPhotoUri = photoURI; // 记录 URI，消费完成后撤销（Q-CQ-B07-009）
                // WHY 单一来源：授权组装（flags + ClipData + EXTRA_OUTPUT）收敛到 PhotoUriGrant.applyTakePictureGrant
                PhotoUriGrant.applyTakePictureGrant(takePictureIntent, photoURI);
                getActivity().startActivityForResult(takePictureIntent, REQUEST_CODE_TAKE_PHOTO);
            }
        } else {
            Toast.makeText(appContext, "No Camera App Found", Toast.LENGTH_SHORT).show();
        }
    }

    private File createImageFile() throws java.io.IOException {
        String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
        String imageFileName = "mock_capture_" + timeStamp + "_";
        File storageDir = new File(appContext.getCacheDir(), "captured_images");
        if (!storageDir.isDirectory() && !storageDir.mkdirs()) {
            throw new java.io.IOException("无法创建拍照缓存目录");
        }
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
        revokePhotoUriPermission(); // 消费完成，及时撤销跨 UID 授权（Q-CQ-B07-009）
    }

    /**
     * 撤销跨 UID 拍照授权：幂等，以与授予一致的掩码回收读写权限，避免授权残留（Q-CQ-B07-009）。
     */
    private void revokePhotoUriPermission() {
        if (currentPhotoUri == null) return;
        try {
            // WHY 单一来源：撤销调用收敛到 PhotoUriGrant.revoke，掩码与授予一致（Q-CQ-B07-009）
            PhotoUriGrant.revoke(appContext, currentPhotoUri);
        } catch (Exception ex) {
            AppLog.e("CameraFragment", "revokePhotoUriPermission", "Revoke failed", ex);
        } finally {
            currentPhotoUri = null; // 无论成败都清空引用，保证只撤销一次
        }
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

    // ---- S-SEC-B09-006: SAF/Mock 导入有界拷贝辅助 ----
    /**
     * 有界 InputStream→OutputStream 拷贝：累计字节上限与总耗时上限，任一超限即受控中断并返回状态。
     * 纯 JVM 可测（Android 单测无模拟器时以 InputStream/模拟时钟验证）。
     * 注意：单次 read() 若被底层 provider 阻塞无法强制中断，超时以每次 read 返回后的 elapsed 判定为准
     *   （Java InputStream 无通用读超时机制；调用方以 1MB 缓冲读取，单次数据量本身有界）。
     */
    public static final class BoundedStreamCopy {
        public enum Status { OK, BYTE_LIMIT, TIME_LIMIT, CANCELLED }

        public static final class Progress {
            public final long bytesCopied;
            public final long elapsedMs;

            Progress(long bytesCopied, long elapsedMs) {
                this.bytesCopied = bytesCopied;
                this.elapsedMs = elapsedMs;
            }
        }

        public static final class Result {
            public final Status status;
            public final long bytesCopied;

            Result(Status status, long bytesCopied) {
                this.status = status;
                this.bytesCopied = bytesCopied;
            }
        }

        /** 取消门：返回 true 表示应中断拷贝。 */
        public interface Cancellation {
            boolean isCancelled();
        }

        /** 单调时钟（毫秒）；生产用 SystemClock::elapsedRealtime，测试用模拟时钟。 */
        public interface Clock {
            long elapsedMs();
        }

        /** 每块进度回调（由调用方自行节流 UI）。 */
        public interface ProgressSink {
            void onProgress(Progress p);
        }

        /** 状态 → 用户可读错误消息；OK 返回 null。上限值取自 CameraFragment 常量，文案与上限保持一致。 */
        public static String messageFor(Status s) {
            if (s == null) return "未知错误";
            switch (s) {
                case OK:
                    return null;
                case BYTE_LIMIT:
                    return "文件超过大小上限（" + (MAX_MOCK_IMPORT_BYTES / (1024L * 1024L)) + "MiB），已中断";
                case TIME_LIMIT:
                    return "导入超时（" + (MAX_MOCK_IMPORT_MILLIS / 1000L) + " 秒），已中断";
                case CANCELLED:
                    return "用户取消";
                default:
                    return "未知错误";
            }
        }

        /**
         * 拷贝 in→out，至多 maxBytes 字节且总耗时不超过 maxMillis；任何情况下写盘不超过 maxBytes。
         * @param buffer    复用缓冲（建议 1MB）
         * @param cancelled 用户取消门（每块判定一次）
         * @param clock     单调时钟（毫秒）；生产传 SystemClock::elapsedRealtime，测试传模拟时钟
         * @param progress  每块进度回调（可空；由调用方自行节流 UI）
         */
        public static Result copy(java.io.InputStream in, java.io.OutputStream out, byte[] buffer,
                                  long maxBytes, long maxMillis,
                                  Cancellation cancelled,
                                  Clock clock,
                                  ProgressSink progress) throws java.io.IOException {
            if (buffer == null || buffer.length == 0) throw new IllegalArgumentException("buffer 不能为空");
            if (maxBytes <= 0) throw new IllegalArgumentException("maxBytes 必须 > 0");
            if (maxMillis <= 0) throw new IllegalArgumentException("maxMillis 必须 > 0");
            if (cancelled == null || clock == null) throw new IllegalArgumentException("cancelled/clock 不能为空");
            long startMs = clock.elapsedMs();
            long copied = 0L;
            int n;
            while ((n = in.read(buffer)) != -1) {
                if (cancelled.isCancelled()) return new Result(Status.CANCELLED, copied);
                if (copied + n > maxBytes) {
                    // 突破字节上限：仅写入到上限为止，随后受控中断（写盘不超过 maxBytes，临时文件由调用方失败路径清理）。
                    int allowed = (int) Math.min((long) buffer.length, maxBytes - copied);
                    if (allowed > 0) out.write(buffer, 0, allowed);
                    copied += allowed;
                    return new Result(Status.BYTE_LIMIT, copied);
                }
                out.write(buffer, 0, n);
                copied += n;
                long nowMs = clock.elapsedMs();
                if (nowMs - startMs > maxMillis) return new Result(Status.TIME_LIMIT, copied);
                if (progress != null) progress.onProgress(new Progress(copied, nowMs - startMs));
            }
            return new Result(Status.OK, copied);
        }
    }
}
