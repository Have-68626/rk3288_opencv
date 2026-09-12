package com.example.rk3288_opencv;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotSame;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

/**
 * FaceRuntimeStatus（任务 14）单元测试：首启未导入态、就绪态、错误态，以及
 * native {@code getRuntimeStatus} JSON 的合并（含容错）。
 */
public class FaceRuntimeStatusTest {

    @Test
    public void notImportedCarriesGuidanceAndSourceLabel() {
        FaceRuntimeStatus s = FaceRuntimeStatus.notImported(0);
        assertTrue(s.isNotImported());
        assertFalse(s.isReady());
        assertFalse(s.isUserImported());
        assertEquals("未导入", s.sourceLabel());
        assertEquals(0, s.importedProfileCount);
        assertTrue("引导文案应提示导入 ZIP", s.message.contains("导入"));
        assertTrue("引导文案应说明 APK 不内置权重", s.message.contains("不内置"));
        assertTrue(s.profileId.isEmpty());
    }

    @Test
    public void readyReportsUserImported() {
        FaceRuntimeStatus s = FaceRuntimeStatus.ready("rk3288_light", "RK3288 Light", "1", "lic",
                2, false, 0);
        assertTrue(s.isReady());
        assertTrue(s.isUserImported());
        assertEquals("用户导入", s.sourceLabel());
        assertEquals("rk3288_light", s.profileId);
        assertEquals("RK3288 Light", s.displayName);
        assertEquals("lic", s.licenseId);
        assertEquals(2, s.importedProfileCount);
    }

    @Test
    public void errorIsNotReadyAndNotUserImported() {
        FaceRuntimeStatus s = FaceRuntimeStatus.error("p1", 1, "激活配置目录缺失：p1");
        assertEquals(FaceRuntimeStatus.State.ERROR, s.state);
        assertFalse(s.isReady());
        assertFalse(s.isUserImported());
        assertEquals("不可用", s.sourceLabel());
        assertEquals("p1", s.profileId);
    }

    @Test
    public void withNativeStatusMergesProfileLoadedAndDim() {
        FaceRuntimeStatus base = FaceRuntimeStatus.notImported(1);
        FaceRuntimeStatus merged = base.withNativeStatus(
                "{\"ok\":true,\"profileLoaded\":true,\"profileId\":\"p1\",\"embeddingDim\":512,"
                        + "\"galleryLoaded\":false,\"galleryCount\":0,\"enrollmentState\":\"IDLE\"}");
        assertNotSame(base, merged);
        assertTrue(merged.nativeProfileActive);
        assertEquals(512, merged.nativeEmbeddingDim);
        // 合并只补 native 字段，不改变模型状态语义。
        assertEquals(FaceRuntimeStatus.State.NOT_IMPORTED, merged.state);
    }

    @Test
    public void withNativeStatusReportsFalseWhenNotLoaded() {
        FaceRuntimeStatus merged = FaceRuntimeStatus.notImported(0)
                .withNativeStatus("{\"ok\":true,\"profileLoaded\":false,\"embeddingDim\":0}");
        assertFalse(merged.nativeProfileActive);
        assertEquals(0, merged.nativeEmbeddingDim);
    }

    @Test
    public void withNativeStatusToleratesMalformedOrNullInput() {
        FaceRuntimeStatus base = FaceRuntimeStatus.notImported(0);
        assertSame(base, base.withNativeStatus(null));
        assertSame(base, base.withNativeStatus(""));
        assertSame(base, base.withNativeStatus("not-json"));
        assertSame(base, base.withNativeStatus("{\"ok\":false}"));
        // 非对象 JSON 也不能崩。
        assertSame(base, base.withNativeStatus("[1,2,3]"));
    }

    @Test
    public void toStringDoesNotLeakSensitiveContent() {
        FaceRuntimeStatus s = FaceRuntimeStatus.ready("p1", "N", "1", "lic", 1, true, 512);
        String str = s.toString();
        assertTrue(str.contains("READY"));
        assertTrue(str.contains("p1"));
    }
}
