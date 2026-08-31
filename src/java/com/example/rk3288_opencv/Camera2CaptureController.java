package com.example.rk3288_opencv;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Range;
import android.util.Size;
import android.view.Surface;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;

// Q-CQ-B07-005/006: 非 final 以允许单测子类覆盖 startSync 驱动启动失败回滚路径
class Camera2CaptureController implements CaptureController {
    private static final int MAX_W = 1920;
    private static final int MAX_H = 1080;
    private final Context appContext;
    private final ExternalFrameSink sink;
    private final CaptureObserver observer;
    private final RotationDegreesProvider deviceRotationDegrees;

    private HandlerThread thread;
    private Handler handler;

    private CameraDevice cameraDevice;
    private CameraCaptureSession session;
    private ImageReader imageReader;
    private CameraDevice.StateCallback currentStateCallback;

    // Q-CQ-B07-005: 代次隔离 —— stop/start 递增代次，旧代次回调到达时按代次丢弃，仅释放自身参数资源
    // 任务 2 归一：代次语义收敛到统一组件 CaptureSourceEpochGate（long epoch，跨线程 volatile）
    // 包内可见（final）：JVM 单测在包内直接驱动 enter/advance 编排代次，属测试接缝
    final CaptureSourceEpochGate epochGate = new CaptureSourceEpochGate();
    // Q-CQ-B07-005: 帧监听器创建时绑定的 epoch（startSync 内设置）
    private volatile long listenerEpoch;

    private int sensorOrientation;
    private boolean mirrored;
    private CameraCharacteristics chars;
    private long lastStatsRealtimeMs;
    private long lastFrameTsNs;
    private int statsFrames;
    private int statsPushOk;
    private int statsPushFail;
    private long statsAnalyzeNsSum;
    private long statsAnalyzeNsMax;
    private int statsApproxDropped;

    Camera2CaptureController(@Nullable Context context,
                             @NonNull ExternalFrameSink sink,
                             @NonNull CaptureObserver observer,
                             @NonNull RotationDegreesProvider deviceRotationDegrees) {
        // Q-CQ-B07-005/006: 允许 null Context（JVM 单测无法创建 Context），生产路径行为不变
        this.appContext = context == null ? null : context.getApplicationContext();
        this.sink = Objects.requireNonNull(sink);
        this.observer = Objects.requireNonNull(observer);
        this.deviceRotationDegrees = Objects.requireNonNull(deviceRotationDegrees);
    }

    @Override
    public String name() {
        return "Camera2";
    }

    @Override
    public long currentSourceEpoch() {
        return epochGate.current();
    }

    @Override
    @SuppressLint("MissingPermission")
    public boolean start(String cameraId) {
        stop();
        // Q-CQ-B07-005: 每次启动递增 epoch —— 新回调绑定新 epoch，旧回调按 epoch 丢弃
        long epoch = epochGate.enter();
        try {
            return startSync(cameraId, epoch);
        } catch (SecurityException se) {
            // Q-CQ-B07-006: 同步启动异常 —— 完整回滚已预建资源后再上报
            rollbackStart();
            observer.onError("camera2", "缺少相机权限");
            return false;
        } catch (CameraAccessException e) {
            rollbackStart();
            observer.onError("camera2", "CameraAccessException: " + e.getMessage());
            return false;
        } catch (Exception e) {
            rollbackStart();
            observer.onError("camera2", "启动失败: " + e.getMessage());
            return false;
        }
    }

    @Override
    public void stop() {
        // Q-CQ-B07-005: 停止也递增 epoch —— 停止后到达的旧回调一律按旧 epoch 丢弃
        epochGate.advance();
        if (handler != null) {
            handler.removeCallbacksAndMessages(null);
        }
        // Q-CQ-B07-005/006: 停止与启动失败回滚共用同一完整释放路径
        rollbackStart();
    }

    // Q-CQ-B07-006: 同步启动分阶段预建资源；任何一步抛异常由 start() 的 rollbackStart() 完整回滚
    @SuppressLint("MissingPermission") // CAMERA 权限由调用方（MainActivity/CameraFragment）授予后调用
    boolean startSync(String cameraId, long epoch) throws CameraAccessException {
        thread = new HandlerThread("Camera2Capture");
        thread.start();
        handler = new Handler(thread.getLooper());

        CameraManager mgr = (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
        if (mgr == null) {
            // Q-CQ-B07-006: 前置失败同样回滚已预建的 thread/handler
            rollbackStart();
            observer.onError("camera2", "CameraManager 为空");
            return false;
        }
        CameraCharacteristics chars = mgr.getCameraCharacteristics(cameraId);
        this.chars = chars;
        Integer so = chars.get(CameraCharacteristics.SENSOR_ORIENTATION);
        sensorOrientation = so == null ? 0 : so;
        Integer facing = chars.get(CameraCharacteristics.LENS_FACING);
        mirrored = facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT;

        Size size = chooseYuvSize(chars);
        imageReader = ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, 2);
        imageReader.setOnImageAvailableListener(this::onImageAvailable, handler);
        // Q-CQ-B07-005: 帧监听绑定当前 epoch
        listenerEpoch = epoch;
        lastStatsRealtimeMs = SystemClock.elapsedRealtime();
        lastFrameTsNs = 0L;
        statsFrames = 0;
        statsPushOk = 0;
        statsPushFail = 0;
        statsAnalyzeNsSum = 0L;
        statsAnalyzeNsMax = 0L;
        statsApproxDropped = 0;

        currentStateCallback = new GenerationStateCallback(epoch);
        mgr.openCamera(cameraId, currentStateCallback, handler);
        return true;
    }

    // Q-CQ-B07-006: 启动失败回滚 —— reader/thread/device/session 全部释放并置空
    private void rollbackStart() {
        if (session != null) {
            try {
                session.stopRepeating();
            } catch (Exception ignored) {
            }
        }
        releaseCameraSession(session);
        releaseCameraDevice(cameraDevice);
        releaseImageReader(imageReader);
        releaseCaptureThread(thread);
        session = null;
        cameraDevice = null;
        imageReader = null;
        thread = null;
        handler = null;
        chars = null;
        currentStateCallback = null;
    }

    private static void closeQuietly(AutoCloseable r) {
        try {
            if (r != null) {
                r.close();
            }
        } catch (Exception ignored) {
        }
    }

    private static void quitQuietly(HandlerThread t) {
        try {
            if (t != null) {
                t.quitSafely();
            }
        } catch (Exception ignored) {
        }
    }

    // Q-CQ-B07-005/006 测试接缝：资源释放出口。
    // JVM 单测无法构造 CameraDevice/CameraCaptureSession/ImageReader 等框架对象，
    // 测试子类覆盖这些方法以断言各释放路径被调用；生产实现仅委托 closeQuietly/quitQuietly。
    void releaseCameraSession(CameraCaptureSession session) {
        closeQuietly(session);
    }

    void releaseCameraDevice(CameraDevice cameraDevice) {
        closeQuietly(cameraDevice);
    }

    void releaseImageReader(ImageReader imageReader) {
        closeQuietly(imageReader);
    }

    void releaseCaptureThread(HandlerThread thread) {
        quitQuietly(thread);
    }

    private void onImageAvailable(ImageReader reader) {
        // Q-CQ-B07-005: 帧回调绑定启动 epoch —— stop/start 后旧 epoch 帧直接丢弃，不推送不统计
        if (!epochGate.isCurrent(listenerEpoch)) {
            return;
        }
        Image image = null;
        long startNs = SystemClock.elapsedRealtimeNanos();
        try {
            image = reader.acquireLatestImage();
            if (image == null) return;

            Image.Plane[] planes = image.getPlanes();
            if (planes == null || planes.length < 3) {
                observer.onFramePushed(false, image.getTimestamp());
                return;
            }

            int rotation = computeRotationDegrees();
            boolean ok = sink.onYuv420888Frame(
                    planes[0].getBuffer(), planes[0].getRowStride(),
                    planes[1].getBuffer(), planes[1].getRowStride(), planes[1].getPixelStride(),
                    planes[2].getBuffer(), planes[2].getRowStride(), planes[2].getPixelStride(),
                    image.getWidth(), image.getHeight(),
                    image.getTimestamp(), rotation, mirrored);
            observer.onFramePushed(ok, image.getTimestamp());
            statsPushOk += ok ? 1 : 0;
            statsPushFail += ok ? 0 : 1;
            long ts = image.getTimestamp();
            if (lastFrameTsNs > 0 && ts > lastFrameTsNs) {
                long deltaNs = ts - lastFrameTsNs;
                if (deltaNs >= 50_000_000L) {
                    int approx = (int) Math.max(0L, (deltaNs / 16_666_666L) - 1L);
                    statsApproxDropped = Math.min(1_000_000, statsApproxDropped + approx);
                }
            }
            lastFrameTsNs = ts;
        } catch (Exception e) {
            long ts = image != null ? image.getTimestamp() : 0L;
            observer.onFramePushed(false, ts);
        } finally {
            long costNs = SystemClock.elapsedRealtimeNanos() - startNs;
            statsFrames++;
            statsAnalyzeNsSum += costNs;
            statsAnalyzeNsMax = Math.max(statsAnalyzeNsMax, costNs);
            long nowMs = SystemClock.elapsedRealtime();
            if (nowMs - lastStatsRealtimeMs >= 1000) {
                long avgUs = statsFrames <= 0 ? 0L : (statsAnalyzeNsSum / statsFrames) / 1000L;
                long maxUs = statsAnalyzeNsMax / 1000L;
                AppLog.i(
                        "Camera2Capture",
                        "stats",
                        "acquire=LATEST fmt=YUV_420_888 frames=" + statsFrames +
                                " pushOk=" + statsPushOk +
                                " pushFail=" + statsPushFail +
                                " approxDrop=" + statsApproxDropped +
                                " analyzeAvg=" + avgUs + "us" +
                                " analyzeMax=" + maxUs + "us"
                );
                lastStatsRealtimeMs = nowMs;
                statsFrames = 0;
                statsPushOk = 0;
                statsPushFail = 0;
                statsAnalyzeNsSum = 0L;
                statsAnalyzeNsMax = 0L;
                statsApproxDropped = 0;
            }
            if (image != null) {
                try {
                    image.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private int computeRotationDegrees() {
        int device = deviceRotationDegrees.getRotationDegrees();
        int sensor = sensorOrientation;
        int rot;
        if (mirrored) {
            rot = (sensor + device) % 360;
        } else {
            rot = (sensor - device + 360) % 360;
        }
        return rot;
    }

    private Size chooseYuvSize(CameraCharacteristics chars) {
        StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map == null) return new Size(640, 480);
        Size[] sizes = map.getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null || sizes.length == 0) return new Size(640, 480);

        Size preferred = null;
        for (Size s : sizes) {
            if (s.getWidth() == 640 && s.getHeight() == 480) {
                preferred = s;
                break;
            }
        }
        if (preferred != null) return preferred;

        List<Size> list = new ArrayList<>(Arrays.asList(sizes));
        Collections.sort(list, new Comparator<Size>() {
            @Override
            public int compare(Size a, Size b) {
                int aa = a.getWidth() * a.getHeight();
                int bb = b.getWidth() * b.getHeight();
                return Integer.compare(aa, bb);
            }
        });
        for (Size s : list) {
            if (s.getWidth() == 1280 && s.getHeight() == 720) return s;
        }
        for (Size s : list) {
            if (s.getWidth() >= 640 && s.getHeight() >= 480) return s;
        }
        return list.get(0);
    }

    private Range<Integer> chooseFpsRange() {
        if (chars == null) return null;
        Range<Integer>[] ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null || ranges.length == 0) return null;

        Range<Integer> best = null;
        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            int u = r.getUpper();
            if (u < 30 || u > 60) continue;
            if (best == null) {
                best = r;
                continue;
            }
            int bu = best.getUpper();
            if (u < bu) {
                best = r;
            } else if (u == bu && r.getLower() > best.getLower()) {
                best = r;
            }
        }
        if (best != null) return best;

        for (Range<Integer> r : ranges) {
            if (r == null) continue;
            int u = r.getUpper();
            if (u <= 60) {
                if (best == null || u > best.getUpper()) best = r;
            }
        }
        return best;
    }

    // Q-CQ-B07-005: 代次隔离 —— 每个 start 新建一个绑定当前代次的状态回调；
    // 旧代次回调在 stop/start 后到达时，仅释放参数 camera，不触碰当前会话状态
    final class GenerationStateCallback extends CameraDevice.StateCallback {
        private final long boundEpoch;

        GenerationStateCallback(long boundEpoch) {
            this.boundEpoch = boundEpoch;
        }

        @Override
        public void onOpened(@NonNull CameraDevice camera) {
            if (!epochGate.isCurrent(boundEpoch)) {
                releaseCameraDevice(camera);
                return;
            }
            if (imageReader == null) {
                releaseCameraDevice(camera);
                return;
            }
            cameraDevice = camera;
            // Q-CQ-B07-008: 会话创建主体提取为包内方法 —— JVM 单测以 null 框架参数直接驱动异常路径
            handleSessionCreate(camera);
        }

        // Q-CQ-B07-008: onOpened 会话创建主体 —— 提取自 onOpened，生产行为与内联等价
        void handleSessionCreate(@NonNull CameraDevice camera) {
            try {
                Surface surface = imageReader.getSurface();
                CaptureRequest.Builder builder = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                builder.addTarget(surface);
                List<Surface> surfaces = Arrays.asList(surface);
                camera.createCaptureSession(surfaces, new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(@NonNull CameraCaptureSession s) {
                        handleSessionConfigured(s, builder);
                    }

                    @Override
                    public void onConfigureFailed(@NonNull CameraCaptureSession s) {
                        handleSessionConfigureFailed(s);
                    }
                }, handler);
            } catch (Exception e) {
                // Q-CQ-B07-008 WHY: 修复前 catch 仅上报错误，cameraDevice 已在 try 前置位、
                // reader/thread 存活 —— 相机停在"打开但无会话无帧"的半死态（与 Q4-7 已修的
                // onConfigured 异常缺陷同构）。代次递增使后续迟到回调（onDisconnected/onError）
                // 按旧代次丢弃；rollbackStart() 释放已置的 cameraDevice/reader/thread 并置空字段，
                // 与 stop() 共用同一完整释放路径（B07-006）。不破坏正常路径。
                epochGate.advance();
                rollbackStart();
                observer.onError("camera2", "onOpened失败: " + e.getMessage());
            }
        }

        // Q-CQ-B07-008: onConfigureFailed 主体提取为包内方法 —— JVM 单测以 null 框架参数直接驱动
        void handleSessionConfigureFailed(@NonNull CameraCaptureSession s) {
            if (!epochGate.isCurrent(boundEpoch)) {
                releaseCameraSession(s);
                return;
            }
            // Q-CQ-B07-008 WHY: 修复前当前代次分支仅上报错误，失败的 session 参数（s）未释放、
            // cameraDevice 已置未释放、reader/thread 存活 —— 相机停在"打开但无会话无帧"的半死态，
            // 直到下次 stop/start 才恢复（与 Q4-7 已修的 onConfigured 异常缺陷同构）。此路径下
            // session 字段为 null（onConfigured 未执行），故必须显式释放失败的 session 参数；
            // 随后代次递增 + rollbackStart() 释放 cameraDevice/reader/thread 并置空字段，回到
            // 可控的已停止状态。不破坏正常路径。
            releaseCameraSession(s);
            epochGate.advance();
            rollbackStart();
            observer.onError("camera2", "createCaptureSession失败");
        }

        // Q-CQ-B07-007: onConfigured 主体提取为包内方法 —— JVM 单测以 null 框架参数直接驱动，
        // 覆盖"配置阶段异常（setRepeatingRequest 等）"路径；生产行为与内联等价。
        void handleSessionConfigured(@NonNull CameraCaptureSession s, @NonNull CaptureRequest.Builder builder) {
            if (!epochGate.isCurrent(boundEpoch)) {
                releaseCameraSession(s);
                return;
            }
            session = s;
            try {
                builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                Range<Integer> fps = chooseFpsRange();
                if (fps != null) {
                    builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps);
                }
                session.setRepeatingRequest(builder.build(), null, handler);
            } catch (Exception e) {
                // Q-CQ-B07-007 WHY: 配置阶段异常（如 setRepeatingRequest 失败）若仅上报错误，
                // 相机将停在"配置完成但无请求"的半死态——session 被占用、无帧流入，
                // 期间相机一直不可用。代次递增使此后的迟到回调（onDisconnected/onError）
                // 按旧代次丢弃，不再误报当前会话；rollbackStart() 释放已创建的
                // session/device/reader/thread 并置空字段，与 stop() 共用同一完整释放路径
                // （B07-006），回到可控的已停止状态；不破坏正常路径。
                epochGate.advance();
                rollbackStart();
                observer.onError("camera2", "setRepeatingRequest失败: " + e.getMessage());
            }
        }

        @Override
        public void onDisconnected(@NonNull CameraDevice camera) {
            if (!epochGate.isCurrent(boundEpoch)) {
                releaseCameraDevice(camera);
                return;
            }
            observer.onError("camera2", "相机断开连接");
            releaseCameraDevice(camera);
        }

        @Override
        public void onError(@NonNull CameraDevice camera, int error) {
            if (!epochGate.isCurrent(boundEpoch)) {
                releaseCameraDevice(camera);
                return;
            }
            observer.onError("camera2", "相机错误 error=" + error);
            releaseCameraDevice(camera);
        }
    }
}
