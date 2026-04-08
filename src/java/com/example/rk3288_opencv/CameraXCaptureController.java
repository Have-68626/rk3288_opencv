package com.example.rk3288_opencv;

import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Range;
import android.util.Size;

import androidx.annotation.NonNull;
import androidx.camera.camera2.interop.Camera2CameraInfo;
import androidx.camera.camera2.interop.Camera2Interop;
import androidx.camera.camera2.interop.ExperimentalCamera2Interop;
import androidx.camera.core.Camera;
import androidx.camera.core.CameraSelector;
import androidx.camera.core.ImageAnalysis;
import androidx.camera.core.ImageProxy;
import androidx.camera.lifecycle.ProcessCameraProvider;
import androidx.core.content.ContextCompat;
import androidx.lifecycle.LifecycleOwner;

import com.google.common.util.concurrent.ListenableFuture;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

@ExperimentalCamera2Interop
final class CameraXCaptureController implements CaptureController {
    private static final int MAX_W = 1920;
    private static final int MAX_H = 1080;
    private final Context appContext;
    private final LifecycleOwner lifecycleOwner;
    private final ExternalFrameSink sink;
    private final CaptureObserver observer;
    private final RotationDegreesProvider deviceRotationDegrees;

    private ExecutorService analyzerExecutor;
    private ProcessCameraProvider provider;
    private Camera camera;
    private ImageAnalysis analysis;

    private int sensorOrientation;
    private boolean mirrored;

    CameraXCaptureController(@NonNull Context context,
                             @NonNull LifecycleOwner lifecycleOwner,
                             @NonNull ExternalFrameSink sink,
                             @NonNull CaptureObserver observer,
                             @NonNull RotationDegreesProvider deviceRotationDegrees) {
        this.appContext = context.getApplicationContext();
        this.lifecycleOwner = Objects.requireNonNull(lifecycleOwner);
        this.sink = Objects.requireNonNull(sink);
        this.observer = Objects.requireNonNull(observer);
        this.deviceRotationDegrees = Objects.requireNonNull(deviceRotationDegrees);
    }

    @Override
    public String name() {
        return "CameraX";
    }

    @Override
    public boolean start(String cameraId) {
        stop();
        analyzerExecutor = Executors.newSingleThreadExecutor(r -> new Thread(r, "CameraXAnalyzer"));
        ListenableFuture<ProcessCameraProvider> future = ProcessCameraProvider.getInstance(appContext);
        future.addListener(() -> {
            try {
                provider = future.get();
                provider.unbindAll();

                CameraSelector selector = new CameraSelector.Builder()
                        .addCameraFilter(infos -> filterByCameraId(infos, cameraId))
                        .build();

                CameraCharacteristics chars = getCameraCharacteristicsOrNull(cameraId);
                Integer so = chars != null ? chars.get(CameraCharacteristics.SENSOR_ORIENTATION) : null;
                sensorOrientation = so == null ? 0 : so;
                Integer facing = chars != null ? chars.get(CameraCharacteristics.LENS_FACING) : null;
                mirrored = facing != null && facing == CameraCharacteristics.LENS_FACING_FRONT;
                Size size = chooseAnalysisSize(chars);

                ImageAnalysis.Builder builder = new ImageAnalysis.Builder()
                        .setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                        .setTargetResolution(size);

                Range<Integer> fps = chooseFpsRange(chars);
                if (fps != null) {
                    try {
                        Camera2Interop.Extender<ImageAnalysis> ext = new Camera2Interop.Extender<>(builder);
                        ext.setCaptureRequestOption(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps);
                    } catch (Exception ignored) {
                    }
                }

                analysis = builder.build();

                analysis.setAnalyzer(analyzerExecutor, this::analyze);
                camera = provider.bindToLifecycle(lifecycleOwner, selector, analysis);
            } catch (Exception e) {
                observer.onError("camerax", "绑定失败: " + e.getMessage());
            }
        }, ContextCompat.getMainExecutor(appContext));
        return true;
    }

    @Override
    public void stop() {
        if (analysis != null) {
            try {
                analysis.clearAnalyzer();
            } catch (Exception ignored) {
            }
            analysis = null;
        }
        if (provider != null) {
            try {
                provider.unbindAll();
            } catch (Exception ignored) {
            }
            provider = null;
        }
        camera = null;
        if (analyzerExecutor != null) {
            try {
                analyzerExecutor.shutdownNow();
            } catch (Exception ignored) {
            }
            analyzerExecutor = null;
        }
    }

    private void analyze(@NonNull ImageProxy image) {
        try {
            int w = image.getWidth();
            int h = image.getHeight();
            if (w > MAX_W || h > MAX_H) {
                observer.onError("camerax", "超规格分辨率: " + w + "x" + h + " (上限 " + MAX_W + "x" + MAX_H + ")");
                observer.onFramePushed(false, image.getImageInfo().getTimestamp());
                return;
            }
            ImageProxy.PlaneProxy[] planes = image.getPlanes();
            if (planes == null || planes.length < 3) {
                observer.onFramePushed(false, image.getImageInfo().getTimestamp());
                return;
            }
            ByteBuffer y = planes[0].getBuffer();
            ByteBuffer u = planes[1].getBuffer();
            ByteBuffer v = planes[2].getBuffer();
            long ts = image.getImageInfo().getTimestamp();
            int rot = computeRotationDegrees();
            boolean ok = sink.onYuv420888Frame(
                    y, planes[0].getRowStride(),
                    u, planes[1].getRowStride(), planes[1].getPixelStride(),
                    v, planes[2].getRowStride(), planes[2].getPixelStride(),
                    w, h,
                    ts, rot, mirrored);
            observer.onFramePushed(ok, ts);
        } catch (Exception e) {
            long ts = image.getImageInfo().getTimestamp();
            observer.onFramePushed(false, ts);
        } finally {
            try {
                image.close();
            } catch (Exception ignored) {
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

    private static List<androidx.camera.core.CameraInfo> filterByCameraId(List<androidx.camera.core.CameraInfo> infos, String cameraId) {
        for (androidx.camera.core.CameraInfo info : infos) {
            try {
                String id = Camera2CameraInfo.from(info).getCameraId();
                if (cameraId.equals(id)) {
                    return java.util.Collections.singletonList(info);
                }
            } catch (Exception ignored) {
            }
        }
        return infos;
    }

    private CameraCharacteristics getCameraCharacteristicsOrNull(@NonNull String cameraId) {
        try {
            android.hardware.camera2.CameraManager mgr =
                    (android.hardware.camera2.CameraManager) appContext.getSystemService(Context.CAMERA_SERVICE);
            if (mgr == null) return null;
            return mgr.getCameraCharacteristics(cameraId);
        } catch (Exception ignored) {
            return null;
        }
    }

    private static Size chooseAnalysisSize(CameraCharacteristics chars) {
        List<Size> sizes = getYuvSizes(chars);
        if (sizes.isEmpty()) return new Size(1280, 720);

        Size preferred720 = null;
        for (Size s : sizes) {
            if (s.getWidth() == 1280 && s.getHeight() == 720) {
                preferred720 = s;
                break;
            }
        }
        if (preferred720 != null) return preferred720;

        List<Size> capped = new ArrayList<>();
        for (Size s : sizes) {
            if (s.getWidth() <= MAX_W && s.getHeight() <= MAX_H) {
                capped.add(s);
            }
        }
        if (capped.isEmpty()) capped = sizes;

        Collections.sort(capped, (a, b) -> Integer.compare(b.getWidth() * b.getHeight(), a.getWidth() * a.getHeight()));
        return capped.get(0);
    }

    private static List<Size> getYuvSizes(CameraCharacteristics chars) {
        if (chars == null) return Collections.emptyList();
        StreamConfigurationMap map = chars.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
        if (map == null) return Collections.emptyList();
        Size[] out = map.getOutputSizes(ImageFormat.YUV_420_888);
        if (out == null || out.length == 0) return Collections.emptyList();
        return new ArrayList<>(Arrays.asList(out));
    }

    private static Range<Integer> chooseFpsRange(CameraCharacteristics chars) {
        if (chars == null) return null;
        Range<Integer>[] ranges = chars.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES);
        if (ranges == null || ranges.length == 0) return null;

        List<Range<Integer>> list = new ArrayList<>(Arrays.asList(ranges));
        Collections.sort(list, new Comparator<Range<Integer>>() {
            @Override
            public int compare(Range<Integer> a, Range<Integer> b) {
                int au = a.getUpper();
                int bu = b.getUpper();
                boolean aOk = (au >= 30 && au <= 60);
                boolean bOk = (bu >= 30 && bu <= 60);
                if (aOk != bOk) return aOk ? -1 : 1;
                int upperCmp = Integer.compare(au, bu);
                if (upperCmp != 0) return upperCmp;
                return -Integer.compare(a.getLower(), b.getLower());
            }
        });
        return list.get(0);
    }
}
