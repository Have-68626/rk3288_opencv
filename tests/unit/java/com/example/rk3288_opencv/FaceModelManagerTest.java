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
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
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

    /**
     * 事务代次测试常量。必须是 32 位小写 hex —— {@code isSafeTxnId} 会拒绝其它形态，
     * 而非法的代次会被降级为「无提交证据」，用错会静默改变用例语义。
     */
    private static final String TXN_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private static final String TXN_B = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    /**
     * 暂存目录名（与生产实现 {@code FaceModelManager.STAGING_DIR} 同名）。
     *
     * <p>此处用**字面量**而非引用生产常量：回归用例必须能在**旧实现**（尚无 {@code STAGING_DIR}）
     * 上编译并跑出断言失败（而非编译失败），否则无法呈现「先红后绿」。
     */
    private static final String STAGING_DIR = ".staging";

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
        // ── 既有全局开关（语义逐字不变，既有用例零改动）────────────────────────────
        boolean failWrite;
        boolean failDelete;
        /** 只让「从 .tmp_ 解压目录改名到目标」这一步失败（迁就其它改名，如回滚的 bak→target）。 */
        boolean failRenameFromTmp;

        // ── 新增：可编程闸门（2026-09-17 外部审查修复）─────────────────────────────
        // 为什么需要：全局布尔**无法表达「第 1 次 writeSync 成功、第 2 次失败」**，而
        // 「提交改写失败 + 备份删除也失败」这个组合（P1 #1 的主证据）恰恰需要它。
        // 用「按内容匹配」而非「按调用序号」：与调用顺序解耦，重构不会让它悄悄失效。
        // （刻意**不**提供按序号的闸门：既无用例需要，又要求「失败也计数」才不漂移，是个隐患。）
        private final Map<String, Set<String>> failNameByOp = new HashMap<>();
        private final Set<String> failWriteIfContains = new HashSet<>();
        /** 只匹配**源文件名**的 rename 闸门（见 {@link #failRenameFromName}）。 */
        private final Set<String> failRenameFrom = new HashSet<>();
        /**
         * 只匹配 writeSync 目标**路径**（以 {@code '/'} 归一化）子串的闸门。
         *
         * <p>为什么需要它：临时文件迁入 {@code .staging/txn/<id>} 与 {@code .staging/commit/<id>} 后，
         * 二者的**文件名都是 profileId**，{@link #failWriteForName} 无法区分「事务记录的暂存文件」与
         * 「提交见证的暂存文件」，故改按所在子目录（{@code .staging/commit/...}）区分。
         */
        private final Set<String> failWritePathContains = new HashSet<>();
        /**
         * 只匹配 writeSync 目标**路径**子串、且要求「**先落半截内容再失败**」的闸门。
         *
         * <p>为什么需要它：{@code DEFAULT_FILE_OPS.writeSync} 是「先 {@code new FileOutputStream(f,false)}
         * 建/截断文件，再 {@code write}」，故真实的写失败（如磁盘写满）会**留下半截文件**；而普通闸门
         * （{@link #gated} 命中即 {@code return false}）**根本不建文件**，表达不了这一现场。临时文件
         * 迁入 {@code .staging/} 后，「半截文件由下次导入的清扫回收」是设计的承重前提，必须能构造它。
         */
        private final Set<String> failWritePartialPathContains = new HashSet<>();

        /**
         * 只让写往该文件名（如 "active_profile.tmp"）的 writeSync 失败。
         *
         * <p>⚠️ 不能用来区分 {@code .staging/txn/<id>} 与 {@code .staging/commit/<id>}（同名）；
         * 那种场合请用 {@link #failWriteForPathContains}。
         */
        void failWriteForName(String name) {
            failNameByOp.computeIfAbsent("write", k -> new HashSet<>()).add(name);
        }

        /**
         * 只让写往**路径**（{@code '/'} 归一化）包含该子串（如 {@code ".staging/commit/p2"}）的操作失败。
         *
         * <p>⚠️ 闸门看的是 {@link #gated} 的 {@code to} 参数，故对**任何**传入非空 {@code to} 的操作都生效
         * ——不止 {@code writeSync}，{@code moveReplace} 也一样。当前唯一用法 {@code ".staging/commit/p2"}
         * 只命中暂存路径（{@code moveReplace} 的目标是顶层正式名 {@code .commit_p2}，不含该子串），故不误伤。
         * 将来若改用更短的子串（如 {@code "commit"}），务必先确认它不会命中别处的 {@code to}。
         */
        void failWriteForPathContains(String pathSubstring) {
            failWritePathContains.add(pathSubstring.replace('\\', '/'));
        }

        /** 让写往路径含该子串的 writeSync **先落半截内容再失败**（还原真实写失败的磁盘现场）。 */
        void failWritePartialForPathContains(String pathSubstring) {
            failWritePartialPathContains.add(pathSubstring.replace('\\', '/'));
        }

        /** 只让内容包含该子串的 writeSync 失败（如 "state=committed"）。 */
        void failWriteIfContains(String needle) {
            failWriteIfContains.add(needle);
        }

        /** 只让删除该名字（如 ".bak_p1"）的 deleteRecursively 失败。 */
        void failDeleteForName(String name) {
            failNameByOp.computeIfAbsent("delete", k -> new HashSet<>()).add(name);
        }

        /**
         * 只让**改名目标**为该名字的 {@code moveReplace} 失败（如记录 {@code .txn_p1}、见证
         * {@code .commit_p2}）。
         *
         * <p>闸门读的是 {@link #gated} 的 {@code to.getName()}/{@code from.getName()}——{@code moveReplace}
         * 的源是暂存文件（名即 profileId）、目标是正式记录名，故用**正式名**注册即可唯一命中。
         */
        void failMoveReplaceForName(String name) {
            failNameByOp.computeIfAbsent("moveReplace", k -> new HashSet<>()).add(name);
        }

        /**
         * 只让**源文件名**为该名字的 rename 失败（如回滚的 {@code .bak_p2 → p2}）。
         *
         * <p>为什么必须只匹配源名：{@code .bak_<id>} 这个名字在两侧都出现——替换是
         * {@code <id> → .bak_<id>}（源名 {@code <id>}）、回滚是 {@code .bak_<id> → <id>}（源名
         * {@code .bak_<id>}）。按「源或目标」匹配会连替换那一步一起打断，导入在**替换之前**就中止，
         * 根本走不到要考的回滚分支。这里沿用既有 {@code failRenameFromTmp} 的「只看源名」口径。
         */
        void failRenameFromName(String name) {
            failRenameFrom.add(name);
        }

        /** 清空闸门（用例中途解除故障、再断言收敛时用）。 */
        void resetGates() {
            failNameByOp.clear();
            failWriteIfContains.clear();
            failRenameFrom.clear();
            failWritePathContains.clear();
            failWritePartialPathContains.clear();
        }

        /** 本次调用是否命中闸门。 */
        private boolean gated(String op, File from, File to, byte[] data) {
            Set<String> names = failNameByOp.get(op);
            if (names != null) {
                if (from != null && names.contains(from.getName())) return true;
                if (to != null && names.contains(to.getName())) return true;
            }
            if (matchesPath(failWritePathContains, to)) return true;
            if (data != null && !failWriteIfContains.isEmpty()) {
                String text = new String(data, StandardCharsets.UTF_8);
                for (String needle : failWriteIfContains) {
                    if (text.contains(needle)) return true;
                }
            }
            return false;
        }

        @Override
        public boolean rename(File from, File to) {
            if (failRenameFromTmp && from.getName().startsWith(".tmp_")) return false;
            if (failRenameFrom.contains(from.getName())) return false;
            return from.renameTo(to);
        }

        @Override
        public boolean moveReplace(File from, File to) {
            // **闸门命中时不得执行 to.delete()**：默认实现是先删目标再改名，但被闸门拦下时必须
            // 完全不动作——否则接缝自身会制造出生产路径不存在的破坏。
            if (gated("moveReplace", from, to, null)) return false;
            //noinspection ResultOfMethodCallIgnored
            to.delete();
            return from.renameTo(to);
        }

        @Override
        public boolean deleteRecursively(File f) {
            if (failDelete) return false;
            if (gated("delete", f, null, null)) return false;
            return deleteRecursivelyReal(f);
        }

        /** 目标路径（{@code '/'} 归一化）是否含集合中任一子串。 */
        private static boolean matchesPath(Set<String> needles, File f) {
            if (f == null || needles.isEmpty()) return false;
            String p = f.getPath().replace('\\', '/');
            for (String needle : needles) {
                if (p.contains(needle)) return true;
            }
            return false;
        }

        @Override
        public boolean writeSync(File f, byte[] data) {
            if (failWrite) return false;
            if (matchesPath(failWritePartialPathContains, f)) {
                // 先落半截再失败：还原 DEFAULT_FILE_OPS.writeSync 的真实语义（先建/截断文件再 write），
                // 普通闸门命中时**根本不建文件**，表达不了「留下半截临时文件」这一现场。
                try (java.io.FileOutputStream fos = new java.io.FileOutputStream(f, false)) {
                    fos.write(data, 0, Math.max(1, data.length / 2));
                    fos.flush();
                } catch (Exception ignored) {
                }
                return false;
            }
            if (gated("write", null, f, data)) return false;
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

    /**
     * 构造**旧格式**事务记录（**无 txnId 行**）：等同于本版本之前的代码写入的现场，
     * 恢复时走 {@code recoverTxnLegacy}（无提交证据可比对）。
     */
    private static void writeTxnFile(File dir, String profileId, boolean hadOld,
                                     String prevActive, boolean committed) throws IOException {
        writeTxnFile(dir, profileId, hadOld, prevActive, committed, null);
    }

    /** 构造事务记录文件（与 DirTxn.encode() 同格式）；{@code txnId == null} 时不写该行。 */
    private static void writeTxnFile(File dir, String profileId, boolean hadOld,
                                     String prevActive, boolean committed, String txnId)
            throws IOException {
        StringBuilder sb = new StringBuilder();
        sb.append("profileId=").append(profileId).append('\n');
        sb.append("hadOld=").append(hadOld).append('\n');
        sb.append("prevActive=").append(prevActive == null ? "" : prevActive).append('\n');
        if (txnId != null) {
            sb.append("txnId=").append(txnId).append('\n');
        }
        sb.append("state=").append(committed ? "committed" : "in_progress").append('\n');
        java.nio.file.Files.write(new File(dir, ".txn_" + profileId).toPath(),
                sb.toString().getBytes(StandardCharsets.UTF_8));
    }

    /** 旧格式激活指针：裸 profileId（无提交证据）。 */
    private static void writeActivePointer(File dir, String profileId) throws IOException {
        java.nio.file.Files.write(new File(dir, FaceModelManager.ACTIVE_POINTER_FILE).toPath(),
                profileId.getBytes(StandardCharsets.UTF_8));
    }

    /** 新格式激活指针：{@code profileId=} + 可选 {@code txnId=}。 */
    private static void writeActivePointer(File dir, String profileId, String txnId)
            throws IOException {
        String s = "profileId=" + profileId + "\n" + (txnId == null ? "" : "txnId=" + txnId + "\n");
        java.nio.file.Files.write(new File(dir, FaceModelManager.ACTIVE_POINTER_FILE).toPath(),
                s.getBytes(StandardCharsets.UTF_8));
    }

    /** 读回指针里的提交代次（断言用）；无该行或指针缺失返回 null。 */
    private static String readPointerTxnId(File dir) throws IOException {
        File f = new File(dir, FaceModelManager.ACTIVE_POINTER_FILE);
        if (!f.isFile()) {
            return null;
        }
        for (String line : new String(readFile(f), StandardCharsets.UTF_8).split("\n")) {
            if (line.startsWith("txnId=")) {
                return line.substring("txnId=".length()).trim();
            }
        }
        return null;
    }

    /** 提交见证文件路径（与生产实现同名；实现若换名，夹具类用例会因「找不到见证」而判未提交 → 红）。 */
    private static File commitWitnessOf(File dir, String profileId) {
        return new File(dir, FaceModelManager.COMMIT_WITNESS_PREFIX + profileId);
    }

    /** 构造提交见证文件；{@code txnId == null} 时不写该行（等价于「无证据」）。 */
    private static void writeCommitWitness(File dir, String profileId, String txnId)
            throws IOException {
        String s = "profileId=" + profileId + "\n" + (txnId == null ? "" : "txnId=" + txnId + "\n");
        java.nio.file.Files.write(commitWitnessOf(dir, profileId).toPath(),
                s.getBytes(StandardCharsets.UTF_8));
    }

    /** 读回见证里的提交代次（断言用）；无该行或见证缺失返回 null。 */
    private static String readCommitWitnessTxnId(File dir, String profileId) throws IOException {
        File f = commitWitnessOf(dir, profileId);
        if (!f.isFile()) {
            return null;
        }
        for (String line : new String(readFile(f), StandardCharsets.UTF_8).split("\n")) {
            if (line.startsWith("txnId=")) {
                return line.substring("txnId=".length()).trim();
            }
        }
        return null;
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
        // 另有一个配置处于激活位，使「替换前的激活指针」与本 profileId **不同**：同名覆盖时
        // 「未提交」与「已提交但记录改写失败」两历史同形（见 ambiguousSameNameLegacySite...
        // 用例），而本用例要验的是**可判定**的未提交回滚，故必须有另一个激活配置。
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        assertTrue(m.activate("pA", ABI, true).isOk());

        // 构造「替换已开始但未提交」现场：旧目录移开、新包占位、事务记录为 in_progress。
        // 记录带代次 TXN_A，而指针上是导入/切换时留下的**另一个**代次 ⇒ 提交点未落定 ⇒ 回滚目录。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "pA", false, TXN_A);

        // 未提交事务只会在**触碰该 profile 时**被恢复（buildStatus 只恢复当前激活的 profile），
        // 故用 activate("p1") 触发 —— 这也正是真实可达路径：用户在该现场下选择切到 p1。
        FaceModelManager.Result r = m.activate("p1", ABI, true);
        assertTrue("恢复后 p1 应可正常激活：" + r.message, r.isOk());
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
            writeTxnFile(dir, "p1", true, "p1", false, TXN_B);   // 代次与指针不符 ⇒ 未提交
            ops.failDelete = true;   // 新目录删不掉
            FaceRuntimeStatus st = m.status();

            assertFalse("恢复失败不得报就绪", st.isReady());
            assertTrue("应如实说明事务未完成", st.message.contains("事务未完成"));
            assertTrue("必须保留新目录现场", target.isDirectory());
            assertTrue("必须保留旧备份", bak.isDirectory());
            assertTrue("必须保留事务记录以待重试", new File(dir, ".txn_p1").exists());
            assertTrue("删除失败时内容必须仍是新包（不得声称已回滚）",
                    java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
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
        writeTxnFile(dir, "p1", true, "p1", false, TXN_B);   // 代次与指针不符 ⇒ 未提交

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

    /**
     * **γ3 判别性用例**（A3 确认此前缺失）：成功路径下**备份清理失败**时，必须**保留 committed 记录**。
     *
     * <p>若无条件 `clearTxn`，磁盘会留下「target + bak + 无记录」——被 `buildStatus` 当**歧义**报 ERROR，
     * 且 `unresolvedSwapReason` 会**永久阻塞**该 profileId 的后续导入。留下 committed 记录则让恢复
     * 继续重试清理：既不报错、也不阻塞。
     */
    @Test
    public void failedBackupCleanupKeepsCommittedTxnInsteadOfLeavingAmbiguity() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            ops.failDelete = true;   // 备份清不掉
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertTrue("导入本身应成功（激活确已完成）", r.isOk());
            File txn = new File(dir, ".txn_p1");
            assertTrue("备份未清掉时**必须保留**事务记录，否则退化成「无记录的歧义现场」", txn.exists());
            assertTrue("记录应仍为 committed", new String(readFile(txn), StandardCharsets.UTF_8)
                    .contains("state=committed"));
            assertTrue("备份仍在（清理待重试）", new File(dir, ".bak_p1").exists());

            // 且不阻塞后续：恢复会重试清理，清掉后记录随之消失、状态仍就绪
            ops.failDelete = false;
            FaceRuntimeStatus st = m.status();
            assertTrue("已提交状态应报就绪（不得被误判为歧义）", st.isReady());
            assertFalse("清理完成后记录应被清除", txn.exists());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * **γ2 判别性用例**（A3 确认此前缺失）：**陈旧备份无法清理时，必须在写事务记录之前中止**。
     *
     * <p>否则会写入 `in_progress` 记录，随后 `target→bak` 因目标已存在而失败，留下
     * 「`in_progress` + 陈旧 bak + 好 target」——下次恢复会把**已提交的新模型**误判为「未提交替换」
     * 而回滚到陈旧备份（静默回退一次有效更新）。
     *
     * <p><b>现场构造</b>：`committed` 记录 + 删不掉的 `.bak_p1`（这正是 A1-P2②/A2-Med2 的触发链）。
     * 之所以需要 committed 记录，是因为「无记录 + 新旧并存」会更早被 `unresolvedSwapReason` 判为歧义而拦下。
     */
    @Test
    public void staleBackupBlocksSwapBeforeTxnIsWritten() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 造出「陈旧 .bak + committed 记录」现场
            File target = m.profileDir("p1");
            File staleBak = new File(dir, ".bak_p1");
            assertTrue(target.renameTo(staleBak));
            Map<String, byte[]> v2 = defaultFiles("v2");
            writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
            writeTxnFile(dir, "p1", true, "p1", true);   // committed

            ops.failDelete = true;   // 陈旧备份清不掉
            Map<String, byte[]> v3 = defaultFiles("v3");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v3, "lic"), v3)), ABI, true);

            assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
            assertTrue("应如实说明备份无法清理，实际=" + r.message, r.message.contains("备份无法清理"));
            File txn = new File(dir, ".txn_p1");
            assertTrue("记录必须仍存在", txn.exists());
            assertTrue("**不得**被降级为 in_progress（否则恢复会误回滚到陈旧备份）",
                    new String(readFile(txn), StandardCharsets.UTF_8).contains("state=committed"));
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    // ══ 2026-09-17 外部审查修复：提交点证据（txnId）/ 恢复顺序 / 人工了结 ══════════════
    // 背景：仅有 state 是不够的——**写 state 本身可能失败**，于是「未提交」与「已提交但
    // committed 改写失败」会留下同一份 in_progress 记录；同名覆盖时连激活指针都前后相同，
    // 旧实现无法区分，会把一次**成功**的导入静默回滚。现在提交证据 = 指针里的 txnId。

    /**
     * 同名覆盖 + **记录代次与指针代次不符** ⇒ 提交点未落定 ⇒ 回滚目录，且**指针不得被动过**
     * （约束 1：提交点之前失败则回滚；未提交的事务从未写过指针，故回滚不还原指针）。
     *
     * <p>「不动指针」在这里是**可观测**的：断言恢复后指针里的代次与恢复前**逐字相同**。
     * 同时覆盖「重复启动」——再调一次 {@code recoverForStartup()} 结果一致。
     */
    @Test
    public void sameNameOverwriteWithStaleTxnIdRollsBackAndLeavesPointerUntouched() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());
        String pointerTxn = readPointerTxnId(dir);
        assertNotNull("切换成功后指针应带提交代次", pointerTxn);
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false, TXN_A);   // 代次 ≠ 指针代次 ⇒ 未提交

        FaceModelManager.StartupRecovery sr = m.recoverForStartup();
        assertNull("恢复应可完成，不应阻塞：" + sr.blockedReason, sr.blockedReason);
        assertEquals("p1", sr.activeProfileId);
        assertTrue("必须还原旧模型内容（不得保留未激活的新包）",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertFalse(".bak_p1 应已被消费", bak.exists());
        assertFalse("事务了结后不得残留记录", new File(dir, ".txn_p1").exists());
        assertEquals("回滚不得改写指针（代次应原样保留）", pointerTxn, readPointerTxnId(dir));

        // 重复启动：幂等
        FaceModelManager.StartupRecovery again = m.recoverForStartup();
        assertNull(again.blockedReason);
        assertEquals("p1", again.activeProfileId);
        assertNoTempArtifacts(dir);
    }

    /**
     * 同名覆盖 + **记录代次与指针相等** ⇒ 提交点已落定（即便记录因改写失败停在 in_progress）
     * ⇒ **保留新目录**、清理备份、清记录。这是外部审查 P1 #1 的核心消解。
     */
    @Test
    public void sameNameOverwriteWithMatchingTxnIdKeepsNewModel() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());
        String pointerTxn = readPointerTxnId(dir);
        assertNotNull(pointerTxn);

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        // 记录停在 in_progress，但代次与指针一致 = 提交点已落定
        writeTxnFile(dir, "p1", true, "p1", false, pointerTxn);

        FaceModelManager.StartupRecovery sr = m.recoverForStartup();
        assertNull(sr.blockedReason);
        assertTrue("必须保留新模型内容（不得回滚一次已提交的更新）",
                java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse(".bak_p1 应已被清理", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        assertEquals("已提交侧不得改写指针", pointerTxn, readPointerTxnId(dir));
        assertNoTempArtifacts(dir);
    }

    /**
     * **旧格式**记录 + 指针已是本 profileId + prevActive 是**另一个**配置 ⇒ 提交点必已落定
     * （未提交的事务不会写指针）⇒ 保留新目录。旧格式兼容路径中「可判定」的加强项。
     */
    @Test
    public void legacyTxnWithPointerOnTargetKeepsNewModel() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("pA", ABI, true).isOk());

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        // 旧格式记录（无代次）：prevActive 是 pA，而指针已是 p1 ⇒ 提交点已落定
        writeTxnFile(dir, "p1", true, "pA", false);
        writeActivePointer(dir, "p1");

        // 触碰该 profile 触发恢复（未提交事务只在其所属 profile 被触碰时恢复）
        assertTrue(m.activate("p1", ABI, true).isOk());
        assertTrue("必须保留新模型内容", java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse("备份应被清理", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 旧格式 + 「回滚已消费备份、**仅指针还原失败**」⇒ 补做指针还原；还原失败必须**保留记录**
     * （P2 #3：旧实现据「备份不存在」直接清记录，会永久丢失这次还原意图）。
     *
     * <p>顺带覆盖约束 2-3：恢复改写指针后，状态必须按**重读到的**配置报——解除故障后应报
     * {@code pX}，而不是恢复前的 {@code pA}。
     */
    @Test
    public void legacyPointerRestoreFailureRetainsTxnThenSelfHeals() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> vx = defaultFiles("vx");
        assertTrue(m.importZip(stream(zip("", manifest("pX", "[\"arm64-v8a\"]", vx, "lic"), vx)), ABI, true).isOk());
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        assertTrue(m.activate("pA", ABI, true).isOk());

        // 现场：pA 的旧格式未提交记录，prevActive = pX，**备份已被消费**（不存在），指针仍是 pA
        writeTxnFile(dir, "pA", true, "pX", false);

        FaultyFileOps ops = new FaultyFileOps();
        ops.failWriteForName(FaceModelManager.ACTIVE_POINTER_FILE);   // 指针还原写不进去
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FaceRuntimeStatus st = m.status();
            assertFalse("指针还原失败不得报就绪", st.isReady());
            assertTrue("必须保留记录以待重试", new File(dir, ".txn_pA").exists());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }

        // 解除故障后自愈。**判别点（约束 2-3）**：这里必须由**状态查询本身**触发那次补做的指针
        // 还原——若 buildStatus 不重读指针，它会按恢复前的 pA 报状态，而磁盘已指向 pX。
        // （因此本用例中 status() 必须排在任何 recoverForStartup() 之前，否则修复会先被后者做掉、
        // 把判别点遮住；启动闸门的同名判别见 startupRecoveryReturnsPostRecoveryPointer。）
        FaceRuntimeStatus st2 = m.status();
        assertTrue("恢复完成后应就绪", st2.isReady());
        assertEquals("状态必须按**重读后**的配置报（约束 2-3）", "pX", st2.profileId);
        assertFalse("事务了结后不得残留记录", new File(dir, ".txn_pA").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 启动闸门必须返回**恢复之后**重读到的指针，而不是恢复前的值（约束 2-1）。
     *
     * <p>与 {@code legacyPointerRestoreFailureRetainsTxnThenSelfHeals} 互为镜像：那条让状态查询
     * 先触发修复以判别 buildStatus 的重读，本条让**闸门**先触发以判别 recoverForStartup 的重读。
     */
    @Test
    public void startupRecoveryReturnsPostRecoveryPointer() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> vx = defaultFiles("vx");
        assertTrue(m.importZip(stream(zip("", manifest("pX", "[\"arm64-v8a\"]", vx, "lic"), vx)), ABI, true).isOk());
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        assertTrue(m.activate("pA", ABI, true).isOk());
        assertEquals("pA", m.activeProfileId());

        // 旧格式未提交记录 + 备份已被消费 + 指针仍是 pA ⇒ 恢复会补做指针还原为 pX
        writeTxnFile(dir, "pA", true, "pX", false);

        FaceModelManager.StartupRecovery sr = m.recoverForStartup();
        assertNull(sr.blockedReason);
        assertEquals("闸门必须返回恢复**之后**的指针（pX），而非恢复前的 pA", "pX", sr.activeProfileId);
        assertEquals("指针本身也应已落到 pX", "pX", m.activeProfileId());
        assertFalse("事务了结后不得残留记录", new File(dir, ".txn_pA").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 同名覆盖 + **旧格式**记录（无代次）+ 新旧并存 ⇒ **证据不足**：保留两份数据与记录、报 ERROR，
     * 并带机器可读的 {@code ambiguousSwapProfileId} 供 UI 给出人工了结入口；
     * 且**不整机砖死**——另一个 profile 的导入不受影响。
     */
    @Test
    public void ambiguousSameNameLegacySiteIsPreservedAndBlocked() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());

        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);   // 旧格式 + 同名 ⇒ 两历史同形

        FaceRuntimeStatus st = m.status();
        assertFalse("证据不足不得自动收敛", st.isReady());
        assertEquals("必须带机器可读的歧义标记供 UI 给入口", "p1", st.ambiguousSwapProfileId);
        assertTrue("两份数据都必须保留", target.isDirectory() && bak.isDirectory());
        assertTrue("记录必须保留（不得丢弃还原意图）", new File(dir, ".txn_p1").exists());
        assertTrue("文案应指向人工了结，实际=" + st.message,
                st.message.contains("并存") || st.message.contains("歧义"));

        // 不整机砖死：另一个 profile 仍可导入
        Map<String, byte[]> v3 = defaultFiles("v3");
        assertTrue("p1 的歧义不得阻塞 p2",
                m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v3, "lic"), v3)), ABI, true).isOk());
        assertTrue(new File(m.profileDir("p2"), DET_BIN).isFile());
    }

    /** 人工了结：选「保留当前目录」⇒ 删备份、清记录、解除阻塞。 */
    @Test
    public void resolveAmbiguousSwapKeepingNewUnblocks() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);

        assertTrue(m.resolveAmbiguousSwap("p1", true).isOk());
        assertTrue("当前目录应保留", target.isDirectory());
        assertTrue("保留的是新模型内容",
                java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse("备份应被删除", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        FaceRuntimeStatus st = m.status();
        assertTrue("了结后应就绪", st.isReady());
        assertNull("不应再带歧义标记", st.ambiguousSwapProfileId);
        assertNoTempArtifacts(dir);
    }

    /** 人工了结：选「还原为备份」⇒ 内容换回旧版、备份被消费、清记录、解除阻塞。 */
    @Test
    public void resolveAmbiguousSwapRestoringBackupUnblocks() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);

        assertTrue(m.resolveAmbiguousSwap("p1", false).isOk());
        assertTrue("内容应还原为旧模型",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertFalse("备份已被消费", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        assertTrue("了结后应就绪", m.status().isReady());
        assertNoTempArtifacts(dir);
    }

    /** 无「新旧并存」现场时 {@code resolveAmbiguousSwap} 必须拒绝——防止被当成通用删除入口。 */
    @Test
    public void resolveAmbiguousSwapRejectsWhenNoScene() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        FaceModelManager.Result r = m.resolveAmbiguousSwap("p1", true);
        assertEquals("无现场应拒绝", FaceModelManager.Status.INVALID_ARGUMENT, r.status);
        assertTrue("拒绝时不得删除任何东西", new File(m.profileDir("p1"), DET_BIN).isFile());

        FaceModelManager.Result r2 = m.resolveAmbiguousSwap("p1", false);
        assertEquals(FaceModelManager.Status.INVALID_ARGUMENT, r2.status);
        assertTrue(new File(m.profileDir("p1"), DET_BIN).isFile());
    }

    /**
     * **目标目录缺失、备份是唯一副本**（A1 审计 N2 + N3）：已提交侧**不得**顺手删掉那份唯一副本；
     * 状态必须给出**可执行**的人工了结入口（只允许「还原为备份」），把配置救回来。
     *
     * <p>N2：`commitSide` 原实现无条件 `deleteRecursively(bak)`——「已提交」只说明提交点落定，
     * 不蕴含「新目录仍在」，target 被外部删除时删备份 = 配置彻底丢失。
     * N3：`resolveAmbiguousSwap` 原来要求 target 与 bak **都**存在，于是这种现场会「报错却点了没用」
     * ——死路。现改为 target 缺失时只允许「还原为备份」。
     */
    @Test
    public void missingTargetKeepsOnlyCopyAndOffersBackupRestore() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        byte[] oldBin = readFile(new File(m.profileDir("p1"), DET_BIN));

        // 现场：target 被外部删除、备份仍在、指针已指向本 profileId、prevActive 是另一配置。
        // 旧格式记录 ⇒ 走 recoverTxnLegacy 的「判为已提交」分支 ⇒ commitSide。
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        writeTxnFile(dir, "p1", true, "pA", false);
        assertFalse("target 应已缺失", target.exists());

        FaceRuntimeStatus st = m.status();
        assertTrue("唯一副本（备份）绝不能被删", bak.isDirectory());
        assertTrue("记录必须保留", new File(dir, ".txn_p1").exists());
        assertFalse("不得报就绪", st.isReady());
        assertEquals("必须给出可执行的人工了结入口", "p1", st.ambiguousSwapProfileId);

        // 目标缺失时「保留当前目录」无从谈起：显式拒绝，而不是静默无反应
        FaceModelManager.Result keepNew = m.resolveAmbiguousSwap("p1", true);
        assertEquals("目标缺失时不得接受「保留当前目录」",
                FaceModelManager.Status.INVALID_ARGUMENT, keepNew.status);
        assertTrue("拒绝时更要保住唯一副本", bak.isDirectory());

        // 唯一可行且正确的选择：还原为备份
        assertTrue("应能经「还原为备份」把配置救回",
                m.resolveAmbiguousSwap("p1", false).isOk());
        assertTrue("内容应为原模型",
                java.util.Arrays.equals(oldBin, readFile(new File(target, DET_BIN))));
        assertFalse("备份已被消费", bak.exists());
        assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
        assertTrue("救回后应就绪", m.status().isReady());
        assertNoTempArtifacts(dir);
    }

    /**
     * 启动闸门：恢复未完成时 {@code recoverForStartup} 必须返回 blockedReason 且 activeProfileId
     * 为空——调用方据此**停止**自动激活，绝不猜测哪个配置该生效（约束 2-1）。
     */
    @Test
    public void startupRecoveryBlocksAutoActivationWhenRecoveryIncomplete() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
        assertTrue(m.activate("p1", ABI, true).isOk());
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false, TXN_A);   // 未提交

        FaultyFileOps ops = new FaultyFileOps();
        ops.failDeleteForName("p1");   // 新目录删不掉 ⇒ 回滚失败
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FaceModelManager.StartupRecovery sr = m.recoverForStartup();
            assertNull("恢复未完成时不得给出激活目标", sr.activeProfileId);
            assertNotNull("必须给出阻塞理由", sr.blockedReason);
            assertTrue("必须保留记录以待重试", new File(dir, ".txn_p1").exists());
            assertTrue("必须保留两份数据", target.isDirectory() && bak.isDirectory());
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 约束 2-2：用户**显式选择**的目标不得被恢复结果偷换。另一个 profile 的未决现场只会在
     * 它自己被触碰时处理，不得改写用户刚选的配置。
     */
    @Test
    public void explicitActivationIsNotReplacedByPendingTxnOfOtherProfile() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);

        // ⚠️ 关键（A1 审计 N1 修正）：**导入会把指针置成刚导入的那个 profile**（importZip 成功路径
        // 无条件 persistActive，与 hadOld 无关）。故必须让「最后一次导入」与「用户将要 activate 的目标」
        // **不同**，否则指针恰好等于用户选择，任何「用指针值替代实参」的实现注入都不会改变结果——
        // 用例就成了空守卫（初版正是如此，A1 用探针实测它保持绿）。
        Map<String, byte[]> va = defaultFiles("va");
        assertTrue(m.importZip(stream(zip("", manifest("pA", "[\"arm64-v8a\"]", va, "lic"), va)), ABI, true).isOk());
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        // p1 留下**证据不足**的未决现场（旧格式 + 同名），应原样滞留
        File target1 = m.profileDir("p1");
        File bak1 = new File(dir, ".bak_p1");
        assertTrue(target1.renameTo(bak1));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target1, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);
        writeTxnFile(dir, "p1", true, "p1", false);

        Map<String, byte[]> vp2 = defaultFiles("vp2");
        assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", vp2, "lic"), vp2)), ABI, true).isOk());
        assertEquals("最后一次导入后指针应指向 p2（= 下面用户选择的**相反**侧）", "p2", m.activeProfileId());

        // 用户显式选择 pA —— 而此刻指针是 p2。若实现用指针值替代实参，这里会去激活 p2。
        assertTrue(m.activate("pA", ABI, true).isOk());

        FaceRuntimeStatus st = m.status();
        assertEquals("用户选择的 pA 不得被指针值（p2）偷换", "pA", st.profileId);
        assertTrue("另一 profile 的未决现场应原样滞留（只在该 profile 被触碰时处理）",
                new File(dir, ".txn_p1").exists() && bak1.isDirectory() && target1.isDirectory());
    }

    /**
     * 指针格式兼容：旧格式（裸 profileId）与新格式（{@code profileId=} + {@code txnId=}）都必须
     * 解析出同一 profileId；**非法代次必须降级为「无证据」而不得污染 profileId**。
     */
    @Test
    public void activePointerParsesLegacyAndTokenFormats() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

        writeActivePointer(dir, "p1");
        assertEquals("p1", m.activeProfileId());
        assertNull("旧格式无提交证据", readPointerTxnId(dir));

        writeActivePointer(dir, "p1", TXN_A);
        assertEquals("p1", m.activeProfileId());
        assertEquals(TXN_A, readPointerTxnId(dir));

        // 非法代次（长度/字符集不符）不得污染 profileId，也**不得成为提交证据**。
        java.nio.file.Files.write(new File(dir, FaceModelManager.ACTIVE_POINTER_FILE).toPath(),
                "profileId=p1\ntxnId=zzz\n".getBytes(StandardCharsets.UTF_8));
        assertEquals("非法代次不得污染 profileId", "p1", m.activeProfileId());

        // 判别力验证：指针与记录都写**同一个**脏串 "zzz"。若任一侧把它当成代次，两侧相等就会
        // 伪造出「已提交」判定（保留新目录、报 READY）；正确行为是两侧都降级为无证据 ⇒ 走旧格式
        // 路径 ⇒ 同名覆盖判为歧义、保留现场并报 ERROR。
        writeTxnFile(dir, "p1", true, "p1", false, "zzz");
        File target = m.profileDir("p1");
        File bak = new File(dir, ".bak_p1");
        assertTrue(target.renameTo(bak));
        Map<String, byte[]> v2 = defaultFiles("v2");
        writeProfileDir(target, manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2);

        FaceRuntimeStatus st = m.status();
        assertFalse("脏代次不得被当成提交证据（否则会误判为已提交而保留新目录）", st.isReady());
        assertEquals("应走旧格式歧义路径", "p1", st.ambiguousSwapProfileId);
    }

    /**
     * **外部审查 P1 #1 的真实导入复现**：{@code committed} 改写失败 **且** 备份也删不掉时，
     * **绝不回滚一次成功的导入**。
     *
     * <p>旧实现把「记录残留会收敛到保留新目录」建立在「备份确实被清掉」这一前提上；前提不成立时
     * 现场退化成 {@code in_progress + target + bak}，恢复按「未提交」把新目录删掉——而 native 仍在
     * 跑新模型。现在提交证据是指针里的代次，恢复据此判定已提交，只重试清理备份。
     *
     * <p>用可编程闸门表达「第 1 次 writeSync（in_progress）成功、第 2 次（committed）失败」——
     * 旧的全局布尔接缝**根本无法表达**这个组合。
     */
    @Test
    public void committedRewriteAndBackupCleanupBothFailDoesNotRollBack() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 只让 committed 改写失败（不影响 in_progress 那次），并让备份删不掉
            ops.failWriteIfContains("state=committed");
            ops.failDeleteForName(".bak_p1");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
            assertTrue("激活确已生效，导入应返回成功：" + r.message, r.isOk());
            assertTrue("现场应退化为 in_progress + target + bak",
                    new File(dir, ".txn_p1").exists() && new File(dir, ".bak_p1").isDirectory());

            ops.resetGates();
            FaceRuntimeStatus st = m.status();
            assertTrue("恢复后应就绪", st.isReady());
            assertEquals("p1", st.profileId);
            assertTrue("**不得回滚一次已提交的导入**：内容必须是新模型",
                    java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(m.profileDir("p1"), DET_BIN))));
            assertFalse("备份应被清理", new File(dir, ".bak_p1").exists());
            assertFalse("记录应被清除", new File(dir, ".txn_p1").exists());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * **第三代理 2026-09-18 实证复现的真实缺口**：提交证据**不得**随激活指针移动而丢失。
     *
     * <p>场景（可达：Android 上 native mmap 模型文件常致备份删不掉，而 {@code commitSide} 存在
     * 的意义正是覆盖它）：同名覆盖 p2 的 {@code committed} 改写失败 **且** 备份删不掉 → 记录停在
     * {@code in_progress}；此时用户切到别的 profile，指针被换成 p1 → **旧实现把 p2 的提交证据一并
     * 摧毁**（证据只存在指针里），之后再触碰 p2 就把它**静默回滚**掉一次成功的导入。
     *
     * <p>见证文件 {@code .commit_<id>} 按 profile 固定命名、写在提交动作最前沿，指针怎么移动都与
     * 它无关，故「切走再切回」不再造成任何回滚。
     */
    @Test
    public void commitEvidenceSurvivesPointerMoveAndDoesNotRollBack() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 只让 committed 改写失败（不影响 in_progress 那次），并让备份删不掉 → 记录停在 in_progress
            ops.failWriteIfContains("state=committed");
            ops.failDeleteForName(".bak_p2");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
            assertTrue("激活确已生效，导入应返回成功：" + r.message, r.isOk());
            assertTrue("现场应退化为 in_progress + target + bak",
                    new File(dir, ".txn_p2").exists() && new File(dir, ".bak_p2").isDirectory());
            assertNotNull("见证的提交代次不得为空", readCommitWitnessTxnId(dir, "p2"));
            assertEquals("此刻见证与指针应带同一个提交代次（指针稍后会被覆盖，见证不会）",
                    readPointerTxnId(dir), readCommitWitnessTxnId(dir, "p2"));

            // ★ 指针移动：切到 p1。旧实现自此丢掉 p2 的提交证据。
            assertTrue(m.activate("p1", ABI, true).isOk());
            assertNotNull("指针应已带 p1 的新鲜代次", readPointerTxnId(dir));

            // 解除文件故障后触碰 p2：恢复必须判「已提交」→ 只重试清理，绝不回滚目录。
            ops.resetGates();
            assertTrue(m.activate("p2", ABI, true).isOk());

            assertTrue("提交证据不得随指针移动而丢失：p2 必须保留新模型",
                    java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(m.profileDir("p2"), DET_BIN))));
            assertFalse("已提交 ⇒ 备份应被清理", new File(dir, ".bak_p2").exists());
            assertFalse("已提交 ⇒ 记录应被清除", new File(dir, ".txn_p2").exists());
            assertFalse("已提交 ⇒ 见证应被清除", commitWitnessOf(dir, "p2").exists());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 见证**单独**即可判定「已提交」：即便激活指针的代次与事务代次不同（指针已移走/已重盖），
     * 恢复也必须保留新目录。
     *
     * <p>夹具直接构造现场，把机制与导入流程解耦——这样即便将来导入流程重构，本用例仍能守住
     * 「见证 ⇒ 已提交」这条判据。
     */
    @Test
    public void commitWitnessAloneIsSufficientEvidenceToKeepNewDir() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        Map<String, byte[]> v2 = defaultFiles("v2");

        File target = m.profileDir("p2");
        writeProfileDir(target, manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2);
        File bak = new File(dir, ".bak_p2");
        writeProfileDir(bak, manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1);
        writeTxnFile(dir, "p2", true, "p1", false, TXN_A);      // 停在 in_progress
        writeCommitWitness(dir, "p2", TXN_A);                   // 提交见证（与事务代次一致）
        writeActivePointer(dir, "p2", TXN_B);                   // 指针代次**不等于**事务代次

        FaceRuntimeStatus st = m.status();

        assertTrue("见证成立即已提交，应报就绪：" + st.message, st.isReady());
        assertEquals("p2", st.profileId);
        assertTrue("已提交 ⇒ 保留新目录",
                java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse("已提交 ⇒ 清理备份", bak.exists());
        assertFalse("已提交 ⇒ 记录清除", new File(dir, ".txn_p2").exists());
        assertFalse("已提交 ⇒ 见证清除", commitWitnessOf(dir, "p2").exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 见证**不得**被伪造：代次与事务不符（或非法、或见证缺失）一律降级为「无证据」⇒ 判未提交
     * ⇒ 回滚目录。
     *
     * <p>判别力来自「代次是每次不同的 nonce」：脏内容一旦参与相等比较，就可能伪造出一次
     * 「已提交」而把**未提交**的新目录留在激活位。
     *
     * <p>⚠️ 三支子用例里**只有 {@code TXN_B}（不匹配）与 {@code null}（无见证）承重**；
     * {@code "zzz"} 属格式健壮性用例、**不具独立判别力**：记录的代次恒为合法 nonce（{@code newTxnId}），
     * 故「见证侧做不做 {@code isSafeTxnId} 校验」对 {@code isCommitted} 的等值比较结果完全相同
     * （A2 审计 F3 实测）。保留它是因为它锁住了「非法代次不得被当成证据」这条契约本身。
     */
    @Test
    public void commitWitnessWithMismatchedOrInvalidTxnIdIsNotEvidence() throws Exception {
        for (String witnessTxn : new String[]{TXN_B, "zzz", null}) {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            Map<String, byte[]> v2 = defaultFiles("v2");

            File target = m.profileDir("p2");
            writeProfileDir(target, manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2);
            File bak = new File(dir, ".bak_p2");
            writeProfileDir(bak, manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1);
            writeTxnFile(dir, "p2", true, "p1", false, TXN_A);
            if (witnessTxn != null) {
                writeCommitWitness(dir, "p2", witnessTxn);
            }
            writeActivePointer(dir, "p2", TXN_B);

            FaceRuntimeStatus st = m.status();

            assertTrue("无有效见证 ⇒ 必须回滚到旧目录（witnessTxn=" + witnessTxn + "）",
                    java.util.Arrays.equals(v1.get(DET_BIN), readFile(new File(target, DET_BIN))));
            assertFalse("回滚应消费备份（witnessTxn=" + witnessTxn + "）", bak.exists());
            assertFalse("回滚后记录应清除（witnessTxn=" + witnessTxn + "）",
                    new File(dir, ".txn_p2").exists());
            assertTrue("回滚后应就绪：" + st.message, st.isReady());
            assertNoTempArtifacts(dir);
        }
    }

    /**
     * **长 profileId 不得截断提交证据**（A2 审计 F1）。
     *
     * <p>现场读取原本用**定长缓冲单次 {@code read()}**（见证 256、事务记录 512）。而 profileId
     * **没有长度上限**（{@code isSafeDirName} 只拒绝空 / {@code .} / {@code ..} / 分隔符，
     * {@code FaceModelManifest} 只要求非空），于是稍长的 profileId 就会把 {@code txnId} 行挤出缓冲
     * → 见证被判「无证据」、记录被降级为「旧格式」→ 在「指针已移走」的现场把一次**成功**的导入
     * 静默回滚。这与本批要消灭的失败模式是同一个。
     *
     * <p>本用例把 profileId 拉到 240 字符：见证内容 ≈ 289 字节（&gt; 256），事务记录 ≈ 572 字节
     * （&gt; 512，因为 profileId 与 prevActive 各出现一次）。指针指向**别的** profile，故「已提交」
     * 只能由见证证明——旧实现两条读都被截断后走旧格式启发式，会判成未提交而回滚。
     */
    @Test
    public void longProfileIdDoesNotTruncateCommitEvidence() throws Exception {
        StringBuilder sb = new StringBuilder("long");
        while (sb.length() < 240) {
            sb.append('x');
        }
        String id = sb.toString();

        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        Map<String, byte[]> v2 = defaultFiles("v2");

        File target = m.profileDir(id);
        writeProfileDir(target, manifest(id, "[\"arm64-v8a\"]", v2, "lic"), v2);
        File bak = new File(dir, ".bak_" + id);
        writeProfileDir(bak, manifest(id, "[\"arm64-v8a\"]", v1, "lic"), v1);
        // prevActive 也取长 id：让记录超过原先的 512 字节缓冲
        writeTxnFile(dir, id, true, id, false, TXN_A);
        writeCommitWitness(dir, id, TXN_A);
        writeActivePointer(dir, "p1", TXN_B);   // 指针在**别的** profile，且代次与本事务无关

        assertTrue("夹具前提：见证内容必须超过原先的 256 字节缓冲",
                commitWitnessOf(dir, id).length() > 256);
        assertTrue("夹具前提：事务记录必须超过原先的 512 字节缓冲",
                new File(dir, ".txn_" + id).length() > 512);

        // 触碰该 profile（activate 入口会先恢复）——「已提交」只能由见证证明。
        FaceModelManager.Result r = m.activate(id, ABI, true);
        assertTrue("恢复后应能正常激活：" + r.message, r.isOk());

        assertTrue("长 profileId 下提交证据不得被截断：新目录必须保留",
                java.util.Arrays.equals(v2.get(DET_BIN), readFile(new File(target, DET_BIN))));
        assertFalse("已提交 ⇒ 备份应被清理", bak.exists());
        assertFalse("已提交 ⇒ 记录应被清除", new File(dir, ".txn_" + id).exists());
        assertFalse("已提交 ⇒ 见证应被清除", commitWitnessOf(dir, id).exists());
        assertNoTempArtifacts(dir);
    }

    /**
     * 见证写不进去 ⇒ **绝不越过提交点**（与「{@code in_progress} 记录写不进去就绝不动手」同一条
     * 原则）：回滚到旧配置、失败返回，且不留下任何见证残留。
     */
    @Test
    public void commitWitnessWriteFailureRollsBackWithoutCrossingCommitPoint() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 记录导入前的指针代次：profileId 在两种结果下都是 "p2"（同名覆盖），**不具判别力**；
            // 能判别「有没有越过提交点」的是代次——越过则 persistActive 会盖上**新鲜**代次。
            String pointerTxnBefore = readPointerTxnId(dir);
            // 见证的暂存文件已迁入 .staging/commit/p2——它与事务记录的暂存文件**同名**（都是 p2），
            // 故不能按文件名注入，改按**子目录路径**注入：只命中见证这一侧的 writeSync。
            ops.failWriteForPathContains(STAGING_DIR + "/commit/p2");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertFalse("见证落不下就不该越过提交点：" + r.message, r.isOk());
            assertTrue("旧配置必须回滚就位",
                    java.util.Arrays.equals(v1.get(DET_BIN), readFile(new File(m.profileDir("p2"), DET_BIN))));
            assertFalse("不得残留见证", commitWitnessOf(dir, "p2").exists());
            assertFalse("不得残留记录", new File(dir, ".txn_p2").exists());
            assertFalse("不得残留备份", new File(dir, ".bak_p2").exists());
            assertEquals("提交点未越过 ⇒ 指针不得被改动", pointerTxnBefore, readPointerTxnId(dir));
            assertEquals("指针仍指向 p2", "p2", m.activeProfileId());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 激活指针写失败 ⇒ 见证必须被**撤销**。
     *
     * <p>否则会留下自相矛盾的状态：现场已回滚（新目录被删、旧目录还原），而盘上却有一条
     * 「本次事务已提交」的见证——下次恢复会据此把**已回滚**的现场再翻成「保留新目录」。
     *
     * <p>⚠️ **本用例不判别「有没有显式撤销」**：这里回滚是**成功**的，而 {@code rollbackProfileSwap}
     * 内部的 {@code finishTxn} 会顺带把见证清掉——即使删掉 {@code importZip} 里那次显式撤销，本用例
     * 仍全绿（A1 审计 Low-2 实测）。判别显式撤销的职责在
     * {@link #commitWitnessIsRevokedBeforeRollbackSoFailedRollbackStaysUncommitted}（那里回滚失败、
     * 记录被保留，两种实现才显出差别）。保留本用例是因为它覆盖「撤销 + 回滚成功」这条组合路径。
     */
    @Test
    public void commitWitnessIsRevokedWhenPointerWriteFails() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            ops.failWriteForName(FaceModelManager.ACTIVE_POINTER_FILE + ".tmp");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertFalse("指针写失败应失败返回：" + r.message, r.isOk());
            assertTrue("旧配置必须回滚就位",
                    java.util.Arrays.equals(v1.get(DET_BIN), readFile(new File(m.profileDir("p2"), DET_BIN))));
            assertFalse("见证必须被撤销（否则下次恢复会把已回滚的现场翻成已提交）",
                    commitWitnessOf(dir, "p2").exists());
            assertFalse("不得残留记录", new File(dir, ".txn_p2").exists());
            assertEquals("指针写失败 ⇒ 保持原激活配置", "p2", m.activeProfileId());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 指针写失败 **且回滚也失败** 时，见证必须**已被撤销**。
     *
     * <p>这条才是「先撤销见证、再回滚」真正承重的地方：回滚**成功**时 {@code rollbackProfileSwap}
     * 内部的 {@code finishTxn} 顺带就把见证清了，掩盖了「有没有显式撤销」的差别；只有回滚失败、
     * 记录被保留时差别才显出来——见证若还在，下一次恢复会判「已提交」，把一个**从未提交**的现场
     * 翻成「保留新目录」，而磁盘上留着的其实是那个没被激活成功的新包。
     */
    @Test
    public void commitWitnessIsRevokedBeforeRollbackSoFailedRollbackStaysUncommitted() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 指针写失败 + 回滚（.bak_p2 → p2）也失败 ⇒ 记录与备份都滞留现场
            ops.failWriteForName(FaceModelManager.ACTIVE_POINTER_FILE + ".tmp");
            ops.failRenameFromName(".bak_p2");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
            assertFalse("指针写失败应失败返回：" + r.message, r.isOk());
            assertFalse("见证必须在回滚**之前**就被撤销", commitWitnessOf(dir, "p2").exists());
            assertTrue("回滚失败 ⇒ 记录保留待下次重试", new File(dir, ".txn_p2").exists());

            // 解除故障：恢复必须判「未提交」→ 继续回滚到旧目录，而不是被残留见证翻成「保留新目录」。
            ops.resetGates();
            FaceRuntimeStatus st = m.status();
            assertTrue("恢复后应就绪：" + st.message, st.isReady());
            assertTrue("未提交的事务不得被判为已提交（内容应为旧模型）",
                    java.util.Arrays.equals(v1.get(DET_BIN), readFile(new File(m.profileDir("p2"), DET_BIN))));
            assertFalse(new File(dir, ".txn_p2").exists());
            assertFalse(new File(dir, ".bak_p2").exists());
            assertFalse(commitWitnessOf(dir, "p2").exists());

            // `.rollback_p2` 意图标记会滞留——既有行为（rollbackDirOnly 不清它，buildStatus 已容忍
            // 「标记在、备份已被消费」并照常判 READY），属既有遗留、非本用例考察对象。清掉再做卫生断言。
            assertTrue("清理人造标记", new File(dir, ".rollback_p2").delete());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    // ---------- ⑲ 布局改造：临时文件迁入恒定名暂存目录 .staging/ ----------

    /**
     * A 路径回归（清扫误删）：profileId 以 {@code .tmp} 结尾时，其**活事务记录** {@code .txn_<id>}
     * 与「profileId 去掉 .tmp 后缀后的记录临时文件」**同名**（{@code p.tmp} 的记录即 {@code .txn_p.tmp}）。
     * 旧清扫按「{@code .txn_} 前缀 + {@code .tmp} 后缀」猜测，会把这份**活证据**当成临时文件删掉，
     * 使本可自动收敛的事务退化为必须人工了结的歧义现场。
     *
     * <p>临时文件迁入恒定名暂存目录 {@code .staging/} 后，清扫不再据名猜测（只清 {@code .staging/} 与
     * {@code .tmp_*}），活记录安然无恙。
     */
    @Test
    public void sweepDoesNotDeleteLiveTxnRecordOfDotTmpProfileId() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);

            // 构造 profileId="p.tmp" 的**未了结**替换现场：新旧目录并存 + in_progress 记录（未提交）。
            Map<String, byte[]> vNew = defaultFiles("new");
            Map<String, byte[]> vOld = defaultFiles("old");
            writeProfileDir(m.profileDir("p.tmp"),
                    manifest("p.tmp", "[\"arm64-v8a\"]", vNew, "lic"), vNew);
            writeProfileDir(new File(dir, ".bak_p.tmp"),
                    manifest("p.tmp", "[\"arm64-v8a\"]", vOld, "lic"), vOld);
            writeTxnFile(dir, "p.tmp", true, "pX", false, TXN_A);   // 记录名恰为 .txn_p.tmp

            // 让恢复无法自行收敛（bak→target 改名失败），以保住这份「未了结」现场供判定。
            ops.failRenameFromName(".bak_p.tmp");

            // 触发清扫：对**另一个** profile 导入（importZip 开头即 sweepStaleTempDirs）。
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                    ABI, true).isOk());

            assertTrue("活事务记录不得被清扫误删（它与「profileId 为 p 的临时文件」同名）",
                    new File(dir, ".txn_p.tmp").exists());

            // 现场判定正确：仍据 in_progress 记录报「未提交事务」，而非退化成「无记录 → 无法判定」。
            FaceModelManager.Result r = m.activate("p.tmp", ABI, true);
            assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
            assertTrue("应据事务记录判为未提交事务，实际=" + r.message,
                    r.message.contains("未提交事务"));
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * B 路径回归（写入覆盖，与清扫无关）：写 profile {@code p} 的事务记录**临时文件**不得破坏
     * profileId {@code p.tmp} 的**正式记录** {@code .txn_p.tmp}。
     *
     * <p>旧实现把临时文件放在顶层、名字由 profileId 拼成（{@code .txn_p.tmp}），与 {@code p.tmp} 的
     * 正式记录逐字同名，写入即以**截断**方式覆盖销毁它。临时文件迁入 {@code .staging/txn/p} 后，
     * 这条独立破坏路径被结构性消除。
     */
    @Test
    public void writeTxnTempFileDoesNotClobberDotTmpProfileRecord() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);

        // 先造 p.tmp 的活记录，并记下其逐字节内容。
        writeTxnFile(dir, "p.tmp", true, "pX", false, TXN_A);
        File live = new File(dir, ".txn_p.tmp");
        assertTrue(live.isFile());
        byte[] before = readFile(live);

        // 让 p 走一次真实导入（其 writeTxn 会写暂存文件 → 改名落到 .txn_p）。
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                ABI, true).isOk());

        assertTrue("p.tmp 的正式事务记录必须完好存在", live.isFile());
        assertTrue("p.tmp 的记录内容必须逐字节未被覆盖",
                java.util.Arrays.equals(before, readFile(live)));
        assertTrue("记录仍应被解析为 p.tmp 的现场",
                new String(readFile(live), StandardCharsets.UTF_8).contains("profileId=p.tmp"));
    }

    /** 正常路径结束后暂存目录 {@code .staging} 不得残留任何文件。 */
    @Test
    public void stagingDirLeavesNoFilesAfterSuccessfulImport() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");
        assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                ABI, true).isOk());
        assertNoStagingFiles(dir);
        assertNoTempArtifacts(dir);
    }

    /**
     * writeTxn 的 {@code moveReplace} 失败 ⇒ **绝不越过破坏性改名**：导入中止、旧配置一字未动、
     * 无正式记录/备份/暂存残留。（此前该失败分支零用例覆盖。）
     */
    @Test
    public void writeTxnMoveReplaceFailureAbortsBeforeAnyDestructiveChange() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                    ABI, true).isOk());

            // 只让「暂存文件 → .txn_p1」这一步改名失败（源是暂存文件、目标是正式记录名）。
            ops.failMoveReplaceForName(".txn_p1");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertEquals(FaceModelManager.Status.ATOMIC_RENAME_FAILED, r.status);
            assertTrue("应说明无法记录事务而中止：" + r.message,
                    r.message.contains("无法记录目录替换事务"));
            assertTrue("旧配置必须一字未动",
                    java.util.Arrays.equals(v1.get(DET_BIN),
                            readFile(new File(m.profileDir("p1"), DET_BIN))));
            assertFalse("不得残留正式记录", new File(dir, ".txn_p1").exists());
            assertFalse("不得残留备份", new File(dir, ".bak_p1").exists());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * writeCommitWitness 的 {@code moveReplace} 失败 ⇒ 与「见证写不进去」同一条原则：**绝不越过
     * 提交点**，回滚到旧配置并失败返回，不留见证/记录/备份/暂存残留。（此前该失败分支零用例覆盖。）
     */
    @Test
    public void commitWitnessMoveReplaceFailureDoesNotCrossCommitPoint() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                    ABI, true).isOk());
            assertTrue(m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v1, "lic"), v1)),
                    ABI, true).isOk());

            String pointerTxnBefore = readPointerTxnId(dir);
            // 只让「暂存文件 → .commit_p2」这一步改名失败（见证那侧）。
            ops.failMoveReplaceForName(".commit_p2");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);

            assertFalse("见证落不下就不该越过提交点：" + r.message, r.isOk());
            assertTrue("旧配置必须回滚就位",
                    java.util.Arrays.equals(v1.get(DET_BIN),
                            readFile(new File(m.profileDir("p2"), DET_BIN))));
            assertFalse("不得残留见证", commitWitnessOf(dir, "p2").exists());
            assertFalse("不得残留记录", new File(dir, ".txn_p2").exists());
            assertFalse("不得残留备份", new File(dir, ".bak_p2").exists());
            assertEquals("提交点未越过 ⇒ 指针不得被改动", pointerTxnBefore, readPointerTxnId(dir));
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * 「{@code writeTxn} 的 {@code writeSync} **中途失败会留下半截临时文件**，由下次导入的清扫回收」
     * ——这条设计前提必须有**行为验证**（A2 审计 Low-2），不能只由注释承担。
     *
     * <p>前提为何成立：{@code DEFAULT_FILE_OPS.writeSync} 先 {@code new FileOutputStream(f,false)}
     * 建/截断文件再 {@code write}，故磁盘写满之类的失败会留下半截文件；而普通失败闸门命中时**根本不建
     * 文件**，所以既有用例从未构造过这个现场。{@code writeTxn} **刻意保留**了旧的自删策略（只在
     * {@code moveReplace} 失败时删），半截文件的回收**完全依赖**「下次导入整体清空 {@code .staging/}」
     * ——本用例把这条链路钉死。
     *
     * <p>注意与 {@code writeCommitWitness} 的分工：后者在 {@code writeSync} 失败时**显式删除**临时文件
     * （早先 A2-F4 的修复），故见证侧不留半截文件。本用例只钉 {@code writeTxn} 这一侧。
     */
    @Test
    public void partialWriteFailureLeavesStagingFileThatNextImportSweeps() throws Exception {
        FaultyFileOps ops = new FaultyFileOps();
        FaceModelManager.setFileOpsForTesting(ops);
        try {
            FakeRuntime rt = new FakeRuntime();
            File dir = tmp.newFolder();
            FaceModelManager m = new FaceModelManager(dir, rt);
            Map<String, byte[]> v1 = defaultFiles("v1");
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());

            // 只让事务记录的暂存文件写一半就失败（该分支刻意不自删）
            ops.failWritePartialForPathContains(STAGING_DIR + "/txn/p2");
            Map<String, byte[]> v2 = defaultFiles("v2");
            FaceModelManager.Result r =
                    m.importZip(stream(zip("", manifest("p2", "[\"arm64-v8a\"]", v2, "lic"), v2)), ABI, true);
            assertFalse("记录写失败应失败返回：" + r.message, r.isOk());

            File partial = new File(dir, STAGING_DIR + "/txn/p2");
            assertTrue("设计前提：真实的写失败会留下半截临时文件（" + partial + "）", partial.isFile());
            assertTrue("半截文件应非空", partial.length() > 0);

            // 下一次导入的清扫必须回收它——这正是「.staging 可安全整体清空」的收益
            ops.resetGates();
            assertTrue(m.importZip(stream(zip("", manifest("p1", "[\"arm64-v8a\"]", v1, "lic"), v1)), ABI, true).isOk());
            assertFalse("下次导入的清扫必须回收半截临时文件", partial.exists());
            assertNoTempArtifacts(dir);
        } finally {
            FaceModelManager.setFileOpsForTesting(null);
        }
    }

    /**
     * **存量暴露**：升级前导入、profileId 含内部控制字符的 profile，状态查询会由 READY 变为
     * 「激活配置 manifest 无效」——因为 `isImportSafeProfileId` 接在 `parse` 上，而 `parse` 也被
     * `buildStatus` 使用。这条后果此前只在文档与 javadoc 里陈述、**无常驻用例**（第三代理指出的
     * 覆盖缺口），本用例把它钉住。
     *
     * <p>用 DEL(0x7F) 而非 {@code \n}/{@code \t}：后者属 Win32 **文件名禁用字符**（0x00-0x1F），
     * 在本机 Windows 测试宿主上根本建不出该目录；DEL 不在禁用集内，故夹具在两平台都能建。判据对
     * 二者一视同仁（都是「C0 控制字符或 DEL」这一条）。
     *
     * <p>对照组用同一夹具结构 + 干净 id，证明 ERROR 确由字符集判据引起，而非夹具本身有问题。
     */
    @Test
    public void existingProfileWithControlCharIdIsReportedAsInvalidManifest() throws Exception {
        FakeRuntime rt = new FakeRuntime();
        File dir = tmp.newFolder();
        FaceModelManager m = new FaceModelManager(dir, rt);
        Map<String, byte[]> v1 = defaultFiles("v1");

        // 对照组：干净 id ⇒ 就绪
        String cleanId = "p1";
        writeProfileDir(m.profileDir(cleanId), manifest(cleanId, "[\"arm64-v8a\"]", v1, "lic"), v1);
        writeActivePointer(dir, cleanId);
        FaceRuntimeStatus ok = m.status();
        assertTrue("对照组应就绪：" + ok.message, ok.isReady());

        // 含 DEL 的 id：manifest 里用 JSON 转义写法，解析后才是真实 DEL
        String badId = "a" + (char) 0x7F + "b";
        String badIdJson = "a\\u007fb";
        writeProfileDir(m.profileDir(badId), manifest(badIdJson, "[\"arm64-v8a\"]", v1, "lic"), v1);
        writeActivePointer(dir, badId);

        FaceRuntimeStatus st = m.status();
        assertFalse("存量含控制字符的 profile 应报 ERROR（不得谎报 READY）", st.isReady());
        assertTrue("应归为 manifest 无效，实际=" + st.message, st.message.contains("manifest 无效"));
        assertTrue("应指出是 profileId 字符非法，实际=" + st.message, st.message.contains("非法字符"));
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
        // 暂存目录独立判：它本身就带点号、名字合法，不能靠下面的前缀列表拦住；且必须**递归**检查，
        // 否则残缺临时文件会「躲进子目录」而漏检（会让「.staging 已空」这类断言变弱）。
        assertNoStagingFiles(storeDir);
        File[] ch = storeDir.listFiles();
        if (ch == null) {
            return;
        }
        for (File f : ch) {
            String n = f.getName();
            assertFalse("残留临时/备份内容: " + n,
                    n.startsWith(".tmp_") || n.startsWith(".bak_") || n.startsWith(".rollback_")
                            || n.startsWith(".txn_")
                            || n.startsWith(FaceModelManager.COMMIT_WITNESS_PREFIX)
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".tmp")
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".bak"));
        }
    }

    /**
     * 暂存目录 {@code .staging} 要么不存在、要么**递归地不含任何文件**（允许残留空目录）。
     *
     * <p>为何必须递归：临时文件迁入 {@code .staging/txn/} 与 {@code .staging/commit/}，若只查顶层
     * 就会漏掉躲在子目录里的半截文件——正是本次布局改造要防的「漏检」。
     */
    private static void assertNoStagingFiles(File storeDir) {
        File staging = new File(storeDir, STAGING_DIR);
        assertFalse("暂存目录下不得残留任何文件（残缺临时文件不得躲进子目录漏检）",
                staging.exists() && containsRegularFile(staging));
    }

    /** {@code f} 自身或其任意后代是否为普通文件。 */
    private static boolean containsRegularFile(File f) {
        if (f.isFile()) {
            return true;
        }
        File[] ch = f.listFiles();
        if (ch != null) {
            for (File c : ch) {
                if (containsRegularFile(c)) {
                    return true;
                }
            }
        }
        return false;
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
