package com.example.rk3288_opencv;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraDevice;
import android.media.ImageReader;
import android.os.HandlerThread;

import org.junit.Test;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * Camera2CaptureController 资源生命周期治理测试。
 * 覆盖：
 * - Q-CQ-B07-005 代次隔离：stop/start 后旧代次回调到达时仅释放参数资源，不操作新会话状态；
 * - Q-CQ-B07-006 启动失败回滚：同步启动在资源预建后抛异常时 reader/thread/device 全部释放。
 *
 * 说明：本类不含 Robolectric/Mockito，且 CameraDevice/CameraCaptureSession 等框架抽象类
 * 的构造器为包私有（android.hardware.camera2），无法在 com.example.rk3288_opencv 下子类化。
 * 因此测试通过 Camera2CaptureController 的包内释放接缝（releaseCameraSession/Device/Reader/
 * CaptureThread）观测释放路径：子类覆盖接缝计数，并以 null 框架参数驱动旧回调。
 */
public class Camera2CaptureControllerTest {

    // ---------- 测试替身 ----------

    static class FakeObserver implements CaptureObserver {
        final List<String> errors = new ArrayList<>();
        boolean pushed;

        @Override
        public void onFramePushed(boolean ok, long timestampNs) {
            pushed = true;
        }

        @Override
        public void onError(String stage, String message) {
            errors.add(stage + ": " + message);
        }

        boolean hasError() {
            return !errors.isEmpty();
        }
    }

    static class FakeSink implements ExternalFrameSink {
        @Override
        public boolean onYuv420888Frame(ByteBuffer yBuffer,
                                        int yRowStride,
                                        ByteBuffer uBuffer,
                                        int uRowStride,
                                        int uPixelStride,
                                        ByteBuffer vBuffer,
                                        int vRowStride,
                                        int vPixelStride,
                                        int width,
                                        int height,
                                        long timestampNs,
                                        int rotationDegrees,
                                        boolean mirrored) {
            return true;
        }
    }

    // 释放接缝观测控制器：覆盖四个释放接缝记录调用次数
    static class SeamReleaseController extends Camera2CaptureController {
        int releaseCameraSessionCalls;
        int releaseCameraDeviceCalls;
        int releaseImageReaderCalls;
        int releaseCaptureThreadCalls;

        SeamReleaseController(FakeObserver obs, FakeSink sink) {
            super(null, sink, obs, () -> 0);
        }

        @Override
        void releaseCameraSession(CameraCaptureSession session) {
            releaseCameraSessionCalls++;
        }

        @Override
        void releaseCameraDevice(CameraDevice cameraDevice) {
            releaseCameraDeviceCalls++;
        }

        @Override
        void releaseImageReader(ImageReader imageReader) {
            releaseImageReaderCalls++;
        }

        @Override
        void releaseCaptureThread(HandlerThread thread) {
            releaseCaptureThreadCalls++;
        }
    }

    // 启动失败回滚控制器：覆盖 startSync 模拟“资源预建后 openCamera 抛异常”
    static class RollbackTestController extends SeamReleaseController {
        int startSyncCalls;

        RollbackTestController(FakeObserver obs, FakeSink sink) {
            super(obs, sink);
        }

        @Override
        boolean startSync(String cameraId, int gen) throws CameraAccessException {
            startSyncCalls++;
            // Q-CQ-B07-006 模拟：reader/device/session/thread 已在真实路径中预建；
            // 这里直接抛异常，断言回滚路径经释放接缝释放全部四类资源
            throw new CameraAccessException(CameraAccessException.CAMERA_ERROR);
        }
    }

    // ---------- 反射辅助（字段均声明于 Camera2CaptureController 基类） ----------

    private static void setField(Camera2CaptureController c, String name, Object value) {
        try {
            Field f = Camera2CaptureController.class.getDeclaredField(name);
            f.setAccessible(true);
            f.set(c, value);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    // 修复前无对应字段时静默跳过，交由后续断言判定 RED
    private static void setFieldQuietly(Camera2CaptureController c, String name, Object value) {
        try {
            Field f = Camera2CaptureController.class.getDeclaredField(name);
            f.setAccessible(true);
            f.set(c, value);
        } catch (NoSuchFieldException ignored) {
        } catch (IllegalAccessException e) {
            throw new RuntimeException(e);
        }
    }

    private static Object getField(Camera2CaptureController c, String name) {
        try {
            Field f = Camera2CaptureController.class.getDeclaredField(name);
            f.setAccessible(true);
            return f.get(c);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
    }

    private static Camera2CaptureController newController(FakeObserver obs, FakeSink sink) {
        // Q-CQ-B07-005/006: null Context 仅用于 JVM 单测，生产路径由 MainActivity 传入真实 Context
        return new Camera2CaptureController(null, sink, obs, () -> 0);
    }

    // ---------- Q-CQ-B07-005：代次隔离 ----------

    /**
     * RED 判别：修复前 onDisconnected/onError 无条件上报 observer.onError("camera2", ...)，
     * 旧代次断开/错误事件也会上报新会话；本测试先推进当前代次到 2，再投递绑定代次 1 的
     * 旧回调（参数 camera 以 null 代替，旧代次门禁在解引用前即返回）：
     * - 旧回调必须经 releaseCameraDevice 接缝释放参数资源；
     * - 不得上报错误、不得写入 cameraDevice/session。
     */
    @Test
    public void staleCallback_onlyReleasesParamAndDoesNotTouchCurrentState() {
        FakeObserver obs = new FakeObserver();
        SeamReleaseController c = new SeamReleaseController(obs, new FakeSink());
        setField(c, "generation", 2); // 模拟已 stop/start，当前代次为 2

        // 绑定旧代次 1 的回调 —— 模拟 stop/start 前注册、stop/start 后才到达的异步回调
        Camera2CaptureController.GenerationStateCallback stale = c.new GenerationStateCallback(1);

        stale.onDisconnected(null);
        assertTrue("旧代次断开回调应释放参数 camera", c.releaseCameraDeviceCalls >= 1);
        assertFalse("旧代次断开不得上报当前会话错误", obs.hasError());

        // ERROR_* 常量不在 CameraDevice 上且单测 stub 不可见，使用字面量错误码即可
        stale.onError(null, 0);
        assertTrue("旧代次错误回调应释放参数 camera", c.releaseCameraDeviceCalls >= 2);
        assertFalse("旧代次错误不得上报当前会话错误", obs.hasError());

        stale.onOpened(null);
        assertTrue("旧代次 onOpened 应释放参数 camera", c.releaseCameraDeviceCalls >= 3);
        assertFalse("旧代次 onOpened 不得上报当前会话错误", obs.hasError());
        assertNull("旧代次 onOpened 不得写入 cameraDevice", getField(c, "cameraDevice"));
        assertNull("旧代次 onOpened 不得写入 session", getField(c, "session"));
    }

    /**
     * RED 判别：修复前 onImageAvailable 不检查代次，stop/start 后旧 reader 的帧仍会推送
     * （此处以 null reader 触发异常路径，catch 中仍调用 onFramePushed）。
     * 修复后帧监听绑定代次与当前代次不一致时直接丢弃：不推送、不统计、不上报。
     */
    @Test
    public void staleFrameCallback_isDroppedWithoutPushOrReport() throws Exception {
        FakeObserver obs = new FakeObserver();
        Camera2CaptureController c = newController(obs, new FakeSink());
        // 模拟 stop/start：帧监听绑定旧代次 1，当前代次已到 2
        setFieldQuietly(c, "listenerGeneration", 1);
        setFieldQuietly(c, "generation", 2);

        Method m = Camera2CaptureController.class.getDeclaredMethod("onImageAvailable", ImageReader.class);
        m.setAccessible(true);
        try {
            m.invoke(c, (ImageReader) null);
        } catch (InvocationTargetException e) {
            // 修复前无代次门禁时 null reader 触发 NPE：仍应以“不得推送”判定失败
            throw new RuntimeException("修复前旧帧回调不应可达（应被代次门禁丢弃）", e.getCause());
        }

        assertFalse("旧代次帧不得推送", obs.pushed);
        assertFalse("旧代次帧不得上报错误", obs.hasError());
    }

    // ---------- Q-CQ-B07-006：同步启动失败完整回滚 ----------

    /**
     * 经 RollbackTestController 驱动 start()：start 首调 stop() 触发一次回滚，随后
     * startSync 抛 CameraAccessException 触发失败回滚，释放接缝各调用两次。
     * RED 判别：修复前 start() 的 catch 分支仅上报错误并 return false，不释放已预建资源
     * （释放接缝调用次数为 0、thread/cameraDevice/session 字段保持非空）。
     */
    @Test
    public void startFailure_rollsBackPrebuiltResources() {
        FakeObserver obs = new FakeObserver();
        RollbackTestController c = new RollbackTestController(obs, new FakeSink());

        boolean ok = c.start("camera-0");

        assertFalse("启动失败应返回 false", ok);
        assertEquals("startSync 应被调用一次", 1, c.startSyncCalls);
        assertEquals("代次应已递增（stop + start 各一次）", Integer.valueOf(2), getField(c, "generation"));
        // stop() 回滚 + 失败回滚 各一次，共两次
        assertEquals("session 释放路径应被调用两次", 2, c.releaseCameraSessionCalls);
        assertEquals("device 释放路径应被调用两次", 2, c.releaseCameraDeviceCalls);
        assertEquals("reader 释放路径应被调用两次", 2, c.releaseImageReaderCalls);
        assertEquals("thread 释放路径应被调用两次", 2, c.releaseCaptureThreadCalls);
        assertNull("imageReader 字段应置空（reader 释放）", getField(c, "imageReader"));
        assertNull("cameraDevice 字段应置空", getField(c, "cameraDevice"));
        assertNull("session 字段应置空", getField(c, "session"));
        assertNull("thread 字段应置空", getField(c, "thread"));
        assertNull("handler 字段应置空", getField(c, "handler"));
        assertNull("currentStateCallback 字段应置空", getField(c, "currentStateCallback"));
        assertTrue("应上报启动失败错误", obs.hasError());
    }
}
