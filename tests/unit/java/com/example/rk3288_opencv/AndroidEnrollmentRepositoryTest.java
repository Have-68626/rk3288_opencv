package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.FileOutputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
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
 * 从而覆盖：新增、覆盖拒绝、读取、删除、损坏文件、密钥不可用、API&lt;23 只读预览；以及
 * 注册表字段语义（整文件长度/摘要）、删除与新增的原子性与回滚、单行损坏跳过与条目集权威性、
 * 枚举完整性判据（`isEnumerationComplete`，判重门的在册依据）、
 * 崩溃恢复（`.bak` 回退的**触发范围**与不触发情形）、同目录跨实例共享锁、接缝的实例级不泄漏。
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

    // ---------- ①b 注册表存的是模板**文件**（envelope 全量）的长度与摘要（FACE-PIPE-010③ 新8） ----------

    /**
     * 判别性断言：`ciphertextSize` / `ciphertextSha256Hex` 虽名为「密文」，实际算的是**整个加密
     * 模板文件** —— 自行回读落盘字节、自行算长度与摘要，再与字段比对，可区分「算整个 envelope」
     * 与「只算 ct+tag 段」两种实现（后者会少 64B 头）。
     *
     * <p>并同时按**列位**断言落盘值：注册表是位置式六列文本、无列名，改错列只会静默丢元数据。
     *
     * <p><b>打红方式</b>：把 `add` 里的 {@code raw} 换成只取密文段（跳过 envelope 头）再算长度/摘要
     * ⇒ 长度断言、摘要断言与第 4/5 列断言同时红。
     */
    @Test
    public void registryRecordsWholeTemplateFileSizeAndDigest() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        byte[] template = templateBytes(64, 81);
        AndroidEnrollmentRepository.Result add = repo.add("alice", template, MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.OK, add.status);
        assertNotNull(add.entry);

        File tpl = repo.templateFileFor("alice");
        byte[] onDisk = Files.readAllBytes(tpl.toPath());
        assertTrue("前置：envelope 含头部 ⇒ 文件长度必须大于「明文长度+16B GCM tag」，否则本用例无区分度",
                onDisk.length > template.length + 16);

        assertEquals("字段应是整个加密模板文件的字节数（含 envelope 头），而非纯密文段长度",
                onDisk.length, add.entry.ciphertextSize);
        assertEquals("字段应是对整个加密模板文件取的 SHA-256，而非仅密文段的摘要",
                FeatureTemplateEncryptedStore.sha256Hex(onDisk), add.entry.ciphertextSha256Hex);

        String[] cols = registryEntryFields(repo, 0);
        assertEquals("注册表条目应为六列（位置式，无列名）", 6, cols.length);
        assertEquals("第 4 列应是模板文件字节数", onDisk.length, Long.parseLong(cols[3]));
        assertEquals("第 5 列应是模板文件 SHA-256", add.entry.ciphertextSha256Hex, cols[4]);
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

    // ---------- ⑧b add 回滚失败必须如实上报（T3 缺口 1） ----------

    /**
     * 注册表写失败后回滚模板**失败**时，`message` 必须如实说明，不得谎报「已回滚模板」。
     *
     * <p><b>为什么必须钉住</b>：孤儿模板存在时，`add` 在模板存在性检查处恒返 `DUPLICATE_USER`，
     * 而 `remove` 先查注册表、无条目即 `NOT_FOUND` ⇒ 删不掉，UI 无自愈路径。该分支此前**零覆盖**
     * （原实现连 {@code deleteWithBackup} 的返回值都没接），失败时文案是假的。
     *
     * <p><b>打红方式</b>：把 {@code boolean rolledBack = …} 去掉、改回无条件「注册表写入失败，
     * 已回滚模板」⇒ 文案断言红。
     */
    @Test
    public void addReportsRollbackFailureWhenTemplateRemovalFails() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();

        // 用非空目录堵住注册表临时文件路径，强制 persistRegistry 失败
        //（手法同 removeKeepsTemplateWhenRegistryWriteFails）。
        File tmpPath = new File(repo.scopeDir(), AndroidEnrollmentRepository.REGISTRY_FILE_NAME + ".tmp");
        assertTrue(tmpPath.mkdirs());
        assertTrue(new File(tmpPath, "block").createNewFile());

        // 删除替身恒失败 ⇒ 模板回滚失败（模板**写入**走存储层真实文件系统，不受该替身影响）。
        repo.setFileOpsForTesting(new UndeletableOps());
        AndroidEnrollmentRepository.Result add;
        try {
            add = repo.add("alice", templateBytes(16, 82), MV, TSV);
        } finally {
            repo.setFileOpsForTesting(null);
        }

        assertFalse("注册表写入失败应回滚模板并返回错误", add.isOk());
        assertTrue("模板回滚失败必须如实上报，实际文案=" + add.message,
                add.message.contains("回滚失败"));
        assertTrue("前置：模板确实残留为孤儿（否则上面的文案断言无意义）",
                repo.templateFileFor("alice").exists());
        assertEquals("孤儿模板的后果：该 userId 之后每次 add 恒返 DUPLICATE_USER（无 UI 自愈路径）",
                AndroidEnrollmentRepository.Status.DUPLICATE_USER,
                repo.add("alice", templateBytes(16, 83), MV, TSV).status);

        new File(tmpPath, "block").delete();
        tmpPath.delete();
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
        // ⚠️ 这里走的是**仓库 API 直调**。经注册 UI 的 add 在本批之后**不可达**（EnrollmentSession
        // 的判重门对有损坏行的会话拒收每一帧 ⇒ 会话永远集不满 8 样本），故 UI 侧的自愈只剩 remove
        // 一条路；详见 ListResult#isEnumerationComplete 的 javadoc 与待办 N1。
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

        // 一次成功写入自愈后，重新变为权威。（同为仓库 API 直调；经 UI 的 add 被判重门挡住，
        // 自愈只剩 remove 一条路 —— 见 ListResult#isEnumerationComplete 的 javadoc。）
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

    // ---------- ⑨c status != OK 时 authoritative 无定义（契约锁定） ----------

    /**
     * 契约锁定：`authoritative` **仅在 `status == OK` 时有意义**。
     *
     * <p>构造「主文件缺失 + 备份存在但备份自身不可解析」：`loadRegistry` 把备份的解析结果**原样**
     * 返回（`CORRUPTED`），此时 `RegistrySnapshot.authoritative` 保持构造默认值 —— 它**不是** false。
     * 故消费方（显示名孤儿清理等破坏性对账）必须先用 `isOk()` 门禁；本用例把该门禁钉住，
     * 而不去断言那个「未定义」的字段值本身。
     *
     * <p><b>打红方式</b>：让该路径返回 `OK` + 空条目集（例如把「备份也解析失败」折叠成「空库」）
     * ⇒ 前两条断言红 —— 那正是「把索引丢失伪装成空库」的回归。改写默认值（或在 CORRUPTED 路径上
     * 显式把 `authoritative` 置 false）⇒ 第三条断言红。
     *
     * <p><b>如实声明覆盖边界</b>：第三条钉的是该「未定义」字段的**观测值**（构造默认值），
     * 它**不是**生产门禁 —— 本用例不、也无法验证真实消费者（`MainActivity.refreshGalleryFromRepository`
     * 的 `!list.isOk()` 早退、`EnrollmentSession.fromRepository` 的枚举完整性判据）是否真的先过门；
     * 那两处由各自的源码/行为用例覆盖。把消费者里的 `isOk()` 门删掉，本用例**仍然全绿**。
     */
    @Test
    public void authoritativeIsMeaninglessWhenStatusNotOk() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        // 主文件缺失 + 备份存在但备份自身损坏。
        Files.write(bakFor(repo).toPath(), "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("备份也不可解析时应如实上报，不得伪装成空库",
                AndroidEnrollmentRepository.Status.CORRUPTED, list.status);
        assertFalse("status != OK ⇒ 消费方必须先用 isOk() 门禁，不得据本字段做破坏性对账",
                list.isOk());
        // 未定义但有观测值：该路径原样返回备份的解析结果，authoritative 保持构造默认值 true
        //（**不是** false）—— 钉住「该值来自默认值、无人刻意置 false」这一现状。
        assertTrue("status != OK 时 authoritative 的观测值为构造默认值 true（未定义，不得据此对账）",
                list.authoritative);
    }

    // ---------- ⑨d 枚举完整性判据（非破坏性判重门禁） ----------

    /**
     * {@link AndroidEnrollmentRepository.ListResult#isEnumerationComplete()} 服务**非破坏性**的判重：
     * 损坏行被跳过时枚举缺行 —— 判重漏一个在册者会让同一张脸被重复注册。
     *
     * <p>它与 {@code authoritative} **不同口径**，本用例钉死其中最关键的一处分歧：全新安装时
     * `authoritative==false`（无法区分「全新安装」与「索引丢失」）但枚举**完整**。
     * 这正是不能在判重路径上改用 `authoritative` 的原因 —— 那会让全新安装永远无法注册。
     *
     * <p><b>接线状况（本用例的判别力从此才成立）</b>：该方法此前在生产**零引用**
     *（`EnrollmentSession.fromRepository` 写的是同义的内联表达式，`MainActivity` 用的是
     * `authoritative`），故本条当时只钉住一个「生产不用」的方法 —— 把真实判据改坏时本用例仍绿。
     * 现已把 `fromRepository` 的门改为调用本方法，新 API 真正接线。
     * ⚠️ 但真正的**行为**门禁仍在 `EnrollmentSessionTest` 的两条用例
     *（`incompleteRegistryEnumerationRejectsSampleWithRepoUnavailable` 与
     * `emptyRepositoryStillAllowsNormalEnrollment`，一拒一放）——
     * 只改本方法而不改调用点，本用例不会红。
     *
     * <p><b>覆盖边界（如实声明）</b>：本用例只覆盖「无注册表文件」与「有损坏行」两种输入，
     * **不**覆盖 {@code .bak} 恢复路径 —— 该路径下本方法同样返回 true，而枚举**可能真的缺行**
     * （备份可能早于最近一次 add），详见该方法 javadoc 里登记的缺口。故本用例不能当作
     * 「判重依据始终可靠」的证据。
     *
     * <p><b>打红方式</b>：把 {@code isEnumerationComplete} 退化成 `return authoritative;`
     * ⇒「全新目录 ⇒ true」红；去掉其 `skippedLineCount == 0` 项 ⇒「有坏行 ⇒ false」红。
     */
    @Test
    public void enumerationCompletenessIsIndependentOfAuthoritative() throws Exception {
        // ① 全新目录：无注册表文件 ⇒ 非权威（破坏性对账不得据此删除），但枚举完整（无缺行）。
        AndroidEnrollmentRepository repo = newRepo();
        AndroidEnrollmentRepository.ListResult fresh = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, fresh.status);
        assertFalse("无注册表 ⇒ 非权威", fresh.authoritative);
        assertTrue("但无损坏行 ⇒ 枚举完整，不得阻断全新安装的判重/注册",
                fresh.isEnumerationComplete());

        // ② 正常库（注册表文件存在、无坏行）⇒ 枚举完整。
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 91), MV, TSV).status);
        assertTrue("正常库的枚举完整", repo.list().isEnumerationComplete());

        // ③ 追加一行垃圾（合法头 + 单行损坏被隔离）⇒ status 仍 OK，但枚举缺行。
        try (FileOutputStream fos = new FileOutputStream(repo.registryFile(), true)) {
            fos.write("this-is-a-corrupt-row\n".getBytes(StandardCharsets.UTF_8));
        }
        AndroidEnrollmentRepository.ListResult partial = repo.list();
        assertEquals(AndroidEnrollmentRepository.Status.OK, partial.status);
        assertEquals(1, partial.skippedLineCount);
        assertFalse("有被跳过的损坏行 ⇒ 枚举不完整（被跳过者仍可能在册）",
                partial.isEnumerationComplete());
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

    // ---------- ⑫b 头损坏不回退 .bak（刻意收窄，行为锁定） ----------

    /**
     * 行为锁定：**只有 IO_ERROR 触发 .bak 回退，CORRUPTED 不触发**（判据范围见 `loadRegistry` 注释）。
     *
     * <p>与既有 {@link #unreadableMainRecoversFromBackup} 同构，把「非空目录占用主文件」换成
     * 「写入非法魔数」：主文件头损坏 + 备份**可解析**时仍报 `CORRUPTED`，且**不得消费备份**。
     * 理由：写入器经 tmp→rename 替换，崩溃窗口只产生「缺失/不可读」；头损坏来自外部破坏，
     * 用可能同样陈旧、未必完好的 bak 覆盖回去反而可能丢增量。
     *
     * <p><b>打红方式</b>：把判据改成 {@code IO_ERROR || CORRUPTED} ⇒ status 变 OK、条目集取自备份，
     * 前两条断言即红（第三条「bak 是否仍在盘上」取决于平台 rename 语义，不作为判红依据）。
     */
    @Test
    public void corruptHeaderDoesNotConsumeBackup() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertEquals(AndroidEnrollmentRepository.Status.OK,
                repo.add("alice", templateBytes(16, 84), MV, TSV).status);
        byte[] good = Files.readAllBytes(repo.registryFile().toPath());

        // 主文件头损坏（外部破坏的形态）；备份本身完全可解析。
        Files.write(repo.registryFile().toPath(),
                "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));
        File bak = bakFor(repo);
        Files.write(bak.toPath(), good);

        AndroidEnrollmentRepository.ListResult list = repo.list();
        assertEquals("头损坏应判 CORRUPTED，不得改用备份",
                AndroidEnrollmentRepository.Status.CORRUPTED, list.status);
        assertTrue("条目集不得来自备份（否则等于放宽了回退判据）", list.entries.isEmpty());
        assertTrue("不得消费备份：bak 必须原样留在盘上", bak.exists());
        assertEquals("主文件不得被备份覆盖", "NOT_A_REGISTRY",
                new String(Files.readAllBytes(repo.registryFile().toPath()),
                        StandardCharsets.UTF_8).trim());
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

    // ---------- ⑭ 结构守卫：无静态接缝（FACE-TEST-003 与存储侧对偶） ----------

    /**
     * **结构守卫**（FACE-TEST-003 补齐）：{@code AndroidEnrollmentRepository} 上**不存在**静态注入入口 /
     * 静态接缝类型字段（本类的两个接缝 {@link FileStoreUtils.FileOps} 与 {@link
     * FeatureTemplateEncryptedStore.PlatformKeyProvider} 均为**实例字段**，注入只作用在本实例）。
     *
     * <p>与 {@code FeatureTemplateEncryptedStoreTest.featureTemplateEncryptedStoreExposesNoStaticInjectionSeam}
     * 同口径。此前本类仅有**行为性**守卫 {@link #injectedOpsArePerInstanceAndDoNotLeak}，缺**结构性**守卫：
     * {@code fileStoreUtilsExposesNoStaticInjectionSeam} 只反射 {@code FileStoreUtils.class}，不覆盖本类。
     *
     * <p><b>判据（含 FACE-TEST-003 收紧）</b>：
     * <ul>
     *   <li><b>方法①（收紧）</b>：不存在「名为 {@code set*} 且带接缝类型参数」的**静态**方法 ——
     *       **不限返回类型**（堵「返回非 void 的静态 setter」）、**不限参数个数**（堵「多参静态 setter」）；</li>
     *   <li><b>方法②</b>：不存在「返回 {@code void} 且参数恰为一个接缝类型」的静态方法；</li>
     *   <li><b>字段③</b>：不存在任何静态接缝类型字段。</li>
     * </ul>
     *
     * <p><b>为何不误伤既有合法 API</b>：本类合法注入方法 {@code setFileOpsForTesting} /
     * {@code setKeyProviderForTesting} 均为**实例**方法 ⇒ 方法判据（先过滤 {@code static}）不命中；静态
     * 工厂 {@code forDefaultGallery}/{@code forScope}、静态 {@code dirLockForTesting} 等均不带接缝参数类型。</p>
     *
     * <p><b>会红在哪些改法上</b>：把 {@code setFileOpsForTesting}/{@code setKeyProviderForTesting} 改为
     * {@code static}，或新增「返回非 void / 多参」的静态 setter ⇒ 判据①红；新增任意静态接缝类型字段 ⇒
     * 字段判据红。</p>
     *
     * <p><b>边界（如实声明）</b>：仍只按**形态**判据（setter 命名 + 接缝参数/字段类型）；以其它类型或非
     * setter 命名承载的静态注入不覆盖。</p>
     */
    @Test
    public void androidEnrollmentRepositoryExposesNoStaticInjectionSeam() {
        for (Method m : AndroidEnrollmentRepository.class.getDeclaredMethods()) {
            if (!Modifier.isStatic(m.getModifiers())) {
                continue;   // 合法注入方法在本类均为**实例**方法，故只查静态
            }
            boolean hasSeamParam = false;
            for (Class<?> p : m.getParameterTypes()) {
                if (isSeamType(p)) {
                    hasSeamParam = true;
                    break;
                }
            }
            assertFalse("不应暴露静态注入入口（名为 set* 且带接缝类型参数的静态方法，"
                            + "不限返回类型/参数个数）：" + m.getName(),
                    m.getName().startsWith("set") && hasSeamParam);
            boolean voidSingleSeam = m.getReturnType() == void.class
                    && m.getParameterCount() == 1
                    && isSeamType(m.getParameterTypes()[0]);
            assertFalse("不应暴露静态注入入口（返回 void 且参数恰为一个接缝类型）：" + m.getName(),
                    voidSingleSeam);
        }
        for (Field f : AndroidEnrollmentRepository.class.getDeclaredFields()) {
            assertFalse("AndroidEnrollmentRepository 不应持有任何静态接缝类型字段（FileOps / PlatformKeyProvider）"
                            + "——默认值请引用 FileStoreUtils.DEFAULT_FILE_OPS / "
                            + "FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER，静态字段会成为进程级可变注入入口："
                            + f.getName(),
                    Modifier.isStatic(f.getModifiers()) && isSeamType(f.getType()));
        }
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

    /**
     * 读注册表第 {@code index} 条条目的空格分隔字段（跳过首行魔数）。
     *
     * <p>注册表是**位置式**六列文本、无列名 ⇒ 改错列只会静默丢元数据，故按列位断言而不是只看字段。
     */
    private static String[] registryEntryFields(AndroidEnrollmentRepository repo, int index) throws Exception {
        List<String> lines = Files.readAllLines(repo.registryFile().toPath(), StandardCharsets.UTF_8);
        return lines.get(1 + index).trim().split("\\s+");
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

    /** 类型是否为任一测试接缝（{@link FileStoreUtils.FileOps} / {@link FeatureTemplateEncryptedStore.PlatformKeyProvider}）。 */
    private static boolean isSeamType(Class<?> type) {
        return FileStoreUtils.FileOps.class.isAssignableFrom(type)
                || FeatureTemplateEncryptedStore.PlatformKeyProvider.class.isAssignableFrom(type);
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
