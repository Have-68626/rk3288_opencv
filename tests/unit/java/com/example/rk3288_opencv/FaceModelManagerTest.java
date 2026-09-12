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

    // ---------- ⑦ native 自检门禁 ----------

    @Test
    public void nativeSelfCheckFailureKeepsNotImported() throws Exception {
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

    // ---------- 夹具 ----------

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
                    n.startsWith(".tmp_") || n.startsWith(".bak_")
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".tmp")
                            || n.equals(FaceModelManager.ACTIVE_POINTER_FILE + ".bak"));
        }
    }

    /** 可编程 native 假实现：ok=false 时 loadProfile 返回失败 JSON。 */
    private static final class FakeRuntime implements FaceModelManager.NativeRuntime {
        boolean ok = true;
        String lastManifest;
        String runtimeJson = "{\"ok\":true,\"profileLoaded\":true,\"embeddingDim\":512,"
                + "\"galleryCount\":0,\"enrollmentState\":\"IDLE\"}";

        @Override
        public String loadProfile(String manifestJson, String currentAbi) {
            lastManifest = manifestJson;
            if (ok) {
                return "{\"ok\":true,\"profileId\":\"x\",\"modelVersion\":\"1\",\"embeddingDim\":512}";
            }
            return "{\"ok\":false,\"error\":{\"code\":\"FACE_PROFILE_INVALID\",\"message\":\"bad\"}}";
        }

        @Override
        public String getRuntimeStatus() {
            return runtimeJson;
        }
    }

    /** 记录 activateRuntime 调用的 native 假实现（loadProfile 恒 ok）。 */
    private static final class RecordingRuntime implements FaceModelManager.NativeRuntime {
        int activateCalls = 0;
        String lastBaseDir;

        @Override
        public String loadProfile(String manifestJson, String currentAbi) {
            return "{\"ok\":true,\"profileId\":\"x\",\"modelVersion\":\"1\",\"embeddingDim\":512}";
        }

        @Override
        public String getRuntimeStatus() {
            return "{\"ok\":true,\"profileLoaded\":true,\"embeddingDim\":512}";
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
