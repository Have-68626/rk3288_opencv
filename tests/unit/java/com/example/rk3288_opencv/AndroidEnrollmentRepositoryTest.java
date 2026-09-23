package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

/**
 * AndroidEnrollmentRepository（任务 16）单元测试。
 *
 * <p>通过 {@link AndroidEnrollmentRepository#setKeyProviderForTesting} 在**本用例创建的实例**上注入
 * JCE 内存密钥替身，使 AndroidKeyStore + AES-GCM 的真实加解密能在纯 JVM（Gradle unit test）上运行，
 * 从而覆盖：新增、覆盖拒绝、读取、删除、损坏文件、密钥不可用、API&lt;23 只读预览。
 */
public class AndroidEnrollmentRepositoryTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int MV = 7;
    private static final int TSV = 2;

    /**
     * 供各用例注入到**本用例创建的 repo 实例**上的密钥提供者替身（内存 JCE 密钥）。此前是进程级静态
     * 注入，改造后由各实例的 {@code setKeyProviderForTesting} 注入（见 {@link #newRepo(File)}）。
     */
    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
        provider = jceProvider();
    }

    // 无 @After（改造前它用于复位进程级静态 provider）：密钥提供者接缝已是**实例级**，各实例由
    // newRepo(File) 就地注入，本测试类不再改写任何静态状态 ⇒ 无静态状态可复位。删除接缝的实例级
    // 注入（setFileOpsForTesting）同理只需逐个用例自洽，无需跨类全局复位 —— 跨实例泄漏结构上不可能。

    // ---------- ① 新增 + 读取（并验证使用后清零） ----------

    @Test
    public void addListGetAndReadBack() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        byte[] template = templateBytes(64, 11);

        AndroidEnrollmentRepository.Result add = repo.add("alice", template, MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.OK, add.status);
        assertNotNull(add.entry);
        assertEquals("alice", add.entry.userId);
        assertTrue("应生成加密模板文件", repo.templateFileFor("alice").exists());
        assertTrue("应生成注册表文件", repo.registryFile().exists());

        // 注册表可枚举
        List<AndroidEnrollmentRepository.Entry> entries = repo.list().entries;
        assertEquals(1, entries.size());
        assertEquals("alice", entries.get(0).userId);
        assertEquals(MV, entries.get(0).modelVersion);
        assertEquals(TSV, entries.get(0).templateSchemaVersion);
        assertEquals(64, entries.get(0).ciphertextSha256Hex.length());

        // get 命中
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);
        assertEquals(AndroidEnrollmentRepository.Status.NOT_FOUND, repo.get("bob").status);

        // 读取：回调内明文等于写入值，回调外明文被清零
        final byte[][] seen = new byte[1][];
        AndroidEnrollmentRepository.Result read = repo.readTemplate("alice", new AndroidEnrollmentRepository.TemplateConsumer() {
            @Override
            public void accept(byte[] plaintextTemplate) {
                seen[0] = plaintextTemplate;
                assertArrayEquals("回调内应拿到原始模板", template, plaintextTemplate);
            }
        });
        assertEquals(AndroidEnrollmentRepository.Status.OK, read.status);
        assertNotNull(seen[0]);
        for (byte b : seen[0]) {
            assertEquals("读取返回后内存明文必须被清零", 0, b);
        }
    }

    // ---------- ② 覆盖拒绝 ----------

    @Test
    public void duplicateAddIsRejectedWithoutOverwrite() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        byte[] first = templateBytes(32, 1);
        byte[] second = templateBytes(32, 2);

        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", first, MV, TSV).status);
        AndroidEnrollmentRepository.Result dup = repo.add("alice", second, MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.DUPLICATE_USER, dup.status);
        assertEquals("拒绝覆盖后仍应只有一条", 1, repo.list().entries.size());

        final byte[][] seen = new byte[1][];
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.readTemplate("alice", new AndroidEnrollmentRepository.TemplateConsumer() {
            @Override
            public void accept(byte[] plaintextTemplate) {
                seen[0] = plaintextTemplate.clone();
            }
        }).status);
        assertArrayEquals("原模板不得被第二次写入覆盖", first, seen[0]);
    }

    // ---------- ③ 删除 ----------

    @Test
    public void removeDeletesTemplateAndRegistryEntry() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", templateBytes(16, 3), MV, TSV).status);

        AndroidEnrollmentRepository.Result rm = repo.remove("alice");
        assertEquals(AndroidEnrollmentRepository.Status.OK, rm.status);
        assertFalse("模板文件应被删除", repo.templateFileFor("alice").exists());
        assertTrue(repo.list().entries.isEmpty());
        assertEquals(AndroidEnrollmentRepository.Status.NOT_FOUND, repo.get("alice").status);
        assertEquals(AndroidEnrollmentRepository.Status.NOT_FOUND, repo.remove("alice").status);
    }

    // ---------- ④ 损坏文件 ----------

    @Test
    public void corruptTemplateReportsCorrupted() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", templateBytes(48, 4), MV, TSV).status);

        // 翻转密文最后一个字节（GCM tag 区域）制造完整性损坏
        File tpl = repo.templateFileFor("alice");
        try (RandomAccessFile raf = new RandomAccessFile(tpl, "rw")) {
            long last = raf.length() - 1;
            raf.seek(last);
            int b = raf.read();
            raf.seek(last);
            raf.write(b ^ 0xFF);
        }

        AndroidEnrollmentRepository.Result read = repo.readTemplate("alice", new AndroidEnrollmentRepository.TemplateConsumer() {
            @Override
            public void accept(byte[] plaintextTemplate) {
            }
        });
        assertEquals(AndroidEnrollmentRepository.Status.CORRUPTED, read.status);
    }

    @Test
    public void corruptRegistryReportsCorrupted() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", templateBytes(16, 5), MV, TSV).status);

        // 覆盖为非法注册表头
        try (FileOutputStream fos = new FileOutputStream(repo.registryFile(), false)) {
            fos.write("NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));
        }

        assertEquals(AndroidEnrollmentRepository.Status.CORRUPTED, repo.list().status);
        assertEquals(AndroidEnrollmentRepository.Status.CORRUPTED, repo.add("bob", templateBytes(16, 6), MV, TSV).status);
    }

    // ---------- ⑤ 密钥不可用 ----------

    @Test
    public void keyUnavailableReportsKeystoreError() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", templateBytes(16, 7), MV, TSV).status);

        repo.setKeyProviderForTesting(brokenProvider());
        assertEquals(AndroidEnrollmentRepository.Status.KEYSTORE_ERROR,
                repo.add("bob", templateBytes(16, 8), MV, TSV).status);
        assertEquals(AndroidEnrollmentRepository.Status.KEYSTORE_ERROR,
                repo.readTemplate("alice", new AndroidEnrollmentRepository.TemplateConsumer() {
                    @Override
                    public void accept(byte[] plaintextTemplate) {
                    }
                }).status);
    }

    // ---------- ⑥ API < 23：只读预览 + 注册禁用 ----------

    @Test
    public void belowApi23AllowsPreviewButDisablesEnrollment() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.add("alice", templateBytes(24, 9), MV, TSV).status);

        // 模拟 API < 23
        repo.setKeyProviderForTesting(unsupportedProvider());

        assertFalse(repo.isEnrollmentEnabled());
        assertTrue(repo.isReadOnlyPreview());
        assertNotNull(repo.unsupportedReason());

        // read 放行：注册表预览仍可用
        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals(1, list.entries.size());
        assertEquals("alice", list.entries.get(0).userId);
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);

        // 注册入口禁用并解释
        AndroidEnrollmentRepository.Result add = repo.add("bob", templateBytes(24, 10), MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.UNSUPPORTED_PLATFORM, add.status);
        assertFalse(add.message.isEmpty());

        // 删除同属写操作，低版本一并禁用，保持“只读”语义自洽（#9）。
        AndroidEnrollmentRepository.Result rm = repo.remove("alice");
        assertEquals(AndroidEnrollmentRepository.Status.UNSUPPORTED_PLATFORM, rm.status);
        assertTrue("低版本不得删除模板", repo.templateFileFor("alice").exists());
        assertEquals("删除被拒后条目仍在", 1, repo.list().entries.size());

        // 低版本无法解密模板
        assertEquals(AndroidEnrollmentRepository.Status.UNSUPPORTED_PLATFORM,
                repo.readTemplate("alice", new AndroidEnrollmentRepository.TemplateConsumer() {
                    @Override
                    public void accept(byte[] plaintextTemplate) {
                    }
                }).status);
    }

    // ---------- ⑦ 入参校验 ----------

    @Test
    public void invalidArgumentsRejected() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.INVALID_ARGUMENT,
                repo.add("", templateBytes(8, 1), MV, TSV).status);
        assertEquals(AndroidEnrollmentRepository.Status.INVALID_ARGUMENT,
                repo.add("alice", new byte[0], MV, TSV).status);
        assertTrue(repo.list().entries.isEmpty());
    }

    // ---------- ⑧ 删除原子性（#1） ----------

    @Test
    public void removeKeepsTemplateWhenRegistryWriteFails() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 21), MV, TSV).status);

        // 用非空目录堵住注册表临时文件路径，强制 persistRegistry 失败。
        File tmpPath = new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".tmp");
        assertTrue(tmpPath.mkdirs());
        assertTrue(new File(tmpPath, "block").createNewFile());

        AndroidEnrollmentRepository.Result rm = repo.remove("alice");
        assertFalse("注册表写入失败应返回错误", rm.isOk());
        assertTrue("注册表写失败不得先删模板（否则留下悬空条目）",
                repo.templateFileFor("alice").exists());
        assertEquals("条目应保持原样", 1, repo.list().entries.size());
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);

        // 清理阻塞目录，避免影响用例自身的临时目录回收。
        new File(tmpPath, "block").delete();
        tmpPath.delete();
    }

    @Test
    public void removeRollsBackRegistryWhenTemplateRemovalFails() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 22), MV, TSV).status);

        // 把模板路径替换成非空目录，使 File.delete() 失败，触发注册表回滚。
        File tpl = repo.templateFileFor("alice");
        assertTrue(tpl.delete());
        assertTrue(tpl.mkdirs());
        assertTrue(new File(tpl, "blocker").createNewFile());

        AndroidEnrollmentRepository.Result rm = repo.remove("alice");
        assertEquals(AndroidEnrollmentRepository.Status.IO_ERROR, rm.status);
        assertEquals("模板删除失败应回滚注册表条目", 1, repo.list().entries.size());
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);

        new File(tpl, "blocker").delete();
        tpl.delete();
    }

    /**
     * §2.5-D 补的**异常**路径：注入一个 {@code delete} 抛 {@code SecurityException} 的接缝，
     * 覆盖 {@code AndroidEnrollmentRepository.remove} 的 {@code catch (SecurityException)}（子代理 4 P1）。
     *
     * <p>既有用例 {@code removeRollsBackRegistryWhenTemplateRemovalFails} 只用「非空目录占位」
     * 覆盖了「{@code delete} 返回 false」那条，**未覆盖抛异常**那条 —— 而接缝让后者零成本可构造。
     * 若有人删掉那个 catch，删除会在异常时直接抛出、绕过回滚，注册表与模板不一致且**无测试拦截**。
     */
    @Test
    public void removeRollsBackRegistryWhenDeleteThrowsSecurityException() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 22), MV, TSV).status);

        repo.setFileOpsForTesting(new FileStoreUtils.FileOps() {
            @Override
            public boolean delete(File f) {
                throw new SecurityException("probe: delete denied");
            }

            @Override
            public boolean exists(File f) {
                return f.exists();
            }
        });
        AndroidEnrollmentRepository.Result rm;
        try {
            rm = repo.remove("alice");  // 不得把 SecurityException 抛出去
        } finally {
            repo.setFileOpsForTesting(null);
        }

        assertEquals(AndroidEnrollmentRepository.Status.IO_ERROR, rm.status);
        assertEquals("抛异常应回滚注册表条目", 1, repo.list().entries.size());
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);
    }

    // ---------- ⑨ 单行损坏跳过/隔离（#7） ----------

    @Test
    public void corruptRowIsSkippedAndValidEntriesRetained() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 31), MV, TSV).status);

        // 合法头部与条目之后追加一行损坏数据。
        try (FileOutputStream fos = new FileOutputStream(repo.registryFile(), true)) {
            fos.write("this-is-a-corrupt-row\n".getBytes(StandardCharsets.UTF_8));
        }

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("坏行不应使整表 CORRUPTED", AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals(1, list.entries.size());
        assertEquals("alice", list.entries.get(0).userId);
        assertEquals("应报告跳过的损坏行数", 1, list.skippedLineCount);
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);

        // 一次成功写入会自愈：损坏行被清除，且不影响后续新增。
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("bob", templateBytes(16, 32), MV, TSV).status);
        AndroidEnrollmentRepository.ListResult healed = repo.list();
        assertEquals(2, healed.entries.size());
        assertEquals(0, healed.skippedLineCount);
    }

    // ---------- ⑨b 条目集权威性（A2 审计 H2：破坏性对账的门禁） ----------

    /**
     * 损坏行被跳过时 {@code authoritative} 必须为 false。
     *
     * <p>它门禁的是**破坏性对账**（显示名孤儿清理）：若用非权威集合做差集删除，「仍在册但本次
     * 没读到」的人会被当作已注销，其派生数据被误删。
     */
    @Test
    public void authoritativeIsFalseWhenARowIsCorrupt() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 31), MV, TSV).status);
        try (FileOutputStream fos = new FileOutputStream(repo.registryFile(), true)) {
            fos.write("this-is-a-corrupt-row\n".getBytes(StandardCharsets.UTF_8));
        }

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(1, list.skippedLineCount);
        assertFalse("有损坏行 ⇒ 条目集不权威（不得据此删除派生数据）", list.authoritative);

        // 一次成功写入自愈后，重新变为权威。
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("bob", templateBytes(16, 32), MV, TSV).status);
        assertTrue("自愈后应恢复权威", repo.list().authoritative);
    }

    /** 无注册表文件 ⇒ 非权威（无法区分「全新安装」与「索引丢失」）。 */
    @Test
    public void authoritativeIsFalseWhenNoRegistryFileExists() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, list.status);
        assertFalse("无注册表不得视为权威", list.authoritative);
    }

    /** 从 {@code .bak} 恢复 ⇒ 非权威（备份可能早于最近新增）。 */
    @Test
    public void authoritativeIsFalseWhenRecoveredFromBackup() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 31), MV, TSV).status);

        // 造出「主文件缺失 + 备份存在」的崩溃现场：dst → bak。
        File dst = repo.registryFile();
        File bak = new File(dst.getParentFile(), dst.getName() + ".bak");
        assertTrue("前置：应能移开主文件", dst.renameTo(bak));

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals("应从备份取回 alice", 1, list.entries.size());
        assertFalse("从备份恢复的条目集不得视为权威", list.authoritative);
    }

    // ---------- ⑩ 同目录跨实例共享锁（#6） ----------

    @Test
    public void sameScopeDirInstancesShareStaticLock() throws Exception {
        File dir = tmp.newFolder();
        assertSame("同一规范化路径必须映射到同一把锁",
                AndroidEnrollmentRepository.dirLockForTesting(dir),
                AndroidEnrollmentRepository.dirLockForTesting(new File(dir.getAbsolutePath())));

        AndroidEnrollmentRepository r1 = newRepo(dir);
        AndroidEnrollmentRepository r2 = newRepo(dir);
        assertSame("同目录的不同仓库实例必须共享锁",
                AndroidEnrollmentRepository.dirLockForTesting(r1.scopeDir()),
                AndroidEnrollmentRepository.dirLockForTesting(r2.scopeDir()));
    }

    @Test
    public void concurrentAddsAcrossInstancesDoNotLoseUpdates() throws Exception {
        final File dir = tmp.newFolder();
        final int n = 8;
        final AndroidEnrollmentRepository[] repos = new AndroidEnrollmentRepository[n];
        for (int i = 0; i < n; i++) {
            repos[i] = newRepo(dir);
        }
        final CountDownLatch start = new CountDownLatch(1);
        final AtomicInteger ok = new AtomicInteger();
        Thread[] threads = new Thread[n];
        for (int i = 0; i < n; i++) {
            final int idx = i;
            threads[i] = new Thread(new Runnable() {
                @Override
                public void run() {
                    try {
                        start.await();
                        if (repos[idx].add("user" + idx, templateBytes(8, idx), MV, TSV).isOk()) {
                            ok.incrementAndGet();
                        }
                    } catch (InterruptedException ignored) {
                        Thread.currentThread().interrupt();
                    }
                }
            });
            threads[i].start();
        }
        start.countDown();
        for (Thread t : threads) {
            t.join();
        }
        assertEquals("并发新增应全部成功", n, ok.get());
        assertEquals("同目录跨实例并发不得丢更新", n, repos[0].list().entries.size());
    }

    // ---------- ⑪ 注册表 userId 可逆编码（#5 文档化取舍） ----------

    @Test
    public void registryStoresReversibleHexUserId() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 51), MV, TSV).status);

        String content = new String(Files.readAllBytes(repo.registryFile().toPath()), StandardCharsets.UTF_8);
        // "alice" 的 UTF-8 hex 为 616c696365；该断言钉住“可逆编码而非哈希”的取舍。
        assertTrue("注册表以可逆 hex 保存 userId", content.contains("616c696365"));
    }

    // ---------- ⑫ 注册表崩溃恢复（P1-3） ----------

    @Test
    public void loadRecoversFromBackupWhenMainMissing() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 61), MV, TSV).status);

        File dst = repo.registryFile();
        File bak = bakFor(repo);
        // 模拟「dst→bak 完成、tmp→dst 未完成」时进程退出：主文件缺失，仅剩 .bak。
        assertTrue(dst.renameTo(bak));
        assertFalse(dst.exists());
        assertTrue(bak.exists());

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("主文件缺失不得伪装成空库", AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals("应从 .bak 恢复既有条目", 1, list.entries.size());
        assertEquals("alice", list.entries.get(0).userId);
        assertTrue("恢复成功后主文件应重新存在", dst.exists());
    }

    @Test
    public void staleTmpResidueIsIgnoredWhenMainValid() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 62), MV, TSV).status);

        // 主文件完好，同时残留一个“半成品” tmp（内容为 bob）；loadRegistry 必须只认主文件。
        File tmpFile = new File(repo.scopeDir(),
                AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".tmp");
        String stale = "FREG1\n" + "626f62 7 2 10 " + repeat('a', 64) + " 1\n";
        Files.write(tmpFile.toPath(), stale.getBytes(StandardCharsets.UTF_8));

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("残留 tmp 不应使读取失败", AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals("残留 tmp 不得干扰主文件条目", 1, list.entries.size());
        assertEquals("应读取主文件中的 alice", "alice", list.entries.get(0).userId);
    }

    @Test
    public void mainRegistryWinsWhenMainAndBackupCoexist() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 63), MV, TSV).status);
        byte[] aliceOnly = Files.readAllBytes(repo.registryFile().toPath());
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("bob", templateBytes(16, 64), MV, TSV).status);

        // 构造「新主文件已就位、旧备份尚未清理」：bak 为仅含 alice 的旧快照。
        File bak = bakFor(repo);
        Files.write(bak.toPath(), aliceOnly);

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals("主文件与备份并存时应以主文件为准", 2, list.entries.size());
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("bob").status);
    }

    @Test
    public void writeAfterRecoveryDoesNotLoseEntries() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 65), MV, TSV).status);

        // 制造崩溃残留：dst 缺失 + bak 存在（bob 的写入若先删 bak 会永久丢掉 alice）。
        File dst = repo.registryFile();
        File bak = bakFor(repo);
        assertTrue(dst.renameTo(bak));

        assertEquals("恢复后新增应成功", AndroidEnrollmentRepository.Status.OK,
                repo.add("bob", templateBytes(16, 66), MV, TSV).status);

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals("恢复后写入不得丢失既有人员", 2, list.entries.size());
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("alice").status);
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.get("bob").status);
        assertFalse("成功写入后不应残留备份", bak.exists());
    }

    @Test
    public void unreadableMainRecoversFromBackup() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 67), MV, TSV).status);
        byte[] aliceRegistry = Files.readAllBytes(repo.registryFile().toPath());

        // 主文件存在但不可读：用空目录占位使 FileInputStream 抛 IOException；备份可解析。
        File dst = repo.registryFile();
        assertTrue(dst.delete());
        assertTrue(dst.mkdirs());
        File bak = bakFor(repo);
        Files.write(bak.toPath(), aliceRegistry);

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("主文件不可读时应回退备份", AndroidEnrollmentRepository.Status.OK, list.status);
        assertEquals(1, list.entries.size());
        assertEquals("alice", list.entries.get(0).userId);
    }

    // ---------- ⑬ 实例级注入不跨实例泄漏（子代理 3 N2 补的结构性守卫，与存储侧对偶） ----------

    /**
     * 删除接缝的实例级注入**不跨实例泄漏**：两个仓库实例各注入不同替身、互不影响。
     *
     * <p><b>为什么这条守卫必要</b>（子代理 3 N2）：{@code FileStoreUtils} 的 {@code FileOps} 接缝
     * 已由「进程级静态」改为「实例字段 + 显式传参」，而 {@code :app:testDebugUnitTest} 的全部测试类
     * 在**同一 JVM 内串行**执行 —— 若接缝退回静态，任一类注入后忘记复位就会污染其后所有走
     * {@code deleteWithBackup} 的用例。存储侧
     * {@code PersonDisplayNameStoreTest.injectedOpsArePerInstanceAndDoNotLeak} 已钉住该不变量，
     * 但 {@code AndroidEnrollmentRepository} 此前**既无结构性守卫、也无行为性守卫**
     * （{@code fileStoreUtilsExposesNoStaticInjectionSeam} 只反射 {@code FileStoreUtils.class}，不覆盖本类）
     * ⇒ 本类「实例级、注入只影响本实例」的承诺完全无守护。本用例补齐该对偶。
     *
     * <p><b>打红方式</b>（改任一处即红）：
     * <ul>
     *   <li>把 {@code AndroidEnrollmentRepository.fileOps} 退回 {@code static}（或让
     *       {@code deleteWithBackup} 读任何静态 {@code fileOps}）—— 则对 {@code b} 的注入会覆盖
     *       对 {@code a} 的注入，{@code a.remove} 将改用 {@code opsB} ⇒「实例 a 的替身应被触达」
     *       与「实例 b 的替身不得被 a 触达」两断言同时红；</li>
     *   <li>让 {@code remove} 不再传本实例的 {@code fileOps}（例如误传
     *       {@code FileStoreUtils.DEFAULT_FILE_OPS}）⇒「实例 a 的替身应被触达」红。</li>
     * </ul>
     */
    @Test
    public void injectedOpsArePerInstanceAndDoNotLeak() throws Exception {
        File dirA = tmp.newFolder("a");
        File dirB = tmp.newFolder("b");
        AndroidEnrollmentRepository a = newRepo(dirA);
        AndroidEnrollmentRepository b = newRepo(dirB);
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                a.add("alice", templateBytes(16, 71), MV, TSV).status);
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                b.add("bob", templateBytes(16, 72), MV, TSV).status);

        String nameA = a.templateFileFor("alice").getName();
        String nameB = b.templateFileFor("bob").getName();
        assertFalse("前置：两个实例的文件名应不同（否则名字断言无区分度）", nameA.equals(nameB));

        UndeletableOps opsA = new UndeletableOps();
        UndeletableOps opsB = new UndeletableOps();
        a.setFileOpsForTesting(opsA);
        b.setFileOpsForTesting(opsB);

        a.remove("alice");   // 失败替身 ⇒ a 的模板删不掉（并触发注册表回滚）

        assertTrue("实例 a 的替身应被触达", opsA.attempted.contains(nameA));
        assertTrue("实例 a 的替身应触达 .bak", opsA.attempted.contains(nameA + ".bak"));
        assertTrue("实例 b 的替身不得被 a 的操作触达（否则存在跨实例泄漏）",
                opsB.attempted.isEmpty());
        assertTrue("a 的删除失败 ⇒ a 的模板仍在（不得被谎报为已删）",
                a.templateFileFor("alice").exists());
        assertTrue("b 的模板不得受 a 的操作影响", b.templateFileFor("bob").exists());

        int aCallsAfterRemove = opsA.attempted.size();
        b.remove("bob");
        assertTrue("实例 b 的替身应被触达", opsB.attempted.contains(nameB));
        assertTrue("实例 b 的替身应触达 .bak", opsB.attempted.contains(nameB + ".bak"));
        assertEquals("a 的替身调用数不得因 b 的操作而变化", aCallsAfterRemove, opsA.attempted.size());
    }

    // ---------- 工具 ----------

    /**
     * 记录型删除替身：「文件删不掉」（{@code delete} 恒返回 false）但**记录被尝试删除的文件名**。
     *
     * <p>与 {@code PersonDisplayNameStoreTest.UndeletableOps} 同构：只断言「调用次数 &gt; 0」挡不住
     * 「实现里漏删主文件、只删 .bak」这类 bug（那种情况下仍有一次调用）；记录名字后可逐项断言
     * 主文件与其 {@code .bak} 都被尝试过。{@code exists} **反映真实文件状态**（不恒 true）——
     * 恒 true 只是「恰好」等于当前实现末尾对主文件查一次 exists 这一实现细节，属白盒耦合。
     */
    private static final class UndeletableOps implements FileStoreUtils.FileOps {
        final List<String> attempted = new java.util.ArrayList<>();

        @Override
        public boolean delete(File f) {
            attempted.add(f.getName());
            return false;
        }

        @Override
        public boolean exists(File f) {
            return f.exists();
        }
    }

    /** 注册表备份文件路径；与实现约定的 {@code <主文件名>.bak} 保持一致。 */
    private static File bakFor(AndroidEnrollmentRepository repo) {
        return new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".bak");
    }

    private static String repeat(char c, int count) {
        char[] out = new char[count];
        java.util.Arrays.fill(out, c);
        return new String(out);
    }

    private AndroidEnrollmentRepository newRepo() throws Exception {
        return newRepo(tmp.newFolder());
    }

    /** 构造并在**本实例**上注入测试用密钥提供者替身（避免 6 处构造点各自重复注入）。 */
    private AndroidEnrollmentRepository newRepo(File dir) {
        AndroidEnrollmentRepository r = new AndroidEnrollmentRepository(null, dir);
        r.setKeyProviderForTesting(provider);
        return r;
    }

    private static byte[] templateBytes(int len, int seed) {
        byte[] out = new byte[len];
        new SecureRandom(new byte[]{(byte) seed}).nextBytes(out);
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

    private static FeatureTemplateEncryptedStore.PlatformKeyProvider brokenProvider() {
        return new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            @Override
            public boolean isSupported() {
                return true;
            }

            @Override
            public SecretKey getOrCreateKeyV1() {
                throw new IllegalStateException("keystore unavailable (test)");
            }
        };
    }
}
