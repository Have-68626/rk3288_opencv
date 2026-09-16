package com.example.rk3288_opencv;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * FaceModelManager（任务 14）单元测试：SAF ZIP 导入安全（Zip Slip / 文件数 / 256MiB 上限）、
 * manifest 契约校验、sha256 引脚、原子启用、native 自检门禁、失败保留原配置、profile 切换。
 *
 * <p>native 依赖通过 {@link FaceModelManager.NativeRuntime} 假实现注入，因此可在纯 JVM 运行，
 * 不加载 so。限额通过包内构造器注入小值，避免真的构造 256MiB 数据。
 */
public class FaceModelManagerTest {

    private static final String ABI = "arm64-v8a";
    private static final String DET_PARAM = "scrfd.param";
    private static final String DET_BIN = "scrfd.bin";
    private static final String EMB_PARAM = "arcface.param";
    private static final String EMB_BIN = "arcface.bin";

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    // ---------- ① 首启未导入 ----------

    @Test
    public void firstLaunchWithoutBuiltinModelsIsNotImported() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        FaceRuntimeStatus st = m.status();
        assertTrue(st.isNotImported());
        assertEquals("未导入", st.sourceLabel());
        assertTrue(st.message.contains("导入"));
        assertTrue(m.listProfiles().isEmpty());
        assertNull(m.activeProfileId());
    }

    // ---------- ② 成功导入 ----------

    @Test
    public void importZipExtractsVerifiesNativedAndActivates() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic-1");

        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertTrue(r.message, r.isOk());
        assertEquals(FaceModelManager.Status.OK, r.status);

        assertEquals("p1", m.activeProfileId());
        assertTrue(new File(m.profileDir("p1"), FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertTrue(new File(m.profileDir("p1"), DET_BIN).isFile());
        assertTrue("必须把 manifest 原文交给 native 自检", rt.lastManifest.contains("\"profileId\":\"p1\""));

        FaceRuntimeStatus st = m.status();
        assertTrue(st.isReady());
        assertEquals("用户导入", st.sourceLabel());
        assertEquals("p1", st.profileId);
        assertEquals(1, st.importedProfileCount);
        assertTrue("native 状态应叠加", st.nativeProfileActive);
        assertEquals(512, st.nativeEmbeddingDim);
        assertNoTempArtifacts(dir);
    }

    // ---------- ③ Zip Slip ----------

    @Test
    public void importRejectsZipSlipEntries() throws Exception {
        for (String evil : new String[]{"../evil.txt", "/abs.txt", "a/../../evil.txt", "..\\evil.txt"}) {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> files = defaultFiles("v1");
            String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
            Map<String, byte[]> entries = entries("", manifest, files);
            entries.put(evil, "x".getBytes(StandardCharsets.UTF_8));

            FaceModelManager.Result r = m.importZip(stream(zip(entries)), ABI, true);
            assertEquals("应拒绝条目: " + evil, FaceModelManager.Status.ZIP_SLIP, r.status);
            assertTrue(r.runtimeStatus.isNotImported());
            assertFalse("不得写出 profile 目录", m.profileDir("p1").exists());
            assertNoTempArtifacts(dir);
        }
    }

    /** 审计 #8：目录条目（以 '/' 结尾）必须先过路径校验，不能让 "../evil/" 从 continue 绕过。 */
    @Test
    public void importRejectsZipSlipDirectoryEntries() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        Map<String, byte[]> entries = entries("", manifest, files);
        entries.put("../evil/", new byte[0]);  // 目录条目

        FaceModelManager.Result r = m.importZip(stream(zip(entries)), ABI, true);
        assertEquals(FaceModelManager.Status.ZIP_SLIP, r.status);
        assertFalse("不得写出 profile 目录", m.profileDir("p1").exists());
        assertNoTempArtifacts(dir);
    }

    /** 前移校验不得误伤合法目录条目（如 "rk3288_light/"）。 */
    @Test
    public void importAcceptsSafeDirectoryEntries() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("nested", "[\"arm64-v8a\"]", files, "lic");
        Map<String, byte[]> entries = entries("rk3288_light/", manifest, files);
        entries.put("rk3288_light/", new byte[0]);  // 合法目录条目

        FaceModelManager.Result r = m.importZip(stream(zip(entries)), ABI, true);
        assertTrue(r.message, r.isOk());
        assertTrue(new File(m.profileDir("nested"), FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertNoTempArtifacts(dir);
    }

    // ---------- ④ 文件数 / 大小上限 ----------

    @Test
    public void importRejectsTooManyFiles() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt,
                new FaceModelManager.Limits(3, FaceModelManager.MAX_TOTAL_BYTES, 1024 * 1024));
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.TOO_MANY_FILES, r.status);
        assertNoTempArtifacts(dir);
    }

    @Test
    public void importRejectsTotalSizeOverLimit() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        // 故意把总上限设为 8 字节，四个模型文件必然超限。
        FaceModelManager m = new FaceModelManager(dir, rt,
                new FaceModelManager.Limits(64, 8, 1024));
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.TOO_LARGE, r.status);
        assertNoTempArtifacts(dir);
    }

    // ---------- ⑤ manifest 定位/校验 ----------

    @Test
    public void importRejectsMissingManifest() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        FaceModelManager.Result r = m.importZip(stream(zip(defaultFiles("v1"))), ABI, true);
        assertEquals(FaceModelManager.Status.MISSING_MANIFEST, r.status);
    }

    @Test
    public void importRejectsMultipleManifests() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        Map<String, byte[]> entries = new LinkedHashMap<>();
        entries.put("a/manifest.json", manifest.getBytes(StandardCharsets.UTF_8));
        entries.put("b/manifest.json", manifest.getBytes(StandardCharsets.UTF_8));
        for (Map.Entry<String, byte[]> e : files.entrySet()) {
            entries.put(e.getKey(), e.getValue());
        }
        assertEquals(FaceModelManager.Status.MANIFEST_INVALID,
                m.importZip(stream(zip(entries)), ABI, true).status);
    }

    @Test
    public void importRejectsAbiMismatch() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"armeabi-v7a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.ABI_MISMATCH, r.status);
        assertFalse(r.runtimeStatus.isReady());
    }

    @Test
    public void importRejectsSha256Mismatch() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        // 先按原始内容生成引脚，再篡改 bin 内容（引脚不变）。
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        files.put(DET_BIN, "tampered-content".getBytes(StandardCharsets.UTF_8));
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.SHA256_MISMATCH, r.status);
    }

    @Test
    public void importRejectsMissingModelFile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        files.remove(DET_BIN);  // manifest 仍引用它
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.MISSING_MODEL_FILE, r.status);
    }

    @Test
    public void importRejectsInvalidManifestJson() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        FaceModelManager.Result r = m.importZip(stream(zip("", "{not-json", files)), ABI, true);
        assertEquals(FaceModelManager.Status.MANIFEST_INVALID, r.status);
    }

    /** 审计 #5：超深嵌套 manifest 必须受控失败（MANIFEST_INVALID），不得抛 StackOverflowError。 */
    @Test
    public void importRejectsDeeplyNestedManifestWithoutCrash() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        int depth = FaceModelManifest.Json.MAX_DEPTH + 10;
        StringBuilder sb = new StringBuilder(depth * 2);
        for (int i = 0; i < depth; i++) sb.append('[');
        for (int i = 0; i < depth; i++) sb.append(']');

        FaceModelManager.Result r = m.importZip(stream(zip("", sb.toString(), files)), ABI, true);
        assertEquals(FaceModelManager.Status.MANIFEST_INVALID, r.status);
        assertFalse(m.profileDir("p1").exists());
        assertNoTempArtifacts(dir);
    }

    // ---------- ⑥ 许可确认 ----------

    @Test
    public void importRequiresLicenseAcceptance() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, false);
        assertEquals(FaceModelManager.Status.LICENSE_NOT_ACCEPTED, r.status);
        assertFalse("未确认许可不得写盘", m.profileDir("p1").exists());
        assertNoTempArtifacts(dir);
    }

    // ---------- ⑦ native 校验/暂存门禁 ----------

    @Test
    public void nativeValidateFailureKeepsNotImported() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        rt.ok = false;
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.NATIVE_CHECK_FAILED, r.status);
        assertNull("native 失败不得设为当前", m.activeProfileId());
        assertFalse("native 失败不得落盘 profile 目录", m.profileDir("p1").exists());
        assertTrue(m.status().isNotImported());
        assertNoTempArtifacts(dir);
    }

    @Test
    public void nativeUnavailableIsTreatedAsSelfCheckFailure() throws Exception {
        FaceModelManager.NativeRuntime throwing = new FaceModelManager.NativeRuntime() {
            @Override
            public String loadProfile(String manifestJson, String currentAbi) {
                throw new UnsatisfiedLinkError("no native-lib");
            }

            @Override
            public String getRuntimeStatus() {
                throw new UnsatisfiedLinkError("no native-lib");
            }
        };
        FaceModelManager m = new FaceModelManager(tmp.newFolder(), throwing);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.NATIVE_CHECK_FAILED, r.status);
        // status() 在 native 不可用时也必须能返回，而不是崩溃。
        assertTrue(m.status().isNotImported());
    }

    /** P1-2：loadProfile 只回 ok 而无 validated 时，不得视为校验通过。 */
    @Test
    public void loadProfileOkButNotValidatedFailsNativeGate() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        rt.validated = false;
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals("仅 ok 而无 validated 不得视为校验通过",
                FaceModelManager.Status.NATIVE_CHECK_FAILED, r.status);
        assertFalse("校验失败不得落盘 profile 目录", m.profileDir("p1").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * P1-2 核心：manifest 已校验/暂存（profileValidated=true）但实时管线未生效
     * （effective=false，如 Engine 未创建 reason=pending）——status 必须区分「已校验」与「已加载生效」，
     * 绝不把仅校验暂存误报为就绪生效。
     */
    @Test
    public void validatedButPipelineNotEffectiveIsDistinguishedFromActive() throws Exception {
        FaceModelManager.NativeRuntime rt = new FaceModelManager.NativeRuntime() {
            @Override
            public String loadProfile(String manifestJson, String currentAbi) {
                return "{\"ok\":true,\"validated\":true,\"staged\":true,\"effective\":false}";
            }

            @Override
            public String getRuntimeStatus() {
                return "{\"ok\":true,\"profileValidated\":true,\"effective\":false,"
                        + "\"reason\":\"pending\",\"embeddingDim\":512}";
            }

            @Override
            public String activateRuntime(String manifestJson, String baseDir, String currentAbi,
                                          String galleryDir) {
                // Engine 尚未创建：pending（ok=true/effective=false），导入本身应成功。
                return "{\"ok\":true,\"pending\":true,\"effective\":false,\"reason\":\"pending\"}";
            }
        };
        FaceModelManager m = new FaceModelManager(tmp.newFolder(), rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertTrue("校验/落盘成功仍应导入成功（激活为 best-effort）", r.isOk());
        FaceRuntimeStatus st = m.status();
        assertTrue("manifest 已校验/暂存", st.nativeProfileValidated);
        assertFalse("仅校验暂存不得判为已加载生效", st.nativeProfileActive);
    }

    // ---------- ⑧ 失败保留原配置 ----------

    @Test
    public void failedImportKeepsPreviousActiveProfile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> a = defaultFiles("A");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", a, "lic"), a)), ABI, true).isOk());
        assertEquals("pA", m.activeProfileId());

        rt.ok = false;
        Map<String, byte[]> b = defaultFiles("B");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("pB", "[\"arm64-v8a\"]", b, "lic"), b)), ABI, true);
        assertEquals(FaceModelManager.Status.NATIVE_CHECK_FAILED, r.status);
        assertEquals("失败必须保留原激活 profile", "pA", m.activeProfileId());
        assertTrue(m.status().isReady());
        assertFalse("失败的 profile 不得落盘", m.profileDir("pB").exists());
    }

    // ---------- ⑨ 切换 profile ----------

    @Test
    public void activateSwitchesCurrentProfile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> a = defaultFiles("A");
        Map<String, byte[]> b = defaultFiles("B");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", a, "lic"), a)), ABI, true).isOk());
        assertTrue(m.importZip(stream(zip("", manifest("pB", "[\"arm64-v8a\"]", b, "lic"), b)), ABI, true).isOk());
        assertEquals("最后导入者成为当前", "pB", m.activeProfileId());
        assertEquals(2, m.listProfiles().size());
        assertEquals(2, m.status().importedProfileCount);

        FaceModelManager.Result r = m.activate("pA", ABI, true);
        assertTrue(r.message, r.isOk());
        assertEquals("pA", m.activeProfileId());
        assertEquals("pA", m.status().profileId);
    }

    @Test
    public void failedActivateKeepsPreviousProfile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> a = defaultFiles("A");
        Map<String, byte[]> b = defaultFiles("B");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", a, "lic"), a)), ABI, true).isOk());
        assertTrue(m.importZip(stream(zip("", manifest("pB", "[\"arm64-v8a\"]", b, "lic"), b)), ABI, true).isOk());
        assertEquals("pB", m.activeProfileId());

        rt.ok = false;
        FaceModelManager.Result r = m.activate("pA", ABI, true);
        assertEquals(FaceModelManager.Status.NATIVE_CHECK_FAILED, r.status);
        assertEquals("切换失败必须保留原配置", "pB", m.activeProfileId());
    }

    @Test
    public void activateUnknownOrUnacceptedProfileIsRejected() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        assertEquals(FaceModelManager.Status.PROFILE_NOT_FOUND, m.activate("nope", ABI, true).status);
        assertEquals(FaceModelManager.Status.LICENSE_NOT_ACCEPTED, m.activate("nope", ABI, false).status);
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT, m.activate("../x", ABI, true).status);
    }

    // ---------- ⑩ 重新导入（原子替换）与嵌套目录 ----------

    @Test
    public void reimportSameProfileReplacesContentAtomically() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        Map<String, byte[]> v2 = defaultFiles("v2-different");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true).isOk());
        assertEquals("同一 profile 重新导入应只有一份", 1, m.listProfiles().size());

        byte[] newBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        assertFalse("重新导入应替换内容", java.util.Arrays.equals(oldBin, newBin));
        assertNoTempArtifacts(m.storeDir());
    }

    /**
     * 审计 #6：指针写入失败触发目录回滚时，回滚 rename 的返回值必须被检查且状态一致——
     * 旧目录恢复就位、指针不变、内容为旧版本、不残留 .bak_&lt;id&gt;。
     */
    @Test
    public void pointerWriteFailureRollsBackDirectoryAndKeepsOldProfile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        // 制造 persistActive 失败现场：active_profile.bak 占为非空目录，dst->bak rename 必然失败。
        File stubbornBak = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE + ".bak");
        assertTrue(stubbornBak.mkdirs());
        assertTrue(new File(stubbornBak, "keep").createNewFile());

        Map<String, byte[]> v2 = defaultFiles("v2-different");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
        assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
        assertEquals("指针未改写，旧 profile 仍激活", "p1", m.activeProfileId());
        assertTrue("旧目录必须回滚就位", m.profileDir("p1").isDirectory());
        assertTrue("回滚后内容应为旧版本", java.util.Arrays.equals(oldBin,
                readFile(new File(m.profileDir("p1"), DET_BIN))));
        assertFalse("不得滞留 .bak_p1", new File(dir, ".bak_p1").exists());
    }

    @Test
    public void importSupportsManifestInsideTopLevelDirectory() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("nested", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("rk3288_light/", manifest, files)), ABI, true);
        assertTrue(r.message, r.isOk());
        assertTrue(new File(m.profileDir("nested"), FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertTrue(new File(m.profileDir("nested"), DET_PARAM).isFile());
    }

    // ---------- ⑪ 状态 ----------

    @Test
    public void statusReportsErrorWhenActiveDirDeleted() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        deleteRecursively(m.profileDir("p1"));
        FaceRuntimeStatus st = m.status();
        assertEquals(FaceRuntimeStatus.State.ERROR, st.state);
        assertEquals("不可用", st.sourceLabel());
        assertTrue(st.message.contains("p1"));
    }

    @Test
    public void invalidImportArgumentsRejected() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT, m.importZip(null, ABI, true).status);
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT,
                m.importZip(stream(new byte[0]), "", true).status);
    }

    // ---------- ⑫ 第二波 #2：激活失败不丢旧配置、不谎报成功 ----------

    /** 第二波 #2：切换时 activateRuntime 失败 -> 指针回滚/不变、旧配置保留、返回失败。 */
    @Test
    public void activateRuntimeFailureKeepsPreviousProfileAndReportsFailure() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> a = defaultFiles("A");
        Map<String, byte[]> b = defaultFiles("B");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", a, "lic"), a)), ABI, true).isOk());
        assertTrue(m.importZip(stream(zip("", manifest("pB", "[\"arm64-v8a\"]", b, "lic"), b)), ABI, true).isOk());
        assertEquals("pB", m.activeProfileId());

        // 切换 pA 时 native reload 失败。
        rt.activateOk = false;
        rt.activateReason = "missing_model";
        FaceModelManager.Result r = m.activate("pA", ABI, true);
        assertEquals(FaceModelManager.Status.RUNTIME_ACTIVATE_FAILED, r.status);
        assertFalse("激活失败不得报成功", r.isOk());
        assertEquals("激活失败必须保留原激活 profile", "pB", m.activeProfileId());
        assertEquals("旧配置应继续就绪", "pB", m.status().profileId);
        assertTrue(m.status().isReady());
    }

    /** 第二波 #2：新导入激活失败 -> 保留新目录但不切指针，返回明确失败状态。 */
    @Test
    public void importZipActivationFailureKeepsDirButDoesNotSwitchPointer() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        rt.activateOk = false;
        rt.activateReason = "missing_model";
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertEquals(FaceModelManager.Status.RUNTIME_ACTIVATE_FAILED, r.status);
        assertNull("激活失败不得切指针", m.activeProfileId());
        assertTrue("新目录应保留（已导入）", m.profileDir("p1").isDirectory());
        assertTrue(m.listProfiles().contains("p1"));
        // #1-Low-2：首次导入失败无旧配置，文案不得误报「已保留原配置」。
        assertFalse("首次导入失败不得谎称保留原配置", r.message.contains("已保留原配置"));
        assertTrue("首次导入失败文案应说明已保留新目录但未激活",
                r.message.contains("已保留新导入的模型目录但未激活"));
        assertNoTempArtifacts(dir);
    }

    // ---------- ⑬ 第二波 #3：持久化崩溃恢复 ----------

    /** 第二波 #3：激活指针主文件缺失 + .bak 存在 -> status() 恢复主指针并 READY。 */
    @Test
    public void statusRecoversActivePointerFromBackup() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        File pointer = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE);
        File bak = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE + ".bak");
        // 模拟 persistActive 在 dst→bak 之后、tmp→dst 完成前崩溃。
        assertTrue(pointer.renameTo(bak));
        assertFalse(pointer.exists());

        FaceRuntimeStatus st = m.status();
        assertTrue("应从 .bak 恢复激活指针", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("恢复后主指针应就位", pointer.isFile());
        assertFalse("恢复后备份应被消费", bak.exists());
        assertNoTempArtifacts(dir);
    }

    /** 第二波 #3：激活目录缺失 + .bak_<id> 存在 -> 恢复目录就位并 READY。 */
    @Test
    public void statusRecoversActiveDirFromSwapBackup() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        File profileDir = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        // 模拟 swapInProfileDirs 在 target→bak 之后、base→target 之前崩溃。
        assertTrue(profileDir.renameTo(bak));
        assertFalse(profileDir.exists());

        FaceRuntimeStatus st = m.status();
        assertTrue("应从 .bak_p1 恢复激活目录", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("恢复后目录应就位", profileDir.isDirectory());
        assertTrue(new File(profileDir, FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertFalse(bak.exists());
    }

    /** 第二波 #3：过渡残留（active_profile.tmp / .tmp_*）不得影响指针与 profile 列表。 */
    @Test
    public void strayTempArtifactsAreIgnored() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        File pointerTmp = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE + ".tmp");
        writeText(pointerTmp, "bogus");
        File strayTmp = new File(dir, ".tmp_stray");
        assertTrue(strayTmp.mkdirs());
        assertTrue(new File(strayTmp, FaceModelManager.MANIFEST_FILE_NAME).createNewFile());

        assertEquals("残留 .tmp 不得被当作激活指针", "p1", m.activeProfileId());
        assertTrue(m.status().isReady());
        assertFalse("点号开头的过渡目录不得计入 profile 列表",
                m.listProfiles().contains(".tmp_stray"));
        assertEquals(1, m.listProfiles().size());
    }

    /** 第二波 #3：目录替换窗口崩溃恢复后，activate 不得丢失已恢复的配置。 */
    @Test
    public void activateAfterDirBackupRecoveryKeepsConfig() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> a = defaultFiles("A");
        Map<String, byte[]> b = defaultFiles("B");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", a, "lic"), a)), ABI, true).isOk());
        assertTrue(m.importZip(stream(zip("", manifest("pB", "[\"arm64-v8a\"]", b, "lic"), b)), ABI, true).isOk());
        assertEquals("pB", m.activeProfileId());

        // 制造 pA 目录替换窗口崩溃现场：pA 缺失 + .bak_pA 存在。
        File dirA = m.profileDir("pA");
        assertTrue(dirA.renameTo(new File(dir, ".bak_pA")));

        // activate 内部走恢复路径，切换回 pA 且不丢配置。
        FaceModelManager.Result r = m.activate("pA", ABI, true);
        assertTrue(r.message, r.isOk());
        assertEquals("pA", m.activeProfileId());
        assertTrue("恢复后的 pA 应可用", m.status().isReady());
        assertEquals("pA", m.status().profileId);
        assertEquals(2, m.listProfiles().size());
    }

    // ---------- ⑭ 第三波审查 #1：同名覆盖激活失败必须回滚旧模型 ----------

    /**
     * 第三波 #1：同名 profileId 重新导入 + activateRuntime 失败 -> 旧目录内容保留、active_profile 未变、
     * 返回失败、无 .bak_&lt;id&gt; 残留。与 {@link #reimportSameProfileReplacesContentAtomically}（成功改名）
     * 形成「成功改名 / 失败回滚」对称覆盖。
     */
    @Test
    public void reimportSameProfileActivationFailureRollsBackToOldModel() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        assertEquals("p1", m.activeProfileId());

        // 同名覆盖：native 实时管线激活失败。
        rt.activateOk = false;
        rt.activateReason = "missing_model";
        Map<String, byte[]> v2 = defaultFiles("v2-different");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
        assertEquals(FaceModelManager.Status.RUNTIME_ACTIVATE_FAILED, r.status);
        assertFalse("激活失败不得报成功", r.isOk());
        assertEquals("同名覆盖失败必须保留原激活 profile", "p1", m.activeProfileId());
        assertTrue("旧目录必须回滚就位", m.profileDir("p1").isDirectory());
        assertTrue("同名覆盖失败必须保留旧模型内容（不得删除旧目录）",
                java.util.Arrays.equals(oldBin, readFile(new File(m.profileDir("p1"), DET_BIN))));
        // #1-Low-2：同名覆盖失败有旧配置，文案应明确「已回滚到原模型」。
        assertTrue("同名覆盖失败文案应说明已回滚到原模型",
                r.message.contains("已回滚到原模型"));
        assertFalse("不得滞留 .bak_p1", new File(dir, ".bak_p1").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 第三波 #1b：同名覆盖激活失败 + {@code rollbackProfileSwap()} 的还原 rename 也失败时，磁盘会留下
     * 「target 缺失 + {@code .bak_<id>} 存在 + 指针仍指同名 &lt;profileId&gt;」的滞留现场——这正是新的
     * 失败分支（返回 {@link FaceModelManager.Status#ATOMIC_RENAME_FAILED} 并保留 {@code .bak_<id>}）
     * 所依赖、并对外承诺「将由启动恢复」的保证：{@code status()}-&gt;{@code recoverProfileDirIfNeeded()}
     * 必须把滞留的**旧模型**目录还原就位，绝不误报就绪、绝不加载到失败/空目录。
     *
     * <p><b>为何本用例直接构造磁盘现场</b>：写它的时候（第三波审查）尚无文件操作接缝。**该理由现已过期**
     * ——任务 A 引入了 {@code FaceModelManager.FileOps}（{@code setFileOpsForTesting}），
     * {@code rollbackProfileSwap} 的 rename 现走 {@code fileOps.rename}，本文件后续的
     * {@code renameFailureDuringDirSwapRestoresOldConfigAndClearsTxn} 等用例正是用它注入失败。
     * 本用例保留「构造终态」的写法，仅因它验证的是**恢复契约**（读出该现场后如何收敛），
     * 而非写入器自身的失败分支。
     */
    @Test
    public void sameNameRollbackFailureStrandsBackupThenStatusRecoversOldModel() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        assertEquals("p1", m.activeProfileId());

        // 构造同名覆盖回滚失败的滞留现场：新 target 已被删除、旧目录滞留 .bak_p1、指针未改写。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        assertFalse("回滚失败后 target 应缺失", target.exists());
        assertTrue("旧目录应滞留于 .bak_p1", bak.isDirectory());
        assertEquals("指针未改写，仍是同名 p1", "p1", m.activeProfileId());

        // 新分支承诺「将由启动恢复」：status() 兜底把滞留备份还原为激活目录，且还原的是旧模型内容；
        // 该现场绝不能被误报为「未导入」（否则会引导用户重导、掩盖滞留的旧模型）。
        FaceRuntimeStatus st = m.status();
        assertFalse("滞留现场不得被当作未导入", st.isNotImported());
        assertTrue("应从 .bak_p1 兜底还原后就绪", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("兜底必须还原旧模型内容（不得误切成失败/空目录）", java.util.Arrays.equals(oldBin,
                readFile(new File(target, DET_BIN))));
        assertTrue("还原后 manifest 应就位",
                new File(target, FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertFalse("还原后不得残留 .bak_p1", bak.exists());
        assertNoTempArtifacts(dir);
    }

    // ---------- ⑮ 第三波审查 #3：statusSnapshot() 无锁读缓存 ----------

    /**
     * 第三波 #3：{@code statusSnapshot()} 不加目录锁、纯读缓存；首帧无缓存返回「加载中」占位
     * （#3-Low-1：专用 LOADING 态，不得被当作「未导入」），后台 {@code status()} 刷新后快照应反映最新状态。
     */
    @Test
    public void statusSnapshotIsLockFreeCacheWithLoadingPlaceholder() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        // 首次读取：尚无后台快照 -> 加载中占位（绝不触发文件 IO/加锁）。
        FaceRuntimeStatus snap0 = m.statusSnapshot();
        assertNotNull(snap0);
        assertFalse("首帧占位不得报就绪", snap0.isReady());
        // #3-Low-1：占位必须是 LOADING（而非 NOT_IMPORTED），否则冷启动会误显「模型来源=未导入」。
        assertTrue("首帧占位应为加载中态", snap0.isLoading());
        assertEquals(FaceRuntimeStatus.State.LOADING, snap0.state);
        assertFalse("加载中不得被当作未导入", snap0.isNotImported());
        assertFalse("加载中不得被判为用户已导入", snap0.isUserImported());
        assertEquals("加载中", snap0.sourceLabel());
        assertTrue("占位文案应提示正在加载", snap0.message.contains("加载"));

        // 后台刷新（status() 写快照）后，快照反映最新状态。
        FaceRuntimeStatus fresh = m.status();
        assertTrue(fresh.isReady());
        FaceRuntimeStatus snap1 = m.statusSnapshot();
        assertTrue("快照应反映上次 status() 结果", snap1.isReady());
        assertEquals("p1", snap1.profileId);
    }

    // ---------- ⑯ 第三波审查 #3b：回滚 rename 失败的兜底 ----------

    /**
     * 第三波 #3b：{@code rollbackProfileSwap()} 的恢复 rename 失败（新 target 已删、旧目录滞留
     * {@code .bak_<id>}、激活指针仍指该 profile）后，{@code buildStatus()} 的
     * {@code recoverProfileDirIfNeeded()} 必须兜底：把滞留备份还原为激活目录，且状态与实际目录一致
     * （还原的是**旧模型内容**、不误报就绪）。
     *
     * <p><b>场景构造说明</b>：无法在纯 JVM 下稳定地让 {@code rollbackProfileSwap} 内部的
     * {@code bak.renameTo(target)} 失败——该 rename 发生在「删除新 target → 还原旧 bak」之间的极短
     * 窗口内，测试无法注入句柄；若改用「持有独占文件句柄令 rename 失败」构造，则依赖 Windows 文件锁
     * 语义，而 Android Gradle 单测跑在 Linux（持句柄仍可 rename 目录），跨平台 flaky。故按**等价**方式
     * 直接构造「target 缺失 + {@code .bak_<id>} 存在」这一回滚失败后的磁盘现场，再调 {@code status()}
     * 验证兜底路径。它与 {@link #statusRecoversActiveDirFromSwapBackup}（崩溃窗口）共用同一
     * {@code recoverProfileDirIfNeeded} 契约；此处额外断言还原的是**旧模型内容**、状态与真实目录一致。
     */
    @Test
    public void rollbackRenameFailureStrandedBackupIsRecoveredByStatus() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        assertEquals("p1", m.activeProfileId());

        // 构造 rollbackProfileSwap 恢复失败的现场：旧目录滞留 .bak_p1，target 缺失，指针未变。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        assertFalse("回滚失败后 target 应缺失", target.exists());
        assertTrue("旧目录应滞留于 .bak_p1", bak.isDirectory());

        // 兜底：buildStatus -> recoverProfileDirIfNeeded 把 .bak_p1 还原为激活目录。
        FaceRuntimeStatus st = m.status();
        assertTrue("应从 .bak_p1 还原激活目录后就绪", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("还原后 target 目录应就位", target.isDirectory());
        assertTrue("还原后必须保留旧模型内容（不得误切成新/空目录）",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertTrue("还原后 manifest 应就位",
                new File(target, FaceModelManager.MANIFEST_FILE_NAME).isFile());
        assertFalse("还原后不得残留 .bak_p1", bak.exists());
        // 状态与实际目录一致：就绪当且仅当激活目录存在且含 manifest。
        assertEquals("状态必须与真实目录一致",
                st.isReady(), target.isDirectory()
                        && new File(target, FaceModelManager.MANIFEST_FILE_NAME).isFile());
    }

    /**
     * 第三波 #3b（负例）：兜底恢复 rename 成功后，若还原出的目录并非有效 profile（缺 manifest），
     * {@code buildStatus()} 绝不能因此误报就绪——必须落到 ERROR（目录无效），与真实目录一致。
     * 直接守住 {@code recoverProfileDirIfNeeded} 之后的 {@code !mf.isFile()} 判定分支。
     */
    @Test
    public void recoveredBackupWithoutManifestIsNotReportedReady() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> files = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true).isOk());

        // 构造 target 缺失 + .bak_p1 存在但内容无效（无 manifest）的兜底现场。
        File target = m.profileDir("p1");
        deleteRecursively(target);
        File bak = new File(dir, ".bak_p1");
        assertTrue(bak.mkdirs());

        FaceRuntimeStatus st = m.status();
        assertFalse("还原目录缺 manifest 不得误报就绪", st.isReady());
        assertEquals(FaceRuntimeStatus.State.ERROR, st.state);
        assertTrue("应如实说明激活配置目录无效/缺失", st.message.contains("p1"));
        assertTrue("兜底应把备份还原为 target 目录", target.isDirectory());
        assertFalse(".bak_p1 应已被消费", bak.exists());
        assertFalse("target 无 manifest，状态与实际目录一致（不就绪）",
                new File(target, FaceModelManager.MANIFEST_FILE_NAME).isFile());
    }

    // ---------- ⑰ 第五轮审查 P1：回滚时新目录删不掉 -> 可恢复的事务标记 ----------

    /**
     * 第五轮审查 P1：同名覆盖激活失败 + {@code rollbackProfileSwap()} 的**新目录删除**失败时，
     * {@code deleteRecursivelyQuietly(swap.target)} 吞掉失败，于是 target 与 {@code .bak_<id>}
     * **同时存在**；而 {@code recoverProfileDirIfNeeded} 的判据是「target 缺失」，原实现**永远不会触发**——
     * 旧模型永久滞留，激活指针却指向那个激活失败的新包（{@code buildStatus} 还会把它误报成就绪）。
     *
     * <p>修复后：回滚时写入 {@code .rollback_<id>} 意图标记，恢复逻辑据此先删新目录、再还原备份。
     * 本用例直接构造「target 存在 + bak 存在 + 标记存在」现场（删除失败无法在纯 JVM 稳定注入，
     * 与 {@link #rollbackRenameFailureStrandedBackupIsRecoveredByStatus()} 同法按等价方式构造）。
     *
     * <p>关键 RED 断点：修复前 target 内是一个**校验合法的新包**，{@code status()} 会把它当成激活配置
     * 而报 ready——正是审查指出的「把失败的新目录当作激活配置」。
     */
    @Test
    public void rollbackWithUndeletableTargetIsRecoveredByMarkerAndRestoresOldModel() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        assertEquals("p1", m.activeProfileId());

        // 回滚失败现场：旧目录移开、新的（激活失败的）包占据 target，故 target 删不掉。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue("旧目录应移开", target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        // 回滚删除失败必然留下意图标记（存在性即语义）。
        java.nio.file.Files.write(new File(dir, ".rollback_p1").toPath(),
                "target=p1\n".getBytes(StandardCharsets.UTF_8));

        FaceRuntimeStatus st = m.status();
        assertTrue("带标记的回滚现场必须由恢复逻辑还原旧模型后就绪", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("必须还原旧模型内容（绝不能把激活失败的新包当作激活配置）",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertFalse(".bak_p1 应已被消费", bak.exists());
        assertFalse("回滚完成后不得残留意图标记", new File(dir, ".rollback_p1").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 第五轮审查 P1（负例）：意图标记存在但**备份已不存在**——标记已无意义，必须清除；否则
     * {@code buildStatus} 会永久停在「回滚未完成」的 ERROR，用户即使重装模型也无法回到就绪态。
     */
    @Test
    public void rollbackMarkerWithoutBackupIsClearedAndDoesNotStickError() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        File marker = new File(dir, ".rollback_p1");
        java.nio.file.Files.write(marker.toPath(), "target=p1\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(marker.exists());

        FaceRuntimeStatus st = m.status();
        assertTrue("无备份可还原时不得卡在「回滚未完成」态", st.isReady());
        assertEquals("p1", st.profileId);
        assertFalse("失去意义的意图标记应被清除", marker.exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 第五轮审查 A3' δ5②：用户对同一 profileId **显式 activate 且成功**后，结果与状态必须一致
     * ——不得出现「Result OK 但 buildStatus 报『回滚未完成』ERROR」。
     *
     * <p><b>覆盖性质（如实说明）</b>：本用例是**不变量护栏**（Result OK ⇒ 状态不就绪即失败），
     * 而**不是**对 {@code activate()} 中新增的 {@code clearRollbackMarker} 那行的**判别性**证据——
     * 在本用例构造的「标记在 + 备份已消费」现场里，前置的 {@code recoverProfileDirIfNeeded} 已经
     * 把它清掉了，删掉那行测试仍会通过。真正需要那行的现场是「标记在 + 备份在 + target 删不掉」，
     * 而「删除失败」无法在纯 JVM 下跨平台稳定注入（Android Gradle 单测跑在 Linux，持句柄仍可
     * 删目录/文件；Windows 侧的只读文件语义不可移植）——与既有回滚用例同处的构造限制。
     */
    @Test
    public void activateClearsPendingRollbackMarkerSoResultAndStatusAgree() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        // 残留「回滚未完成」现场（标记在、备份已被消费：正是 A3' γ7 的收尾状态）。
        File marker = new File(dir, ".rollback_p1");
        java.nio.file.Files.write(marker.toPath(), "target=p1\n".getBytes(StandardCharsets.UTF_8));
        assertTrue(marker.exists());

        FaceModelManager.Result r = m.activate("p1", ABI, true);
        assertTrue("激活应成功", r.isOk());
        assertFalse("激活成功后不得残留回滚意图标记", marker.exists());
        FaceRuntimeStatus st = m.status();
        assertTrue("Result 为 OK 时状态不得仍是「回滚未完成」ERROR", st.isReady());
        assertEquals("p1", st.profileId);
        assertNoTempArtifacts(dir);
    }

    /**
     * 第六轮双代理审计（A1-P2 / A2-High）**回归护栏**：意图标记写不出去时，回滚**仍须尽力完成**。
     *
     * <p>背景：曾经把「标记写失败」当作**停手条件**（写不成功就不动磁盘）。双代理各自独立枚举
     * 「标记写失败」前提下的组合后一致判定该做法**更差**——它牺牲了两种本可自动还原的组合
     * （删除+改名都成功；或删除成功但改名失败 → target 缺失可由崩溃窗口恢复），却未换来任何改善；
     * 而它想避免的「target 在 + bak 在 + 无标记」现场，恰恰是「什么都不做」的必然结果。
     * 故标记只决定「失败后能否由启动恢复兜底」，**不得成为放弃还原的理由**。
     *
     * <p><b>可移植的构造</b>：把标记路径预建成**非空目录**，则 {@code new FileOutputStream(dir)}
     * 在 Windows 与 Linux 上**都**会抛异常（不像文件锁那样依赖平台语义）。此时删除与改名本可成功，
     * 故端状态必须是：**旧模型已还原到激活位**、返回 {@code RUNTIME_ACTIVATE_FAILED}（已回滚到原模型）。
     *
     * <p><b>判别力（如实界定）</b>：若有人重新引入「写失败即停手」，回滚不再执行 → 返回
     * {@code ATOMIC_RENAME_FAILED}，**唯有状态码断言会失败**——因为调用方的 {@code buildStatus}
     * 仍会经 {@code recoverProfileDirIfNeeded} 就地完成还原（本构造下标记目录 {@code exists()==true}，
     * 恰使恢复生效），故**内容断言会通过**。此点经第三代理实测确认，勿再声称「内容断言也会失败」。
     * <b>不能判别什么</b>：「删除成功但改名失败 → 崩溃窗口恢复」那条组合无法在纯 JVM 下稳定构造
     *（需 target 不可删）；真实「无标记」场景（标记文件建不出来而非被占成目录）的端状态亦与之不同。
     */
    @Test
    public void rollbackStillCompletesWhenMarkerCannotBeWritten() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        // 让标记必然写不进去：预建非空目录（它同时充当 recovery 的标记，故还原路径通畅）。
        File marker = new File(dir, ".rollback_p1");
        assertTrue(marker.mkdirs());
        assertTrue(new File(marker, "keep").createNewFile());

        // 同名覆盖 + 激活失败 → 进入 rollbackProfileSwap。
        rt.activateOk = false;
        rt.activateReason = "runtime_init_failed";
        Map<String, byte[]> v2 = defaultFiles("v2");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

        // 关键：记录失败不得阻止还原——必须走到「已回滚到原模型」而非「回滚失败」。
        assertEquals(FaceModelManager.Status.RUNTIME_ACTIVATE_FAILED, r.status);
        assertFalse(r.isOk());
        assertTrue("文案应说明已回滚到原模型，实际=" + r.message, r.message.contains("已回滚到原模型"));
        File target = m.profileDir("p1");
        assertTrue("旧模型必须已还原到激活位", target.isDirectory());
        assertTrue("激活位必须是旧模型内容，而非激活失败的新包",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        // 清理人造标记目录，避免污染后续断言。
        deleteRecursively(marker);
    }

    // ---------- ⑱ 任务 A：目录替换事务记录 ----------

    /** 可注入失败的文件操作替身（任务 A 的 FileOps 接缝）。 */
    private static final class FaultyFileOps implements FaceModelManager.FileOps {
        boolean failWrite;
        boolean failRename;
        boolean failDelete;
        /** 只让「从 .tmp_ 解压目录改名到目标」这一步失败（迁就其它改名，如回滚的 bak→target）。 */
        boolean failRenameFromTmp;

        @Override
        public boolean rename(File from, File to) {
            if (failRename) return false;
            if (failRenameFromTmp && from.getName().startsWith(".tmp_")) return false;
            return from.renameTo(to);
        }

        @Override
        public boolean deleteRecursively(File f) {
            return !failDelete && deleteRecursivelyReal(f);
        }

        @Override
        public boolean writeSync(File f, byte[] data) {
            if (failWrite) return false;
            try (java.io.FileOutputStream fos = new java.io.FileOutputStream(f, false)) {
                fos.write(data);
                fos.flush();
                return true;
            } catch (Exception e) {
                return false;
            }
        }
    }

    private static boolean deleteRecursivelyReal(File f) {
        if (!f.exists()) return true;
        if (f.isDirectory()) {
            File[] cs = f.listFiles();
            if (cs != null) for (File c : cs) deleteRecursivelyReal(c);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
        return !f.exists();
    }

    /** 构造事务记录文件（与 DirTxn.encode() 同格式）。 */
    private static void writeTxnFile(File dir, String profileId, boolean hadOld,
                                     String prevActive, boolean committed) throws IOException {
        String s = "profileId=" + profileId + "\n"
                + "hadOld=" + hadOld + "\n"
                + "prevActive=" + (prevActive == null ? "" : prevActive) + "\n"
                + "state=" + (committed ? "committed" : "in_progress") + "\n";
        java.nio.file.Files.write(new File(dir, ".txn_" + profileId).toPath(),
                s.getBytes(StandardCharsets.UTF_8));
    }

    /**
     * 未提交事务 + 备份存在 ⇒ 启动恢复**回滚到旧目录与原指针**。
     *
     * <p>这是事务记录的核心价值：`.rollback_` 只有「存在即回滚」语义，无法区分「替换进行中」与
     * 「已提交未清理」；有 state 才能判定该退旧还是留新。
     */
    @Test
    public void inProgressTxnRollsBackToOldModelAndPointer() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        // 构造「替换已开始但未提交」现场：旧目录移开、新包占位、事务记录为 in_progress。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);

        FaceRuntimeStatus st = m.status();
        assertTrue("未提交事务应恢复旧模型后就绪", st.isReady());
        assertEquals("p1", st.profileId);
        assertTrue("必须还原旧模型内容（不得保留未激活的新包）",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertFalse(".bak_p1 应已被消费", bak.exists());
        assertFalse("事务了结后不得残留记录", new File(dir, ".txn_p1").exists());
        assertNoTempArtifacts(dir);
    }

    /** 已提交事务 ⇒ **保留新目录**、清理备份、清记录（绝不误回滚一次成功的更新）。 */
    @Test
    public void committedTxnKeepsNewModelAndCleansBackup() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", true);   // committed

        FaceRuntimeStatus st = m.status();
        assertTrue(st.isReady());
        assertTrue("已提交 ⇒ 保留新目录内容",
                java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse("已提交 ⇒ 备份应被清理", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        assertNoTempArtifacts(dir);
    }

    /** 事务记录**写失败** ⇒ 中止导入且**旧配置一动未动**（Codex 要求 1）。 */
    @Test
    public void txnWriteFailureAbortsImportWithoutTouchingOldConfig() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

            ops.failWrite = true;   // 事务记录写不进去
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
            assertTrue("旧模型内容必须原样保留",
                    java.util.Arrays.equals(oldBin, readFile(new File(m.profileDir("p1"), DET_BIN))));
            assertFalse("不得留下事务记录", new File(dir, ".txn_p1").exists());
            assertFalse("不得残留备份", new File(dir, ".bak_p1").exists());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 「无事务记录且新旧目录并存」= **歧义现场**：不猜哪份应生效（Codex 要求 6）。
     *
     * <p>自动恢复 `bak` 会误撤销一次成功更新（成功路径的备份清理失败也会形成该并存），故只能报错。
     */
    @Test
    public void ambiguousCoexistenceWithoutTxnIsReportedNotAutoResolved() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        // 无事务记录、无回滚标记 → 歧义

        FaceRuntimeStatus st = m.status();
        assertFalse("歧义现场不得报就绪", st.isReady());
        assertTrue("应如实说明无法判定", st.message.contains("无法判定") || st.message.contains("并存"));
        assertTrue("必须保留两份数据（target）", target.isDirectory());
        assertTrue("必须保留两份数据（bak）", bak.isDirectory());
    }

    /**
     * 替换过程中「临时目录无法改名到 target」⇒ 就地还原旧配置且**结清事务记录**。
     *
     * <p>覆盖 Codex 要求的「改名失败」分支：这条路径在进程内回滚后必须清掉事务记录，否则残留的
     * 未提交记录会让后续 `buildStatus` 误报「事务未完成」。
     */
    @Test
    public void renameFailureDuringDirSwapRestoresOldConfigAndClearsTxn() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

            ops.failRenameFromTmp = true;   // 只让 base→target 这一步失败，回滚用的 bak→target 照常
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
            assertTrue("旧模型内容必须原样保留",
                    java.util.Arrays.equals(oldBin, readFile(new File(m.profileDir("p1"), DET_BIN))));
            assertFalse("就地还原后不得残留备份", new File(dir, ".bak_p1").exists());
            assertFalse("就地还原后必须结清事务记录", new File(dir, ".txn_p1").exists());
            assertTrue("状态应仍为就绪（旧模型在位）", m.status().isReady());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 恢复过程中「新目录删不掉」⇒ **保持现场、保留记录、报 ERROR**（Codex 要求 5）。
     *
     * <p>绝不可以在删不掉的情况下继续还原备份——那会把旧模型覆盖进一个仍在使用的新目录。
     */
    @Test
    public void deleteFailureDuringTxnRecoveryKeepsSceneAndReportsError() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            File target = m.profileDir("p1");
            File bak = new File(dir, ".bak_p1");
            assertTrue(target.renameTo(bak));
            Map<String, byte[]> v2 = defaultFiles("v2");
            writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
            writeTxnFile(dir, "p1", true, "p1", false);

            ops.failDelete = true;   // 新目录删不掉
            FaceRuntimeStatus st = m.status();

            assertFalse("恢复失败不得报就绪", st.isReady());
            assertTrue("应如实说明事务未完成", st.message.contains("事务未完成"));
            assertTrue("必须保留新目录现场", target.isDirectory());
            assertTrue("必须保留旧备份", bak.isDirectory());
            assertTrue("必须保留事务记录以待重试", new File(dir, ".txn_p1").exists());
            assertFalse("删除失败时不得误报已回滚（内容仍是新包）",
                    java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN)))
                            && !target.isDirectory());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /** 恢复必须**幂等**：反复查询状态不得产生不同结果或进一步改动磁盘（Codex 验收要求）。 */
    @Test
    public void txnRecoveryIsIdempotentAcrossRepeatedStatusCalls() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);

        for (int i = 0; i < 3; i++) {
            FaceRuntimeStatus st = m.status();
            assertTrue("第 " + i + " 次恢复应就绪", st.isReady());
            assertEquals("p1", st.profileId);
            assertTrue("第 " + i + " 次后内容应为旧模型",
                    java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
            assertFalse("第 " + i + " 次后不应残留备份", bak.exists());
            assertFalse("第 " + i + " 次后不应残留记录", new File(dir, ".txn_p1").exists());
        }
        assertNoTempArtifacts(dir);
    }

    /** A4：遗留在存储目录里的解压临时目录应在下次导入时被清扫（此前无任何清理路径）。 */
    @Test
    public void staleExtractionTempDirsAreSweptOnNextImport() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);

        // 模拟「解压阶段被杀」留下的残留
        File stale = new File(dir, ".tmp_12345");
        assertTrue(stale.mkdirs());
        assertTrue(new File(stale, "leftover.bin").createNewFile());
        assertTrue("前置：残留目录应存在", stale.isDirectory());

        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        assertFalse("下次导入应清扫掉残留的解压临时目录", stale.exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * A2-Med1 回归：**首次导入**在「激活已成功、仅指针写失败」时，必须**保留用户唯一的新包**。
     *
     * <p>此前 `rollbackProfileSwap` 的 `!hadOld` 分支会 `deleteRecursivelyQuietly(swap.target)`，
     * 把用户唯一的新包删掉，且文案谎称「已回滚配置」（根本没有可回滚的配置）。与激活失败分支
     *（那里刻意保留新目录）语义也不一致。
     */
    @Test
    public void firstImportPointerWriteFailureKeepsTheOnlyNewPackage() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);

        // 制造 persistActive 失败现场：dst 与 dst.bak 都占为非空目录 → dst→bak 的 rename 必然失败。
        File dst = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE);
        assertTrue(dst.mkdirs());
        assertTrue(new File(dst, "keep").createNewFile());
        File stubbornBak = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE + ".bak");
        assertTrue(stubbornBak.mkdirs());
        assertTrue(new File(stubbornBak, "keep").createNewFile());

        Map<String, byte[]> v1 = defaultFiles("v1");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true);

        assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
        assertTrue("首次导入失败必须保留用户唯一的新包", m.profileDir("p1").isDirectory());
        assertTrue("内容应是刚导入的新包",
                java.util.Arrays.equals(v1.get(DET_BIN),
                        readFile(new File(m.profileDir("p1"), DET_BIN))));
        assertFalse("事务应已了结", new File(dir, ".txn_p1").exists());
        assertTrue("文案应说明是保留而非回滚", r.message.contains("保留"));
    }

    /** A1-P2① / A2-Low7(a)：未了结的歧义现场必须**拒绝后续导入**（要求 5 的强制点）。 */
    @Test
    public void ambiguousCoexistenceBlocksFurtherImport() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        // 无事务记录、无回滚标记，却新旧目录并存 → 歧义
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);

        Map<String, byte[]> v3 = defaultFiles("v3");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v3, "lic"), v3)), ABI, true);

        assertEquals("歧义现场必须拒绝继续覆盖导入", FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
        assertTrue("必须保留两份数据（target）", target.isDirectory());
        assertTrue("必须保留两份数据（bak）", bak.isDirectory());
        assertTrue("应如实说明无法判定", r.message.contains("无法判定"));
    }

    /** A2-Low7(d)：已提交事务 + 备份删不掉 ⇒ 仍报 READY，且**不阻塞**后续导入（记录交给重试清理）。 */
    @Test
    public void committedTxnWithUndeletableBackupStillReportsReady() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            File target = m.profileDir("p1");
            File bak = new File(dir, ".bak_p1");
            assertTrue(target.renameTo(bak));
            Map<String, byte[]> v2 = defaultFiles("v2");
            writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
            writeTxnFile(dir, "p1", true, "p1", true);   // committed

            ops.failDelete = true;   // 备份清不掉
            FaceRuntimeStatus st = m.status();
            assertTrue("已提交的新模型应仍报就绪（不得被误回滚）", st.isReady());
            assertTrue("已提交 ⇒ 保留新目录内容",
                    java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
            assertTrue("清理未完成 ⇒ 保留记录以重试", new File(dir, ".txn_p1").exists());
            assertTrue("备份仍在（清理待重试）", bak.isDirectory());

            // 不阻塞后续导入（与「无记录歧义」不同：committed 是有据可依的）
            ops.failDelete = false;
            Map<String, byte[]> v3 = defaultFiles("v3");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v3, "lic"), v3)), ABI, true);
            assertTrue("已提交记录不应阻塞后续导入", r.isOk());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    // ---------- 夹具 ----------

    /** 按 flat 布局把 manifest 与模型文件写入目录（供构造磁盘现场使用）。 */
    private static void writeProfileDir(File dir, String manifestJson, Map<String, byte[]> files)
            throws IOException {
        assertTrue("目录应可创建", dir.isDirectory() || dir.mkdirs());
        java.nio.file.Files.write(new File(dir, FaceModelManager.MANIFEST_FILE_NAME).toPath(),
                manifestJson.getBytes(StandardCharsets.UTF_8));
        for (Map.Entry<String, byte[]> e : files.entrySet()) {
            java.nio.file.Files.write(new File(dir, e.getKey()).toPath(), e.getValue());
        }
    }

    private FaceModelManager manager(FakeRuntime rt) throws IOException {
        return new FaceModelManager(tmp.newFolder(), rt);
    }

    private static Map<String, byte[]> defaultFiles(String tag) {
        Map<String, byte[]> m = new LinkedHashMap<>();
        m.put(DET_PARAM, ("det-param-" + tag).getBytes(StandardCharsets.UTF_8));
        m.put(DET_BIN, ("det-bin-" + tag).getBytes(StandardCharsets.UTF_8));
        m.put(EMB_PARAM, ("emb-param-" + tag).getBytes(StandardCharsets.UTF_8));
        m.put(EMB_BIN, ("emb-bin-" + tag).getBytes(StandardCharsets.UTF_8));
        return m;
    }

    private static String manifest(String profileId, String abisJson, Map<String, byte[]> files,
                                   String license) throws Exception {
        return "{"
                + "\"schemaVersion\":1,"
                + "\"profileId\":\"" + profileId + "\","
                + "\"displayName\":\"" + profileId + " Display\","
                + "\"supportedAbis\":" + abisJson + ","
                + "\"detector\":{"
                + "\"param\":{\"path\":\"" + DET_PARAM + "\",\"sha256\":\"" + sha256(files.get(DET_PARAM)) + "\"},"
                + "\"bin\":{\"path\":\"" + DET_BIN + "\",\"sha256\":\"" + sha256(files.get(DET_BIN)) + "\"},"
                + "\"outputs\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"],"
                + "\"inputWidth\":640,\"inputHeight\":640},"
                + "\"embedder\":{"
                + "\"param\":{\"path\":\"" + EMB_PARAM + "\",\"sha256\":\"" + sha256(files.get(EMB_PARAM)) + "\"},"
                + "\"bin\":{\"path\":\"" + EMB_BIN + "\",\"sha256\":\"" + sha256(files.get(EMB_BIN)) + "\"},"
                + "\"inputWidth\":112,\"inputHeight\":112},"
                + "\"detectionConfidence\":0.5,\"nmsIou\":0.4,\"identitySimilarity\":0.4,"
                + "\"modelVersion\":\"1\",\"licenseId\":\"" + license + "\"}";
    }

    private static Map<String, byte[]> entries(String prefix, String manifestJson,
                                               Map<String, byte[]> files) {
        Map<String, byte[]> out = new LinkedHashMap<>();
        out.put(prefix + FaceModelManager.MANIFEST_FILE_NAME, manifestJson.getBytes(StandardCharsets.UTF_8));
        for (Map.Entry<String, byte[]> e : files.entrySet()) {
            out.put(prefix + e.getKey(), e.getValue());
        }
        return out;
    }

    private static byte[] zip(String prefix, String manifestJson, Map<String, byte[]> files)
            throws IOException {
        return zip(entries(prefix, manifestJson, files));
    }

    private static byte[] zip(Map<String, byte[]> entries) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        try (ZipOutputStream zos = new ZipOutputStream(bos)) {
            for (Map.Entry<String, byte[]> e : entries.entrySet()) {
                zos.putNextEntry(new ZipEntry(e.getKey()));
                zos.write(e.getValue());
                zos.closeEntry();
            }
        }
        return bos.toByteArray();
    }

    private static InputStream stream(byte[] data) {
        return new ByteArrayInputStream(data);
    }

    private static String sha256(byte[] data) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        byte[] d = md.digest(data);
        char[] hex = "0123456789abcdef".toCharArray();
        char[] out = new char[d.length * 2];
        for (int i = 0; i < d.length; i++) {
            int v = d[i] & 0xFF;
            out[i * 2] = hex[v >>> 4];
            out[i * 2 + 1] = hex[v & 0x0F];
        }
        return new String(out);
    }

    private static byte[] readFile(File f) throws IOException {
        try (InputStream in = new java.io.FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static void writeText(File f, String s) throws IOException {
        try (java.io.FileOutputStream fos = new java.io.FileOutputStream(f, false)) {
            fos.write(s.getBytes(StandardCharsets.UTF_8));
        }
    }

    private static void deleteRecursively(File f) {
        if (f.isDirectory()) {
            File[] ch = f.listFiles();
            if (ch != null) {
                for (File c : ch) {
                    deleteRecursively(c);
                }
            }
        }
        assertTrue("清理失败: " + f, f.delete());
    }

    /** 断言无临时/备份/指针残留，防半成品配置泄漏。 */
    private static void assertNoTempArtifacts(File storeDir) {
        File[] ch = storeDir.listFiles();
        if (ch == null) {
            return;
        }
        for (File f : ch) {
            String n = f.getName();
            assertFalse("残留临时/备份内容: " + n,
                    n.startsWith(".tmp_") || n.startsWith(".bak_") || n.startsWith(".rollback_")
                            || n.startsWith(".txn_")
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".tmp")
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".bak"));
        }
    }

    /** 可编程 native 假实现：ok=false 时 loadProfile 返回失败 JSON，validated=false 时只校验不通过。 */
    private static final class FakeRuntime implements FaceModelManager.NativeRuntime {
        boolean ok = true;
        boolean validated = true;
        /** activateRuntime 成败开关（#2：失败时调用方须保留/回滚指针）。 */
        boolean activateOk = true;
        boolean activateEffective = true;
        String activateReason = "ok";
        int activateCalls = 0;
        String lastBaseDir;
        String lastManifest;
        String runtimeJson = "{\"ok\":true,\"profileValidated\":true,\"profileLoaded\":true,"
                + "\"effective\":true,\"reason\":\"ok\",\"embeddingDim\":512,"
                + "\"galleryCount\":0,\"enrollmentState\":\"IDLE\"}";

        @Override
        public String loadProfile(String manifestJson, String currentAbi) {
            lastManifest = manifestJson;
            if (ok) {
                return "{\"ok\":true,\"validated\":" + validated + ",\"staged\":true,"
                        + "\"effective\":false,\"profileId\":\"x\",\"modelVersion\":\"1\","
                        + "\"embeddingDim\":512}";
            }
            return "{\"ok\":false,\"error\":{\"code\":\"FACE_PROFILE_INVALID\",\"message\":\"bad\"}}";
        }

        @Override
        public String getRuntimeStatus() {
            return runtimeJson;
        }

        @Override
        public String activateRuntime(String manifestJson, String baseDir, String currentAbi,
                                      String galleryDir) {
            activateCalls++;
            lastBaseDir = baseDir;
            if (activateOk) {
                return "{\"ok\":true,\"pending\":false,\"effective\":" + activateEffective
                        + ",\"reason\":\"" + activateReason + "\",\"generation\":1}";
            }
            return "{\"ok\":false,\"pending\":false,\"effective\":false,\"reason\":\""
                    + activateReason + "\",\"generation\":0,\"error\":{"
                    + "\"code\":\"FACE_RUNTIME_ACTIVATE_FAILED\",\"message\":\"reload failed\"}}";
        }
    }

    /** 记录 activateRuntime 调用的 native 假实现（loadProfile 恒 validated）。 */
    private static final class RecordingRuntime implements FaceModelManager.NativeRuntime {
        int activateCalls = 0;
        String lastBaseDir;

        @Override
        public String loadProfile(String manifestJson, String currentAbi) {
            return "{\"ok\":true,\"validated\":true,\"staged\":true,\"effective\":false,"
                    + "\"profileId\":\"x\",\"modelVersion\":\"1\",\"embeddingDim\":512}";
        }

        @Override
        public String getRuntimeStatus() {
            return "{\"ok\":true,\"profileValidated\":true,\"effective\":true,\"embeddingDim\":512}";
        }

        @Override
        public String activateRuntime(String manifestJson, String baseDir, String currentAbi,
                                      String galleryDir) {
            activateCalls++;
            lastBaseDir = baseDir;
            return "{\"ok\":true,\"pending\":false,\"effective\":true,\"reason\":\"ok\"}";
        }
    }

    // ---------- native 激活：导入成功后必须绑定实时管线 ----------

    @Test
    public void importActivatesRuntimeWithStableProfileDir() throws Exception {
        RecordingRuntime rt = new RecordingRuntime();
        FaceModelManager m = new FaceModelManager(tmp.newFolder(), rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
        assertTrue(r.message, r.isOk());
        assertEquals("导入成功必须激活实时管线一次", 1, rt.activateCalls);
        assertEquals("必须以稳定的最终 profile 目录绑定，而非将被清理的临时目录",
                m.profileDir("p1").getAbsolutePath(), rt.lastBaseDir);
        assertFalse("绑定目录不得为 .tmp_ 临时目录", rt.lastBaseDir.contains(".tmp_"));
    }

    @Test
    public void activateRebindsRuntimeToProfileDir() throws Exception {
        RecordingRuntime rt = new RecordingRuntime();
        FaceModelManager m = new FaceModelManager(tmp.newFolder(), rt);
        Map<String, byte[]> files = defaultFiles("v1");
        String manifest = manifest("p1", "[\"arm64-v8a\"]", files, "lic");
        assertTrue(m.importZip(stream(zip("", manifest, files)), ABI, true).isOk());
        FaceModelManager.Result r = m.activate("p1", ABI, true);
        assertTrue(r.message, r.isOk());
        assertEquals("切换成功后必须重绑实时管线一次", 2, rt.activateCalls);
        assertEquals(m.profileDir("p1").getAbsolutePath(), rt.lastBaseDir);
    }

    @Test
    public void importRejectsReservedProfileId() throws Exception {
        for (String id : new String[]{"active_profile", ".tmp_x", ".bak_y"}) {
            FakeRuntime rt = new FakeRuntime();
            FaceModelManager m = manager(rt);
            Map<String, byte[]> files = defaultFiles("v1");
            String manifest = manifest(id, "[\"arm64-v8a\"]", files, "lic");
            FaceModelManager.Result r = m.importZip(stream(zip("", manifest, files)), ABI, true);
            assertEquals("应拒绝保留 profileId: " + id,
                    FaceModelManager.Status.MANIFEST_INVALID, r.status);
        }
    }

    @Test
    public void activateRejectsReservedProfileId() throws Exception {
        FaceModelManager m = manager(new FakeRuntime());
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT,
                m.activate("active_profile", ABI, true).status);
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT,
                m.activate(".tmp_x", ABI, true).status);
    }

    @Test
    public void successReturnsRuntimeStatusReady() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        FaceModelManager m = manager(rt);
        Map<String, byte[]> files = defaultFiles("v1");
        FaceModelManager.Result r =
                m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", files, "lic"), files)), ABI, true);
        assertTrue(r.isOk());
        assertTrue(r.runtimeStatus.isReady());
        assertNotNull(r.runtimeStatus.profileId);
    }
}
