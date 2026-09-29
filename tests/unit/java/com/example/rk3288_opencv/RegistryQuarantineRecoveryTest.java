package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * 注册表「整份不可枚举」时的**非破坏性恢复**（{@link AndroidEnrollmentRepository#quarantineCorruptRegistry()}）
 * 单元测试。
 *
 * <h2>缺口背景（外部审查 [P2]「H：注册表全损坏时界面无恢复入口」）</h2>
 * 注册表的自愈只剩「一次成功的 {@code add}/{@code remove} 会重写注册表、顺带清除损坏行」这一条路；
 * 而经 UI 的 {@code add} 被 {@code EnrollmentSession} 的判重门切断，{@code remove} 又需要至少一条
 * 可删条目。当注册表**整份损坏 / 全部行不合法**时 {@code list().entries} 为空 ⇒ UI 里没有任何可删
 * 条目 ⇒ 用户无法自愈。本方法为该死角提供一条**不删除任何文件**的出路。
 *
 * <h2>本文件钉住的行为契约</h2>
 * <ol>
 *   <li><b>绝不删除</b>：原文件改名旁置为 {@code <原名>.corrupt.<ts>}，字节完整保留；</li>
 *   <li>无可解析副本时一并旁置 {@code .bak}/{@code .tmp}，避免 {@code persistRegistry} 阶段 1 把它们
 *       改名回主文件后被空表覆盖（那等于销毁它们）；若副本可解析则**先自愈**（见
 *       {@link #corruptMainWithUsableBackupSelfHealsInsteadOfQuarantiningBackup}），不再旁置；</li>
 *   <li><b>只做旁置 + 建空表</b>：不枚举、不重建、不伪造条目；</li>
 *   <li><b>前置条件不满足即拒绝</b>：注册表仍有可用条目 / 本就无损坏时返回非 OK，且**不改动任何文件**；</li>
 *   <li>与低版本「只读预览」语义自洽：API&lt;23 返回 {@link AndroidEnrollmentRepository.Status#UNSUPPORTED_PLATFORM}。</li>
 * </ol>
 *
 * <h2>覆盖边界（如实声明，勿高估）</h2>
 * <ul>
 *   <li><b>未覆盖</b>：旁置 / 新建空表**中途失败**的回滚分支（真实文件系统上难以稳定制造
 *       {@code renameTo} 失败 —— {@link FileStoreUtils#renameQuietly} 只吞 {@code SecurityException}，
 *       且旁置路径不经过可注入的 {@code FileOps}/{@code PlatformKeyProvider} 接缝）⇒ 该分支零覆盖；
 *       其中「**回滚本身失败**」（回滚 {@code renameTo} 亦失败，文案须如实列出仍被旁置的文件）这一支
 *       **无判别锚** —— 无法构造，按代码走查保证语义。</li>
 *   <li><b>未覆盖</b>：UI 接线（{@code MainActivity} 是 {@code AppCompatActivity}，纯 JVM 不可构造）；
 *       UI 侧由源码结构守卫单独钉住形态，行为由本类的仓库层用例钉住。</li>
 * </ul>
 */
public class RegistryQuarantineRecoveryTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int MV = 7;
    private static final int TSV = 2;

    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
        provider = jceProvider();
    }

    // ---------- ① 头损坏：旁置 + 建空表 ----------

    /**
     * 整份头损坏（{@code list() !isOk()}）时，恢复动作必须：旁置原文件为 {@code .corrupt.<ts>}、
     * 字节完整保留、随后得到一张可用的空注册表。
     *
     * <p><b>判别点</b>：把实现退化成「直接删除注册表」（而非改名旁置）⇒ 「旁置文件存在」「字节完整
     * 保留」两条红；只删不建空表 ⇒ 末条「list() 变为可用空表」红（且会重新落回「无注册表文件」= 非权威空库）。
     */
    @Test
    public void headerCorruptRegistryIsQuarantinedAsideAndResetToEmpty() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        File registry = repo.registryFile();
        byte[] corruptBytes = "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8);
        Files.write(registry.toPath(), corruptBytes);
        assertFalse("前置条件：头损坏时 list 必须返回非 OK", repo.list().isOk());

        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();

        assertTrue("整份损坏的注册表应可旁置恢复，实际=" + r.status + " / " + r.message, r.isOk());
        File aside = firstQuarantined(repo);
        assertNotNull("必须生成 <注册表名>.corrupt.<ts> 旁置文件", aside);
        assertArrayEquals("旁置文件必须逐字节保留原内容（绝不删除/改写）",
                corruptBytes, Files.readAllBytes(aside.toPath()));
        assertTrue("结果文案必须给出旁置文件的绝对路径，供用户事后手工取回，实际=" + r.message,
                r.message.contains(aside.getAbsolutePath()));

        AndroidEnrollmentRepository.ListResult after = repo.list();
        assertTrue("恢复后应存在一张可用的空注册表", after.isOk());
        assertTrue("恢复后注册表应为空", after.entries.isEmpty());
        assertTrue("恢复后判重门必须放行（否则仍无法注册新人员）", after.isEnumerationComplete());
        assertTrue("恢复后应真实存在注册表文件", repo.registryFile().exists());
    }

    // ---------- ② 全部行损坏：旁置 + 建空表 ----------

    /**
     * 「头部合法、全部行不合法」这一形态下 {@code list()} 仍返回 {@code OK}，但 {@code entries} 为空、
     * {@code skippedLineCount} 等于总行数（UI 正是按此形态判定死角）。恢复必须同样可用。
     *
     * <p><b>为什么本条不可与①合并</b>：① 走的是「{@code status != OK}」判据，本条走「{@code status == OK
     * 且 entries 空且 skipped &gt; 0}」判据 —— 二者是恢复入口的两个**不同**触发条件，缺一即留死角。
     */
    @Test
    public void allRowsCorruptRegistryIsQuarantinedAsideAndResetToEmpty() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        File registry = repo.registryFile();
        byte[] corruptBytes = ("FREG1\n"
                + "this-is-a-corrupt-row\n"
                + "also-not-a-valid-entry extra columns here\n").getBytes(StandardCharsets.UTF_8);
        Files.write(registry.toPath(), corruptBytes);

        AndroidEnrollmentRepository.ListResult before = repo.list();
        assertTrue("前置条件：合法头 + 全坏行时 status 仍为 OK", before.isOk());
        assertTrue("前置条件：entries 应为空", before.entries.isEmpty());
        assertEquals("前置条件：应报告全部数据行被跳过", 2, before.skippedLineCount);

        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();

        assertTrue("全坏行形态也应可旁置恢复，实际=" + r.status + " / " + r.message, r.isOk());
        File aside = firstQuarantined(repo);
        assertNotNull(aside);
        assertArrayEquals("旁置文件必须逐字节保留原内容", corruptBytes,
                Files.readAllBytes(aside.toPath()));

        AndroidEnrollmentRepository.ListResult after = repo.list();
        assertTrue(after.isOk());
        assertTrue(after.entries.isEmpty());
        assertTrue("恢复后判重门必须放行", after.isEnumerationComplete());
    }

    // ---------- ③ 前置条件：仍有可用条目 ⇒ 拒绝且不改动 ----------

    /**
     * 注册表仍有可解析条目时**不得**旁置：那会把仍可用的人从活性路径移走（用户会以为人被删了）。
     * 此时应如实拒绝，并保持注册表原封不动。
     */
    @Test
    public void registryWithUsableEntriesIsNotQuarantined() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 41), MV, TSV).status);
        byte[] before = Files.readAllBytes(repo.registryFile().toPath());

        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();

        assertFalse("仍有可用条目时不得旁置", r.isOk());
        assertNull("不得生成任何旁置文件", firstQuarantined(repo));
        assertArrayEquals("拒绝时注册表必须逐字节不变", before,
                Files.readAllBytes(repo.registryFile().toPath()));
        assertEquals("alice 必须仍在册", 1, repo.list().entries.size());
        assertEquals("alice", repo.list().entries.get(0).userId);
    }

    // ---------- ④ 前置条件：本就无注册表（全新安装） ⇒ 拒绝 ----------

    /** 全新安装（无注册表文件、无损坏行）不是「损坏」⇒ 不得生成空表之外的任何动作。 */
    @Test
    public void freshRepositoryWithNoRegistryIsNotQuarantined() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();

        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();

        assertFalse("无损坏时不得旁置", r.isOk());
        assertNull("不得生成旁置文件", firstQuarantined(repo));
        assertFalse("拒绝时不得凭空创建注册表文件", repo.registryFile().exists());
    }

    // ---------- ⑤ .bak / .tmp 一并旁置，杜绝「恢复后被空表覆盖」 ----------

    /**
     * 崩溃残留下 {@code .bak}/{@code .tmp} 与损坏主文件并存时，恢复必须把它们**一并旁置**，
     * 否则 {@link AndroidEnrollmentRepository} 的 {@code persistRegistry} 阶段 1
     * 「恢复基线」会在主文件缺失时把残留文件改名回主文件、随后被空表覆盖 —— 等于**销毁**它们。
     *
     * <p><b>判别点</b>：只旁置主文件（漏旁置 {@code .bak}/{@code .tmp}）⇒ 「{@code .bak.corrupt.} /
     * {@code .tmp.corrupt.} 旁置文件存在且字节完整」两条红。
     */
    @Test
    public void backupAndTmpArtifactsAreQuarantinedTooNotDestroyed() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        File registry = repo.registryFile();
        File bak = new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".bak");
        File tmpFile = new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".tmp");

        byte[] mainBytes = "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8);
        byte[] bakBytes = "FREG1\nBAK-LEGACY-BYTES\n".getBytes(StandardCharsets.UTF_8);
        byte[] tmpBytes = "FREG1\nTMP-LEGACY-BYTES\n".getBytes(StandardCharsets.UTF_8);
        Files.write(registry.toPath(), mainBytes);
        Files.write(bak.toPath(), bakBytes);
        Files.write(tmpFile.toPath(), tmpBytes);
        // 主文件头损坏 + 残留件同为「坏行」形态：自愈读回后仍是空条目集（status==OK 但 entries 空、
        // 有跳过行）⇒ 仍落入「无可删条目」死角，quarantine 入口照常可用。
        AndroidEnrollmentRepository.ListResult before = repo.list();
        assertTrue("前置条件：无可解析条目（残留件本身也无有效行）", before.entries.isEmpty());
        assertTrue("前置条件：应报告跳过的坏行", before.skippedLineCount > 0);

        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();
        assertTrue(r.isOk());

        File asideMain = firstQuarantined(repo);
        File asideBak = quarantinedNamed(repo, ".bak.corrupt.");
        File asideTmp = quarantinedNamed(repo, ".tmp.corrupt.");
        assertNotNull("主文件旁置文件应存在", asideMain);
        assertNotNull(".bak 也应被旁置（否则会被阶段 1 改名回主文件后被空表覆盖）", asideBak);
        assertNotNull(".tmp 也应被旁置", asideTmp);
        assertArrayEquals("主文件字节必须保留", mainBytes, Files.readAllBytes(asideMain.toPath()));
        assertArrayEquals(".bak 字节必须保留（不得被销毁）", bakBytes,
                Files.readAllBytes(asideBak.toPath()));
        assertArrayEquals(".tmp 字节必须保留（不得被销毁）", tmpBytes,
                Files.readAllBytes(asideTmp.toPath()));

        assertFalse("规范化 .bak 名不应再残留", bak.exists());
        assertFalse("规范化 .tmp 名不应再残留", tmpFile.exists());

        AndroidEnrollmentRepository.ListResult after = repo.list();
        assertTrue(after.isOk());
        assertTrue("恢复出来的应是空表（不得从残留 .bak/.tmp 复活出任何条目）", after.entries.isEmpty());
        assertTrue(after.isEnumerationComplete());
    }

    // ---------- ⑥ 低版本语义自洽：API<23 禁用写操作 ----------

    /** 与 {@code remove} 同为写操作，低版本必须一并禁用，且不得改动任何文件。 */
    @Test
    public void quarantineIsRejectedBelowApi23() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        File registry = repo.registryFile();
        byte[] corrupt = "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8);
        Files.write(registry.toPath(), corrupt);

        repo.setKeyProviderForTesting(unsupportedProvider());
        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();

        assertEquals(AndroidEnrollmentRepository.Status.UNSUPPORTED_PLATFORM, r.status);
        assertArrayEquals("低版本拒绝时不得改动注册表", corrupt,
                Files.readAllBytes(registry.toPath()));
        assertNull("低版本拒绝时不得生成旁置文件", firstQuarantined(repo));
    }

    // ---------- ⑦ 主文件头损坏 + 可用 .bak ⇒ 优先自愈，不得旁置健康副本 ----------

    /**
     * **自愈优先于旁置**：主文件头损坏（{@code CORRUPTED}）、但 {@code .bak} 完好可解析时，恢复路径
     * 必须先从 {@code .bak} **读回**条目（自愈），而不是走「旁置 + 建空表」把健康的 {@code .bak}
     * 一起清出活性路径。
     *
     * <p>缺陷现场（修复前）：{@code loadRegistry} 的备份恢复臂只对 {@code IO_ERROR} 开放，主文件
     * {@code CORRUPTED} 时快照直接返回 {@code CORRUPTED} ⇒ {@code quarantineCorruptRegistry} 判定
     * 「无可用条目」⇒ 把健康的 {@code .bak} 也一并旁置 + 建空表。数据不丢（是改名），但**活性库被
     * 清空**，而 {@code .bak} 本可用来自愈。
     *
     * <p><b>判别力</b>：去掉 {@code loadRegistry} 里「主文件不可解析时优先自愈」那段（恢复为只对
     * {@code IO_ERROR} 回退）⇒ 首次 {@code list()} 返回 {@code CORRUPTED}（首三条断言红），且随后
     * {@code quarantineCorruptRegistry()} 会把 {@code .bak} 一并旁置（末两条断言红）。
     */
    @Test
    public void corruptMainWithUsableBackupSelfHealsInsteadOfQuarantiningBackup() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 91), MV, TSV).status);
        byte[] goodRegistry = Files.readAllBytes(repo.registryFile().toPath());

        // 主文件头损坏（外部破坏的形态）；.bak 完好且含 alice。
        Files.write(repo.registryFile().toPath(),
                "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));
        File bak = new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".bak");
        Files.write(bak.toPath(), goodRegistry);

        // 触发恢复：应自愈为可用注册表（条目取自 .bak），而不是报 CORRUPTED。
        AndroidEnrollmentRepository.ListResult recovered = repo.list();
        assertTrue("头损坏 + 可用 .bak 应自愈为可用注册表，实际=" + recovered.status, recovered.isOk());
        assertEquals("应自愈恢复出 alice", 1, recovered.entries.size());
        assertEquals("alice", recovered.entries.get(0).userId);
        assertFalse("自愈恢复的条目集不得宣称权威（备份可能早于最近新增）", recovered.authoritative);

        // 库已自愈可用：旁置入口必须拒绝，且不得产生任何 .corrupt.* 旁置文件。
        AndroidEnrollmentRepository.Result r = repo.quarantineCorruptRegistry();
        assertFalse("库已自愈可用，旁置入口必须拒绝（否则会清空活性库）", r.isOk());
        assertEquals("不得产生任何 .corrupt.* 旁置文件（健康的 .bak 未被旁置）", 0,
                quarantinedFiles(repo).size());
        assertTrue(".bak 必须原样留在盘上（主文件存在时只读自愈，不消费副本）", bak.exists());
    }

    // ---------- ⑧ 旁置目标名冲突：取唯一名，避免 POSIX rename(2) 静默覆盖既有证据 ----------

    /**
     * {@code uniqueAsideFile} 必须返回一个**当前不存在**的旁置目标名：若 {@code <原名>.corrupt.<ts>}
     * 已被占用，则依次追加 {@code -<序号>}。POSIX/Android 的 {@code rename(2)} 会静默覆盖同名目标，
     * 不取唯一名会销毁此前的旁置证据（Windows 上 {@code renameTo} 到已存在目标会失败，属失败安全）。
     *
     * <p><b>判别力</b>：去掉「同名则追加序号」的那段循环（直接返回首个候选）⇒ 返回名与被占用名
     * 相同 ⇒ 前两条断言红。
     */
    @Test
    public void uniqueAsideFileAvoidsOverwritingExistingEvidence() throws Exception {
        File dir = tmp.newFolder();
        long ts = 1_700_000_000_000L;
        String base = AndroidEnrollmentRepository.REGISTRY_FILE_NAME;
        File occupied = new File(dir, base + ".corrupt." + ts);
        assertTrue("前置：占用首个候选名", occupied.createNewFile());

        File picked = AndroidEnrollmentRepository.uniqueAsideFile(dir, base, ts);
        assertNotEquals("同名旁置文件已存在时必须换名，避免 POSIX rename(2) 静默覆盖既有证据",
                occupied.getName(), picked.getName());
        assertFalse("选出的名字必须尚不存在", picked.exists());

        // 再占用 -1，应退到 -2。
        assertTrue(new File(dir, base + ".corrupt." + ts + "-1").createNewFile());
        File picked2 = AndroidEnrollmentRepository.uniqueAsideFile(dir, base, ts);
        assertEquals(base + ".corrupt." + ts + "-2", picked2.getName());
    }

    // ---------- 工具 ----------

    /** 枚举 scopeDir 下所有 {@code <注册表名>*.corrupt.*} 旁置文件（主文件与 .bak/.tmp 的皆含）。 */
    private static List<File> quarantinedFiles(AndroidEnrollmentRepository repo) {
        List<File> out = new ArrayList<>();
        File[] fs = repo.scopeDir().listFiles();
        if (fs == null) {
            return out;
        }
        for (File f : fs) {
            if (f.getName().startsWith(AndroidEnrollmentRepository.REGISTRY_FILE_NAME)
                    && f.getName().contains(".corrupt.")) {
                out.add(f);
            }
        }
        return out;
    }

    /** 主文件的旁置文件（名字严格为 {@code <注册表名>.corrupt.<ts>}，不含 .bak/.tmp）。 */
    private static File firstQuarantined(AndroidEnrollmentRepository repo) {
        String prefix = AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".corrupt.";
        for (File f : quarantinedFiles(repo)) {
            if (f.getName().startsWith(prefix)
                    && !f.getName().contains(".bak.corrupt.")
                    && !f.getName().contains(".tmp.corrupt.")) {
                return f;
            }
        }
        return null;
    }

    /** 按名字片段取某个旁置文件（如 {@code .bak.corrupt.}）。 */
    private static File quarantinedNamed(AndroidEnrollmentRepository repo, String nameFragment) {
        for (File f : quarantinedFiles(repo)) {
            if (f.getName().contains(nameFragment)) {
                return f;
            }
        }
        return null;
    }

    private AndroidEnrollmentRepository newRepo() throws Exception {
        AndroidEnrollmentRepository r = new AndroidEnrollmentRepository(null, tmp.newFolder());
        r.setKeyProviderForTesting(provider);
        return r;
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

    private static FeatureTemplateEncryptedStore.PlatformKeyProvider unsupportedProvider() {
        return new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            @Override
            public boolean isSupported() {
                return false;
            }

            @Override
            public SecretKey getOrCreateKeyV1() {
                throw new IllegalStateException("unreachable: unsupported platform");
            }
        };
    }
}
