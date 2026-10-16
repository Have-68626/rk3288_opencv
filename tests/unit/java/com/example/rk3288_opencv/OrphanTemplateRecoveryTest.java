package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.List;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

/**
 * 孤儿模板旁置（A15）单元测试。
 *
 * <p>覆盖契约：
 * <ol>
 *   <li>已引用（在册）的模板文件**不能**旁置；</li>
 *   <li>列表后新出现引用（列表与确认之间注册表变化）时，旁置必须**重读**并拒绝；</li>
 *   <li>缺主表 / 旧版 FREG1 / 不完整 FREG2 时**只读**（不可旁置）；</li>
 *   <li>旁置失败时**原文件保留**；</li>
 *   <li>恢复目录同名冲突时**不覆盖**既有恢复文件（取唯一名）；</li>
 *   <li>非模板命名文件与子目录**不处理**；</li>
 *   <li>旁置成功后释放原名，可用同一 userId 重新注册，且恢复副本保留。</li>
 * </ol>
 *
 * <p>仓库层用例；{@code MainActivity} 是 {@code AppCompatActivity}，纯 JVM 不可构造，其 UI 接线由
 * 源码结构（与本包既有形态守卫同法）另行保证，不在此覆盖。
 */
public class OrphanTemplateRecoveryTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int MV = 7;
    private static final int TSV = 2;

    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
        provider = jceProvider();
    }

    // ---------- ① 已引用文件不能旁置 ----------

    @Test
    public void referencedTemplateCannotBeQuarantined() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(40, 1), MV, TSV).status);
        String name = repo.templateFileFor("alice").getName();

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(name);

        assertFalse("已引用的模板必须拒绝旁置", r.isOk());
        assertTrue("原文件必须保留", repo.templateFileFor("alice").exists());
        assertFalse("不得生成恢复副本", new File(recoveryDir(repo), name).exists());
    }

    // ---------- ② 列表后新引用导致拒绝 ----------

    @Test
    public void newlyReferencedFileIsRejectedOnQuarantine() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("seed", templateBytes(40, 2), MV, TSV).status);
        // 造一个孤儿（ghost 不在注册表），经列表可见且允许旁置。
        writeOrphanTemplate(repo, "ghost", 3);
        String ghostName = repo.templateFileFor("ghost").getName();

        AndroidEnrollmentRepository.OrphanListResult listed = repo.listOrphanTemplates();
        assertTrue("前置条件：完整 FREG2 ⇒ 允许旁置", listed.quarantineAllowed);
        assertTrue("前置条件：ghost 应被列为孤儿", listed.fileNames.contains(ghostName));

        // 列表之后，注册表新增对 ghost 的引用（外部改表：ghost 已在册）。
        List<String> lines = Files.readAllLines(repo.registryFile().toPath(), StandardCharsets.UTF_8);
        String seedRow = lines.get(2);
        String ghostRow = hex("ghost") + " 7 2 48 " + repeat('a', 64) + " 1";
        Files.write(repo.registryFile().toPath(),
                ("FREG2\nCOUNT 2\n" + seedRow + "\n" + ghostRow + "\nEND 2\n")
                        .getBytes(StandardCharsets.UTF_8));

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(ghostName);

        assertFalse("确认后新引用必须导致拒绝", r.isOk());
        assertTrue("被拒后原文件必须保留", repo.templateFileFor("ghost").exists());
        assertFalse("被拒后不得生成恢复副本", new File(recoveryDir(repo), ghostName).exists());
    }

    // ---------- ③ 缺主表 / 旧版 FREG1 / 不完整 FREG2 ⇒ 只读 ----------

    @Test
    public void legacyFreg1RegistryIsViewOnly() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        // 旧版 FREG1 主表（含一行合法条目）。
        Files.write(repo.registryFile().toPath(),
                ("FREG1\n" + hex("seed") + " 7 2 48 " + repeat('a', 64) + " 1\n")
                        .getBytes(StandardCharsets.UTF_8));
        writeOrphanTemplate(repo, "ghost", 4);
        String ghostName = repo.templateFileFor("ghost").getName();

        AndroidEnrollmentRepository.OrphanListResult listed = repo.listOrphanTemplates();
        assertTrue("旧版 FREG1 仍可查看", listed.isOk());
        assertFalse("旧版 FREG1 不得允许旁置", listed.quarantineAllowed);
        assertTrue("旧版 FREG1 仍可列出孤儿", listed.fileNames.contains(ghostName));

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(ghostName);
        assertFalse("旧版 FREG1 下旁置必须拒绝", r.isOk());
        assertTrue("被拒后原文件必须保留", repo.templateFileFor("ghost").exists());
    }

    @Test
    public void truncatedFreg2RegistryIsViewOnly() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        // 截断的 FREG2（缺 END）⇒ 枚举不完整 ⇒ 不可旁置。
        Files.write(repo.registryFile().toPath(),
                ("FREG2\nCOUNT 1\n" + hex("seed") + " 7 2 48 " + repeat('a', 64) + " 1\n")
                        .getBytes(StandardCharsets.UTF_8));
        writeOrphanTemplate(repo, "ghost", 5);
        String ghostName = repo.templateFileFor("ghost").getName();

        AndroidEnrollmentRepository.OrphanListResult listed = repo.listOrphanTemplates();
        assertTrue(listed.isOk());
        assertFalse("不完整 FREG2 不得允许旁置", listed.quarantineAllowed);
        assertTrue(listed.fileNames.contains(ghostName));

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(ghostName);
        assertFalse("不完整 FREG2 下旁置必须拒绝", r.isOk());
        assertTrue("被拒后原文件必须保留", repo.templateFileFor("ghost").exists());
    }

    // ---------- ④ 旁置失败留原件 ----------

    @Test
    public void renameFailureKeepsOriginalFile() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("seed", templateBytes(40, 6), MV, TSV).status);
        writeOrphanTemplate(repo, "ghost", 7);
        File src = repo.templateFileFor("ghost");
        // 把恢复目录名占位成**普通文件** ⇒ 无法在其下建目标 ⇒ rename 失败。
        File recoveryPath = new File(repo.scopeDir(),
                FeatureTemplateEncryptedStore.ORPHAN_RECOVERY_DIR_NAME);
        assertTrue(recoveryPath.createNewFile());

        AndroidEnrollmentRepository.Result r =
                repo.quarantineOrphanTemplate(src.getName());

        assertFalse("旁置失败必须返回错误", r.isOk());
        assertTrue("旁置失败时原文件必须保留", src.exists());
    }

    // ---------- ⑤ 恢复名冲突不覆盖 ----------

    @Test
    public void recoveryNameConflictDoesNotOverwrite() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("seed", templateBytes(40, 8), MV, TSV).status);
        writeOrphanTemplate(repo, "ghost", 9);
        File src = repo.templateFileFor("ghost");

        // 预置同名恢复文件（哨兵内容），旁置目标必须换名。
        File recoveryDir = recoveryDir(repo);
        assertTrue(recoveryDir.mkdirs());
        File existing = new File(recoveryDir, src.getName());
        byte[] sentinel = "SENTINEL".getBytes(StandardCharsets.UTF_8);
        Files.write(existing.toPath(), sentinel);

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(src.getName());
        assertTrue("应旁置成功（换名）", r.isOk());
        assertTrue("原文件应移走", !src.exists());
        assertArrayEquals("既有恢复文件必须不被覆盖", sentinel, Files.readAllBytes(existing.toPath()));
        // 换名后的副本存在。
        File moved = new File(recoveryDir, src.getName() + "-1");
        assertTrue("应生成唯一名副本", moved.exists());
    }

    // ---------- ⑥ 非模板 / 子目录不处理 ----------

    @Test
    public void nonTemplateFilesAndSubdirectoriesAreIgnored() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("seed", templateBytes(40, 10), MV, TSV).status);

        // 干扰项：非模板命名、长度/大小写不符的伪模板、子目录内的合法命名模板。
        assertTrue(new File(repo.scopeDir(), "other.txt").createNewFile());
        assertTrue(new File(repo.scopeDir(), "u_short.bin").createNewFile());
        assertTrue(new File(repo.scopeDir(), "u_" + repeat('A', 64) + ".bin").createNewFile());
        File sub = new File(repo.scopeDir(), "sub");
        assertTrue(sub.mkdirs());
        assertTrue(new File(sub, "u_" + repeat('a', 64) + ".bin").createNewFile());

        // 一个真正的孤儿（合法命名 + 真实信封）。
        writeOrphanTemplate(repo, "ghost", 11);
        String ghostName = repo.templateFileFor("ghost").getName();

        AndroidEnrollmentRepository.OrphanListResult listed = repo.listOrphanTemplates();

        assertTrue(listed.isOk());
        assertEquals("只应列出唯一的真孤儿", 1, listed.fileNames.size());
        assertEquals(ghostName, listed.fileNames.get(0));
    }

    // ---------- ⑦ 旁置后同名注册成功且副本在 ----------

    @Test
    public void quarantineAllowsSameNameReEnrollmentAndKeepsCopy() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("seed", templateBytes(40, 12), MV, TSV).status);
        writeOrphanTemplate(repo, "ghost", 13);
        File src = repo.templateFileFor("ghost");
        String name = src.getName();

        AndroidEnrollmentRepository.Result r = repo.quarantineOrphanTemplate(name);
        assertTrue("旁置应成功", r.isOk());
        assertFalse("原名应释放", src.exists());
        assertTrue("恢复副本必须保留", new File(recoveryDir(repo), name).exists());
        assertFalse("旁置后该文件不再出现在孤儿列表", repo.listOrphanTemplates().fileNames.contains(name));

        // 原名释放后可重新注册同一 userId。
        assertEquals("旁置后同名应可重新注册", AndroidEnrollmentRepository.Status.OK,
                repo.add("ghost", templateBytes(40, 14), MV, TSV).status);
        assertTrue("重新注册应生成原名文件", src.exists());
    }

    // ---------- ⑧ 自检：接缝与命名 ----------

    @Test
    public void orphanTargetIsUniqueWithinRecoveryDir() throws Exception {
        File dir = tmp.newFolder();
        File occupied = new File(dir, "u_" + repeat('a', 64) + ".bin");
        assertTrue(occupied.createNewFile());
        File picked = AndroidEnrollmentRepository.uniqueOrphanTarget(dir, occupied.getName());
        assertNotEquals("同名时必须换名", occupied.getName(), picked.getName());
        assertFalse("选出的名字必须尚不存在", picked.exists());
    }

    @Test
    public void recoveredCompleteBackupOnlyAllowsViewingOrphans() throws Exception {
        AndroidEnrollmentRepository repo = repositoryWithCorruptMainAndStaleBackup();
        AndroidEnrollmentRepository.ListResult registry = repo.list();
        assertTrue("完整副本仍可读取", registry.isOk());
        assertTrue("副本结构完整不等于权威主表", registry.isEnumerationComplete());
        assertFalse(registry.authoritative);

        AndroidEnrollmentRepository.OrphanListResult listed = repo.listOrphanTemplates();
        assertTrue(listed.isOk());
        assertTrue("陈旧副本漏掉的活模板只可作为待核实项查看",
                listed.fileNames.contains(repo.templateFileFor("later").getName()));
        assertFalse("非权威副本不能授权旁置仍可能在册的模板", listed.quarantineAllowed);
    }

    @Test
    public void directQuarantineRejectsNonAuthoritativeRecoveredBackup() throws Exception {
        AndroidEnrollmentRepository repo = repositoryWithCorruptMainAndStaleBackup();
        File template = repo.templateFileFor("later");
        byte[] original = Files.readAllBytes(template.toPath());
        AndroidEnrollmentRepository.Result result = repo.quarantineOrphanTemplate(template.getName());

        assertEquals("写入口必须独立复核权威性", AndroidEnrollmentRepository.Status.CORRUPTED, result.status);
        assertTrue("被拒绝后原模板必须仍在活目录", template.isFile());
        assertArrayEquals(original, Files.readAllBytes(template.toPath()));
        assertFalse("不得先创建恢复目录再拒绝", recoveryDir(repo).exists());
    }

    private AndroidEnrollmentRepository repositoryWithCorruptMainAndStaleBackup() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertTrue(repo.add("seed", templateBytes(40, 15), MV, TSV).isOk());
        byte[] oldRegistry = Files.readAllBytes(repo.registryFile().toPath());
        assertTrue(repo.add("later", templateBytes(40, 16), MV, TSV).isOk());
        Files.write(new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".bak").toPath(),
                oldRegistry);
        Files.write(repo.registryFile().toPath(), "CORRUPTED\n".getBytes(StandardCharsets.UTF_8));
        return repo;
    }

    // ---------- 工具 ----------

    private static File recoveryDir(AndroidEnrollmentRepository repo) {
        return new File(repo.scopeDir(), FeatureTemplateEncryptedStore.ORPHAN_RECOVERY_DIR_NAME);
    }

    private void writeOrphanTemplate(AndroidEnrollmentRepository repo, String userId, int seed) {
        FeatureTemplateEncryptedStore.WriteResult wr = FeatureTemplateEncryptedStore.write(
                provider, null, repo.templateFileFor(userId), userId, templateBytes(48, seed), MV, TSV);
        assertEquals(FeatureTemplateEncryptedStore.Code.OK, wr.code);
    }

    private AndroidEnrollmentRepository newRepo() throws Exception {
        AndroidEnrollmentRepository r = new AndroidEnrollmentRepository(null, tmp.newFolder());
        r.setKeyProviderForTesting(provider);
        return r;
    }

    private static String repeat(char c, int count) {
        char[] out = new char[count];
        java.util.Arrays.fill(out, c);
        return new String(out);
    }

    /** userId 的 UTF-8 hex（与注册表落盘编码一致，仅用于构造测试数据）。 */
    private static String hex(String userId) {
        byte[] b = userId.getBytes(StandardCharsets.UTF_8);
        char[] HEX = "0123456789abcdef".toCharArray();
        StringBuilder sb = new StringBuilder(b.length * 2);
        for (byte x : b) {
            sb.append(HEX[(x >> 4) & 0xF]).append(HEX[x & 0xF]);
        }
        return sb.toString();
    }

    private static byte[] templateBytes(int len, int seed) {
        byte[] out = new byte[len];
        new java.security.SecureRandom(new byte[]{(byte) seed}).nextBytes(out);
        return out;
    }

    private static FeatureTemplateEncryptedStore.PlatformKeyProvider jceProvider() throws Exception {
        KeyGenerator kg = KeyGenerator.getInstance("AES");
        kg.init(128);
        final SecretKey key = kg.generateKey();
        return new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            @Override
            public boolean isSupported() {
                return true;
            }

            @Override
            public SecretKey getOrCreateKeyV1() {
                return key;
            }
        };
    }
}
