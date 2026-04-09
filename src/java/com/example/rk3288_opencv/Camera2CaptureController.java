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

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;

final class Camera2CaptureController implements CaptureController {
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

    Camera2CaptureController(@NonNull Context context,
                             @NonNull ExternalFrameSink sink,
                             @NonNull CaptureObserver observer,
                             @NonNull RotationDegreesProvider deviceRotationDegrees) {
        this.appContext = context.getApplicationContext();
        this.sink = Objects.requireNonNull(sink);
        this.observer = Objects.requireNonNull(observer);
        this.deviceRotationDegrees = Objects.requireNonNull(deviceRotationDegrees);
    }

    @Override
    public String name() {
        return "Camera2";
    }

    @Override
    @SuppressLint("MissingPermission")
    public boolean start(String cameraId) {
        stop();
        thread = new HandlerThread("Camera2Capture");
        thread.start();
        handler = new Handler(thread.getLooper());

        try {
            CameraManager mgr = (CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) {
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
            lastStatsRealtimeMs = SystemClock.elapsedRealtime();
            lastFrameTsNs = 0L;
            statsFrames = 0;
            statsPushOk = 0;
            statsPushFail = 0;
            statsAnalyzeNsSum = 0L;
            statsAnalyzeNsMax = 0L;
            statsApproxDropped = 0;

            mgr.openCamera(cameraId, stateCallback, handler);
            return true;
        } catch (SecurityException se) {
            observer.onError("camera2", "缺少相机权限");
            return false;
        } catch (CameraAccessException e) {
            observer.onError("camera2", "CameraAccessException: " + e.getMessage());
            return false;
        } catch (Exception e) {
            observer.onError("camera2", "启动失败: " + e.getMessage());
            return false;
        }
    }

    @Override
    public void stop() {
        if (handler != null) {
            handler.removeCallbacksAndMessages(null);
        }
        if (session != null) {
            try {
                session.stopRepeating();
            } catch (Exception ignored) {
            }
            try {
                session.close();
            } catch (Exception ignored) {
            }
            session = null;
        }
        if (cameraDevice != null) {
            try {
                cameraDevice.close();
            } catch (Exception ignored) {
            }
            cameraDevice = null;
        }
        if (imageReader != null) {
            try {
                imageReader.close();
            } catch (Exception ignored) {
            }
            imageReader = null;
        }
        if (thread != null) {
            try {
                thread.quitSafely();
            } catch (Exception ignored) {
            }
            thread = null;
        }
        handler = null;
        chars = null;
    }

    private void onImageAvailable(ImageReader reader) {
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

    private final CameraDevice.StateCallback stateCallback = new CameraDevice.StateCallback() {
        @Override
        public void onOpened(@NonNull CameraDevice camera) {
            cameraDevice = camera;
            try {
                Surface surface = imageReader.getSurface();
                CaptureRequest.Builder builder = camera.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                builder.addTarget(surface);
                List<Surface> surfaces = Arrays.asList(surface);
                camera.createCaptureSession(surfaces, new CameraCaptureSession.StateCallback() {
                    @Override
                    public void onConfigured(@NonNull CameraCaptureSession s) {
                        session = s;
                        try {
                            builder.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_VIDEO);
                            Range<Integer> fps = chooseFpsRange();
                            if (fps != null) {
                                builder.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps);
                            }
                            session.setRepeatingRequest(builder.build(), null, handler);
                        } catch (Exception e) {
                            observer.onError("camera2", "setRepeatingRequest失败: " + e.getMessage());
                        }
                    }

                    @Override
                    public void onConfigureFailed(@NonNull CameraCaptureSession s) {
                        observer.onError("camera2", "createCaptureSession失败");
                    }
                }, handler);
            } catch (Exception e) {
                observer.onError("camera2", "onOpened失败: " + e.getMessage());
            }
        }

        @Override
        public void onDisconnected(@NonNull CameraDevice camera) {
            observer.onError("camera2", "相机断开连接");
            try {
                camera.close();
            } catch (Exception ignored) {
            }
        }

        @Override
        public void onError(@NonNull CameraDevice camera, int error) {
            observer.onError("camera2", "相机错误 error=" + error);
            try {
                camera.close();
            } catch (Exception ignored) {
            }
        }
    };
}
