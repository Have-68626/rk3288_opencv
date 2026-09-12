package com.example.rk3288_opencv;

import org.junit.After;
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
 * <p>通过 {@link FeatureTemplateEncryptedStore#setPlatformKeyProviderForTesting} 注入 JCE 内存密钥，
 * 使 AndroidKeyStore + AES-GCM 的真实加解密能在纯 JVM（Gradle unit test）上运行，从而覆盖：
 * 新增、覆盖拒绝、读取、删除、损坏文件、密钥不可用、API&lt;23 只读预览。
 */
public class AndroidEnrollmentRepositoryTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int MV = 7;
    private static final int TSV = 2;

    @Before
    public void setUp() throws Exception {
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(jceProvider());
    }

    @After
    public void tearDown() {
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(null);
    }

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

        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(brokenProvider());
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
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(unsupportedProvider());

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

    // ---------- ⑩ 同目录跨实例共享锁（#6） ----------

    @Test
    public void sameScopeDirInstancesShareStaticLock() throws Exception {
        File dir = tmp.newFolder();
        assertSame("同一规范化路径必须映射到同一把锁",
                AndroidEnrollmentRepository.dirLockForTesting(dir),
                AndroidEnrollmentRepository.dirLockForTesting(new File(dir.getAbsolutePath())));

        AndroidEnrollmentRepository r1 = new AndroidEnrollmentRepository(null, dir);
        AndroidEnrollmentRepository r2 = new AndroidEnrollmentRepository(null, dir);
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
            repos[i] = new AndroidEnrollmentRepository(null, dir);
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

    // ---------- 工具 ----------

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
        return new AndroidEnrollmentRepository(null, tmp.newFolder());
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
