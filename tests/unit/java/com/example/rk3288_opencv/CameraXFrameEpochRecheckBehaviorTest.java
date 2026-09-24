package com.example.rk3288_opencv;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.graphics.Rect;

import androidx.camera.core.ImageInfo;
import androidx.camera.core.ImageProxy;
import androidx.lifecycle.Lifecycle;
import androidx.lifecycle.LifecycleOwner;

import org.junit.Test;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/**
 * FACE-CAM-008 **行为性**判别（CameraX 侧）——与 Camera2 的
 * {@link Camera2CaptureControllerTest#frameNotCommittedIfEpochStaleAtSubmitPoint} 对偶。
 *
 * <p>背景：{@code CameraFrameEpochRecheckStructureTest} 是**纯源文本匹配**的结构守卫，
 * 对「保留条件、删掉 {@code return;}」「极性反转 {@code if(!isCurrent)→if(isCurrent)}」这类
 * **语义**回归无判别力。本类以运行期行为补上这一缺口：经反射驱动私有的
 * {@code CameraXCaptureController.analyze(ImageProxy, long)}，注入的
 * {@code RotationDegreesProvider} 在返回前**同步推进代次**（确定性、无 sleep），
 * 精确复现「入口校验通过 → 提交前代次失效」窗口 —— 即复检必须位于
 * {@code computeRotationDegrees()} 之后的理由。
 *
 * <p>构造真 androidx 实例所需的最小替身均在本类内实现：{@code ImageProxy}/{@code ImageInfo}/
 * {@code PlaneProxy}（真 androidx 接口，camera-core 在单测类路径上），
 * 一个最小 {@code Context}（见 {@code tests/unit/java/android/content/Context.java}），
 * 一个匿名 {@code LifecycleOwner}（构造器只做 null 检查，不触达其方法）。
 *
 * <p><b>打红方式</b>：
 * <ul>
 *   <li>删掉 {@code analyze} 内提交前的二次 {@code isCurrent(capturedEpoch)} 复检 ⇒
 *       过期帧仍被推送，{@code frameNotCommittedIfEpochStaleAtSubmitPoint} 的
 *       {@code sink.calls == 0} 变 1 ⇒ RED；</li>
 *   <li>极性反转 {@code if(!isCurrent(...))} → {@code if(isCurrent(...))} ⇒ 同上（当前帧被丢、
 *       过期帧被放行）；</li>
 *   <li>误删提交（{@code sink.onYuv420888Frame}）⇒ 正对照
 *       {@code frameCommittedWhenEpochStillCurrentAtSubmitPoint} RED。</li>
 *   <li>删掉 {@code analyze} 开头**入口整段**校验（含 {@code image.close()} 早退）⇒
 *       {@code frameNotCommittedIfEpochAlreadyStaleAtEntry} 的 {@code rotCalls == 0} 变 1 ⇒ RED。
 *       <b>注意</b>：入口校验此前是行为用例的死角 —— 删掉它时上面两条用例**仍全绿**
 *       （二者的入口代次恒为当前 ⇒ 走到提交前复检照样丢弃；全程不推进 ⇒ 两条都通过）。</li>
 * </ul>
 */
public class CameraXFrameEpochRecheckBehaviorTest {

    // ---------- 测试替身 ----------

    static final class FakeObserver implements CaptureObserver {
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

    static final class FakeSink implements ExternalFrameSink {
        int calls;

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
            calls++;
            return true;
        }
    }

    /** 最小 {@code ImageProxy.PlaneProxy}：仅提供 analyze 读取的 buffer/stride。 */
    static final class FakePlane implements ImageProxy.PlaneProxy {
        private final ByteBuffer buffer;
        private final int rowStride;
        private final int pixelStride;

        FakePlane(ByteBuffer buffer, int rowStride, int pixelStride) {
            this.buffer = buffer;
            this.rowStride = rowStride;
            this.pixelStride = pixelStride;
        }

        @Override
        public int getRowStride() {
            return rowStride;
        }

        @Override
        public int getPixelStride() {
            return pixelStride;
        }

        @Override
        public ByteBuffer getBuffer() {
            return buffer;
        }
    }

    /** 最小 {@code ImageInfo}：analyze 只读 {@link #getTimestamp()}。 */
    static final class FakeImageInfo implements ImageInfo {
        private final long timestampNs;

        FakeImageInfo(long timestampNs) {
            this.timestampNs = timestampNs;
        }

        @Override
        public androidx.camera.core.impl.TagBundle getTagBundle() {
            return null;
        }

        @Override
        public long getTimestamp() {
            return timestampNs;
        }

        @Override
        public int getRotationDegrees() {
            return 0;
        }

        @Override
        public void populateExifData(androidx.camera.core.impl.utils.ExifData.Builder builder) {
        }
    }

    /** 最小 {@code ImageProxy}：analyze 只读尺寸/平面/时间戳并 close。 */
    static final class FakeImageProxy implements ImageProxy {
        private final ImageProxy.PlaneProxy[] planes;
        private final int width;
        private final int height;
        private final ImageInfo info;
        int closeCalls;

        FakeImageProxy(ImageProxy.PlaneProxy[] planes, int width, int height, long timestampNs) {
            this.planes = planes;
            this.width = width;
            this.height = height;
            this.info = new FakeImageInfo(timestampNs);
        }

        @Override
        public void close() {
            closeCalls++;
        }

        @Override
        public Rect getCropRect() {
            return null;
        }

        @Override
        public void setCropRect(Rect rect) {
        }

        @Override
        public int getFormat() {
            return 0;
        }

        @Override
        public int getHeight() {
            return height;
        }

        @Override
        public int getWidth() {
            return width;
        }

        @Override
        public ImageProxy.PlaneProxy[] getPlanes() {
            return planes;
        }

        @Override
        public ImageInfo getImageInfo() {
            return info;
        }

        @Override
        public android.media.Image getImage() {
            return null;
        }
    }

    /** 最小 {@code LifecycleOwner}：构造器仅做 null 检查，不调用其方法。 */
    static final class FakeLifecycleOwner implements LifecycleOwner {
        @Override
        public Lifecycle getLifecycle() {
            return null;
        }
    }

    // ---------- 反射接缝（生产可见性不变） ----------

    private static CaptureSourceEpochGate gateOf(CameraXCaptureController c) throws Exception {
        Field f = CameraXCaptureController.class.getDeclaredField("startGate");
        f.setAccessible(true);
        return (CaptureSourceEpochGate) f.get(c);
    }

    /**
     * 置 {@code outputResolutionLogged=true}：跳过 analyze 首帧的「输出分辨率=…」日志——
     * 该行走 {@code AppLog.i} → {@code android.util.Log.println}，在 mockable jar 下抛
     * "not mocked"。此日志与本用例所测的代次复检无关，跳过不削弱判别力。
     */
    private static void skipResolutionLog(CameraXCaptureController c) throws Exception {
        Field f = CameraXCaptureController.class.getDeclaredField("outputResolutionLogged");
        f.setAccessible(true);
        f.setBoolean(c, true);
    }

    private static void invokeAnalyze(CameraXCaptureController c, ImageProxy image, long capturedEpoch)
            throws Exception {
        Method m = CameraXCaptureController.class
                .getDeclaredMethod("analyze", ImageProxy.class, long.class);
        m.setAccessible(true);
        m.invoke(c, image, capturedEpoch);
    }

    private static CameraXCaptureController newController(ExternalFrameSink sink,
                                                          CaptureObserver observer,
                                                          RotationDegreesProvider provider) {
        return new CameraXCaptureController(
                new Context(), new FakeLifecycleOwner(), sink, observer, provider);
    }

    /** 构造一帧可被 analyze 消费的 YUV_420_888 替身（3 平面，4x4 未超规格）。 */
    private static FakeImageProxy buildOneFrame(long timestampNs) {
        ImageProxy.PlaneProxy[] planes = new ImageProxy.PlaneProxy[]{
                new FakePlane(ByteBuffer.allocate(16), 16, 1),
                new FakePlane(ByteBuffer.allocate(8), 8, 1),
                new FakePlane(ByteBuffer.allocate(8), 8, 1)
        };
        return new FakeImageProxy(planes, 4, 4, timestampNs);
    }

    // ---------- FACE-CAM-008 行为判别 ----------

    /**
     * RED 判别：修复前 analyze 只在入口校验一次代次（:203），入口校验与 sink 提交之间若
     * 发生 stop/切源，旧帧仍被推入已失效会话。本用例令代次推进发生在
     * {@code computeRotationDegrees()} 内（入口校验之后、提交复检之前），精确复现该窗口。
     *
     * <p>断言：修复后提交前复检失败即丢弃，sink 不得被调用（旧代码 {@code sink.calls==1} 即 RED）；
     * 帧仍须经 finally 关闭释放。
     */
    @Test
    public void frameNotCommittedIfEpochStaleAtSubmitPoint() throws Exception {
        FakeObserver obs = new FakeObserver();
        FakeSink sink = new FakeSink();
        final CaptureSourceEpochGate[] gateHolder = new CaptureSourceEpochGate[1];
        // 关键：代次推进发生在 computeRotationDegrees() 内（入口校验之后、提交复检之前）
        RotationDegreesProvider provider = () -> {
            gateHolder[0].advance();
            return 0;
        };
        CameraXCaptureController c = newController(sink, obs, provider);
        gateHolder[0] = gateOf(c);
        skipResolutionLog(c);

        long capturedEpoch = gateOf(c).enter(); // 当前代次 1，analyzer 闭包捕获同代次
        FakeImageProxy image = buildOneFrame(123L);

        invokeAnalyze(c, image, capturedEpoch);

        assertEquals("提交前代次已失效，帧不得推送", 0, sink.calls);
        assertFalse("复检丢弃不得上报推送", obs.pushed);
        assertFalse("复检丢弃不得上报错误", obs.hasError());
        assertTrue("帧仍须经 finally 关闭释放", image.closeCalls >= 1);
    }

    /**
     * 正对照：代次在提交前保持当前时，帧必须正常提交 —— 证明上面的判别用例不是
     * 「因为路径本就到不了 sink」而恒绿（若误删提交逻辑，本用例 RED）。
     */
    @Test
    public void frameCommittedWhenEpochStillCurrentAtSubmitPoint() throws Exception {
        FakeObserver obs = new FakeObserver();
        FakeSink sink = new FakeSink();
        CameraXCaptureController c = newController(sink, obs, () -> 0);
        skipResolutionLog(c);

        long capturedEpoch = gateOf(c).enter(); // 当前代次 1，全程不推进
        FakeImageProxy image = buildOneFrame(123L);

        invokeAnalyze(c, image, capturedEpoch);

        assertEquals("代次仍当前，帧应正常提交", 1, sink.calls);
        assertTrue("提交成功应上报推送", obs.pushed);
        assertTrue("帧仍须经 finally 关闭释放", image.closeCalls >= 1);
    }

    /**
     * **FACE-CAM-007 判别性用例**：入口即过期（{@code capturedEpoch} 在进入 {@code analyze} 前
     * 已经失效）必须在**入口整段丢弃** —— 不取旋转、不取尺寸/平面、不提交、不统计。
     *
     * <p>与 {@link #frameNotCommittedIfEpochStaleAtSubmitPoint} 互补：后者覆盖「入口当时有效、
     * 提交前失效」，本用例覆盖 {@code analyze} 开头的入口分支
     * （{@code if (!startGate.isCurrent(capturedEpoch))}）。
     * 该分支此前**从未被任何行为用例以「入口即 stale」这一输入构造过**：既有的两条用例入口代次
     * 都是当前代次，故删掉入口整段校验后它们**仍全绿**（帧会走到提交前复检照样被丢弃，或全程不推进
     * 压根无需入口校验），入口分支遂成死角。
     *
     * <p><b>打红方式（改坏哪一处即红）</b>：
     * <ul>
     *   <li>删掉 {@code analyze} 开头的入口校验整段 ⇒ 帧继续进入帧处理、走到
     *       {@code computeRotationDegrees()} ⇒ {@code rotCalls} 变 1 ⇒ 红。
     *       注意此时 {@code sink.calls} 仍为 0（提交前复检会兜住），故**不能**只断言它；</li>
     *   <li>把入口分支的 {@code image.close()} 早退改成「只 return 不 close」（该早退在 try 之外，
     *       不走 finally）⇒ 最后一条断言红；</li>
     *   <li>入口校验极性反转 {@code if (isCurrent(...))} ⇒ 过期帧不再被入口拦下、继续走到
     *       {@code computeRotationDegrees()} ⇒ {@code rotCalls} 变 1 ⇒ 红。</li>
     * </ul>
     *
     * <p><b>为何用 {@code rotCalls} 而不是反射读私有 {@code statsFrames}</b>：{@code rotCalls} 是
     * **行为性**观测量（注入的 {@link RotationDegreesProvider} 被调用次数），不新增对私有字段的
     * 白盒耦合，判别力等价于「有没有进到帧处理」。
     */
    @Test
    public void frameNotCommittedIfEpochAlreadyStaleAtEntry() throws Exception {
        FakeObserver obs = new FakeObserver();
        FakeSink sink = new FakeSink();
        final int[] rotCalls = {0};
        // ★ 判别器：入口早退路径绝不触达 computeRotationDegrees()；删掉入口校验即被调用一次。
        RotationDegreesProvider provider = () -> {
            rotCalls[0]++;
            return 0;
        };
        CameraXCaptureController c = newController(sink, obs, provider);
        skipResolutionLog(c);

        long staleEpoch = gateOf(c).enter();   // 代次 1
        gateOf(c).advance();                   // 代次 2 ⇒ staleEpoch 在入口即已失效
        FakeImageProxy image = buildOneFrame(123L);

        invokeAnalyze(c, image, staleEpoch);

        assertEquals("入口即过期 ⇒ 帧不得推送", 0, sink.calls);
        assertFalse("入口丢弃不得上报推送", obs.pushed);
        assertFalse("入口丢弃不得上报错误", obs.hasError());
        assertEquals("入口即丢弃 ⇒ 不得进入帧处理（不取旋转）", 0, rotCalls[0]);
        assertTrue("帧仍须经 finally 或入口早退关闭释放", image.closeCalls >= 1);
    }
}
