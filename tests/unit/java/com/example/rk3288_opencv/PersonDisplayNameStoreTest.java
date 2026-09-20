package com.example.rk3288_opencv;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

/**
 * 显示名缓存层（任务 B）单元测试。
 *
 * <p>与 {@code FeatureTemplateEncryptedStoreTest} 同一手法：注入内存 JCE 密钥替身，
 * 故**纯 JVM 下即跑真实 AES-GCM**（无需 Robolectric，本仓也未引入）。
 */
public class PersonDisplayNameStoreTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Before
    public void setUp() throws Exception {
        // §2.5-D / 子代理 4 P4：删除行为接缝已由「进程级静态」改为**实例级注入**
        // （PersonDisplayNameStore#setFileOpsForTesting），每个测试在**自己创建的实例**上注入，
        // 故无需（也无法）在类级 setUp/tearDown 里做跨类复位 —— 跨实例泄漏在结构上不可能发生。
        KeyGenerator kg = KeyGenerator.getInstance("AES");
        kg.init(128);
        final SecretKey key = kg.generateKey();
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return true;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        return key;
                    }
                });
    }

    @After
    public void tearDown() {
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(null);
    }

    /**
     * 模拟「文件删不掉」：{@code delete} 恒返回 false、{@code exists} 恒返回 true。
     *
     * <p>**记录被尝试删除的文件名**（而非仅计数）—— A2 审计指出：只断言「调用次数 > 0」挡不住
     * 「实现里漏删主文件、只删 .bak」这类 bug（那种情况下仍有一次调用）。记录名字后可以逐项断言
     * **主文件与其 .bak 都被尝试过**，守卫才真正承重。
     */
    private static final class UndeletableOps implements FileStoreUtils.FileOps {
        final java.util.List<String> attempted = new java.util.ArrayList<>();

        @Override
        public boolean delete(File f) {
            attempted.add(f.getName());
            return false;
        }

        @Override
        public boolean exists(File f) {
            // 反映**真实**文件状态，而不是恒 true（子代理 4 P6）：恒 true 只是「恰好」等于
            // 「当前实现只在末尾对主文件查一次 exists」这一**实现细节**的真相，属白盒耦合——
            // 一旦实现改为先查 .bak、或在删除前查 exists，恒 true 的替身就会**说谎**
            // （对不存在的 .bak 报告存在）。用真实状态后，替身不再预设调用序。
            return f.exists();
        }
    }

    /** 注入「delete 抛 SecurityException」的替身：用于覆盖异常路径（子代理 4 P1）。 */
    private static final class ThrowingOps implements FileStoreUtils.FileOps {
        @Override
        public boolean delete(File f) {
            throw new SecurityException("probe: delete denied");
        }

        @Override
        public boolean exists(File f) {
            return f.exists();
        }
    }

    /** 未缓存时 get 必须降级为 personId（UI 线程纯读、不触发 IO）。 */
    @Test
    public void getFallsBackToPersonIdWhenUncached() throws Exception {
        PersonDisplayNameStore s = new PersonDisplayNameStore(tmp.newFolder());
        assertEquals("alice", s.get("alice"));
        assertEquals(0, s.cachedCount());
    }

    /** save 应写穿缓存，随后的 get 立即命中。 */
    @Test
    public void saveWritesThroughCache() throws Exception {
        PersonDisplayNameStore s = new PersonDisplayNameStore(tmp.newFolder());
        assertTrue(s.save("alice", "张三"));
        assertEquals("张三", s.get("alice"));
        assertEquals(1, s.cachedCount());
    }

    /** 换一个实例（空缓存）后 prefetch 应从磁盘解密回来 —— 证明真的落了盘且可读回。 */
    @Test
    public void prefetchDecryptsFromDisk() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore writer = new PersonDisplayNameStore(dir);
        assertTrue(writer.save("alice", "张三"));

        PersonDisplayNameStore reader = new PersonDisplayNameStore(dir);
        assertEquals("换实例后未预取应降级", "alice", reader.get("alice"));
        reader.prefetch(Collections.singletonList("alice"));
        assertEquals("张三", reader.get("alice"));
    }

    /** 空显示名一律拒绝且不落盘（get 自会降级为 personId）。 */
    @Test
    public void saveRejectsBlankName() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertFalse(s.save("alice", ""));
        assertFalse(s.save("alice", "   "));
        assertFalse(s.save("alice", null));
        assertEquals(0, s.cachedCount());
        assertFalse(FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice").exists());
    }

    /** prefetch 应丢弃「已不在人员集合内」的缓存条目，但**不得**删文件（集合未必权威）。 */
    @Test
    public void prefetchPrunesCacheButKeepsFiles() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));
        assertTrue(s.save("bob", "李四"));
        assertEquals(2, s.cachedCount());

        s.prefetch(Collections.singletonList("alice"));

        assertEquals("已不在集合内的条目应从缓存丢弃", 1, s.cachedCount());
        assertEquals("存活项仍应命中缓存", "张三", s.get("alice"));
        assertEquals("bob 应降级为 personId", "bob", s.get("bob"));
        assertTrue("prefetch 不得删文件",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob").exists());
    }

    /** remove 应同时删文件与缓存条目。 */
    @Test
    public void removeDeletesFileAndCacheEntry() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        assertTrue(f.isFile());

        s.remove("alice");

        assertFalse("文件应被删除", f.exists());
        assertEquals("缓存应被清理", 0, s.cachedCount());
        assertEquals("alice", s.get("alice"));
    }

    /** removeOrphans 只删「显示名文件」中未被列出的，且不得误删模板文件。 */
    @Test
    public void removeOrphansDeletesOnlyUnlistedNameFiles() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));
        assertTrue(s.save("bob", "李四"));

        // 同一目录放一个模板文件：其前缀为 u_，绝不能被当作显示名孤儿删除。
        File tpl = new File(dir, "u_deadbeef.bin");
        java.nio.file.Files.write(tpl.toPath(), new byte[]{1});

        int removed = s.removeOrphans(Collections.singleton("alice"));

        assertEquals("应只删 bob 的显示名文件", 1, removed);
        assertTrue("alice 的文件应保留",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice").exists());
        assertFalse("bob 的文件应被删除",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob").exists());
        assertTrue("模板文件不得被误删", tpl.exists());
    }

    /** 平台不支持（API<23）：save 返回 false、prefetch 不崩、get 降级 —— 且绝不落盘明文。 */
    @Test
    public void unsupportedPlatformDegradesWithoutPlaintext() throws Exception {
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return false;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        throw new AssertionError("不支持时不得取密钥");
                    }
                });

        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertFalse(s.save("alice", "张三"));
        assertEquals(0, s.cachedCount());
        s.prefetch(Arrays.asList("alice"));            // 不得抛异常
        assertEquals("alice", s.get("alice"));
        assertEquals("降级时目录内不得出现任何文件", 0, dir.listFiles().length);
    }

    /**
     * 预取必须**重新解密**而非「命中即跳过」（A2 审计 Low 修复）：文件被带外改动后，下一次预取
     * 应反映磁盘现状，而不是一直显示陈旧名字。
     */
    @Test
    public void prefetchRefreshesChangedFileInsteadOfServingStaleCache() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore cache = new PersonDisplayNameStore(dir);
        assertTrue(cache.save("alice", "旧名"));
        assertEquals("旧名", cache.get("alice"));

        // 带外改写：另一个实例写入新值（模拟外部改动 / 密钥轮换后的重写）。
        PersonDisplayNameStore writer = new PersonDisplayNameStore(dir);
        assertTrue(writer.save("alice", "新名"));

        cache.prefetch(Collections.singletonList("alice"));
        assertEquals("预取应重新解密、反映磁盘现状", "新名", cache.get("alice"));
    }

    /** 磁盘字节不得含明文显示名。 */
    @Test
    public void persistedFileHasNoPlaintext() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "UniquelyNamedPerson"));
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        String text = new String(java.nio.file.Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
        assertFalse("磁盘不得含显示名明文", text.contains("UniquelyNamedPerson"));
        assertFalse("磁盘不得含 personId 明文", text.contains("alice"));
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // §2.5-D：删除失败路径（FACE-UI-011 要点⑥ 此前缺失的第三类用例）
    // ═══════════════════════════════════════════════════════════════════════════
    //
    // 此前的缺口：FileStoreUtils.deleteWithBackup 无注入接缝，而真实文件系统上很难**稳定**制造
    // delete() 失败（POSIX 看父目录写权限、Windows 看句柄占用，语义还不一致）⇒ 纯 JVM 下无法构造。
    // 现经 FileStoreUtils.FileOps 接缝注入失败行为。

    /** 删除失败时 remove 仍须清理缓存、不抛异常（remove 的契约是「文件删除失败不影响缓存清理」）。 */
    @Test
    public void removeClearsCacheEvenWhenFileDeleteFails() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        assertTrue(f.isFile());

        UndeletableOps ops = new UndeletableOps();
        s.setFileOpsForTesting(ops);
        try {
            s.remove("alice");  // 不得抛异常
        } finally {
            s.setFileOpsForTesting(null);
        }

        // 守卫承重点（A2 M1）：必须**逐项**证明主文件与其 .bak 都被尝试删除 —— 仅断言「调用次数 > 0」
        // 挡不住「实现里漏删主文件、只删 .bak」这类 bug（那种情况下仍有一次调用）。
        assertTrue("必须尝试删主文件（只删 .bak 是错的）", ops.attempted.contains(f.getName()));
        assertTrue("必须尝试删 .bak（否则读路径的『阶段 1 恢复基线』会复活已删除数据）",
                ops.attempted.contains(f.getName() + ".bak"));
        assertEquals("删除失败也必须清理缓存", 0, s.cachedCount());
        assertEquals("缓存已清 ⇒ get 降级为 personId", "alice", s.get("alice"));
        assertTrue("删除确实失败 ⇒ 文件仍在（不得被谎报为已删）", f.exists());
    }

    /** 删除失败时 removeOrphans 不得把失败计入删除数（返回值以「确实已不存在」为准）。 */
    @Test
    public void removeOrphansDoesNotCountFailedDeletes() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));

        UndeletableOps ops = new UndeletableOps();
        s.setFileOpsForTesting(ops);
        int removed;
        try {
            removed = s.removeOrphans(Collections.emptySet());
        } finally {
            s.setFileOpsForTesting(null);
        }

        final String name = FeatureTemplateEncryptedStore
                .nameFileForPersonInDir(dir, "alice").getName();
        // 同 A2 M1：逐项断言，而非只数调用次数。
        assertTrue("必须尝试删主文件", ops.attempted.contains(name));
        assertTrue("必须尝试删 .bak", ops.attempted.contains(name + ".bak"));
        assertEquals("删不掉就不得计入", 0, removed);
        assertTrue("文件应仍在",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice").exists());
    }

    /**
     * 默认实现下的**幂等**语义：文件本来就不存在时 {@code File.delete()} 会返回 false，
     * 但那属于「已经删掉了」⇒ deleteWithBackup 应报**成功**，不得因返回值而误报失败。
     *
     * <p>⚠️ 定位如实（A2 M3）：这是**特征化测试**（钉住**改前就已如此**的语义），
     * <b>不是</b> §2.5-D 接缝的回归网 —— 它不注入接缝，改前改后都通过。
     * 它真正防的是「把返回值改成 `delete()` 的返回值」这一**假想实现**（那样本用例会红）。
     */
    @Test
    public void deleteWithBackupTreatsMissingFileAsSuccess() throws Exception {
        File dir = tmp.newFolder();
        File missing = new File(dir, "n_neverexisted.enc");
        assertFalse(missing.exists());
        assertTrue("不存在的文件：删除应视为成功（幂等）",
                FileStoreUtils.deleteWithBackup(missing, FileStoreUtils.DEFAULT_FILE_OPS));
    }

    /** {@code deleteWithBackup} 的 {@code parent == null} 分支（无父目录的裸文件名）此前无覆盖（子代理 4 P7）。 */
    @Test
    public void deleteWithBackupHandlesFileWithoutParent() {
        File bare = new File("no_such_parent_probe.enc");
        assertNull("前置：该文件不应有父目录", bare.getParentFile());
        // 不存在的文件 ⇒ 幂等成功；且不得因 parent==null 抛异常
        assertTrue(FileStoreUtils.deleteWithBackup(bare, FileStoreUtils.DEFAULT_FILE_OPS));
    }

    /**
     * **特征化测试（钉住现状，不是期望）** —— 子代理 4 P1 指出的覆盖缺口。
     *
     * <p>{@code FileStoreUtils.deleteWithBackup} **不吞** {@code SecurityException}，
     * 而本类的 {@code remove} **没有** try/catch ⇒ 异常会传播出去并**跳过缓存清理**。
     * 这与 {@code remove} 的 javadoc 承诺「文件删除失败不影响缓存清理」**不一致**。
     *
     * <p>影响面：Android 上 {@code File.delete()} 抛 {@code SecurityException} 需要 SecurityManager，
     * 现代 Android 已无 ⇒ **实际不可达**。故本次**只钉现状、不改行为**，修复选项另行记录：
     * ① 在 {@code remove} 内 catch；② 让 {@code deleteWithBackup} 不抛（与本类
     * {@code renameQuietly} 的「吞 {@code SecurityException}」保持一致）。
     *
     * <p>⚠️ 若将来按 ① 或 ② 修好，**本用例会红**（那正是它存在的意义），届时改为断言「缓存已清理」。
     */
    @Test
    public void removePropagatesSecurityExceptionAndSkipsCacheClear() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        assertTrue(s.save("alice", "张三"));
        assertEquals(1, s.cachedCount());

        s.setFileOpsForTesting(new ThrowingOps());
        try {
            try {
                s.remove("alice");
                fail("当前实现不 catch SecurityException，应当传播（若此处不抛，说明行为已改变，请更新本用例）");
            } catch (SecurityException expected) {
                // 现状如此：异常未被拦截
            }
        } finally {
            s.setFileOpsForTesting(null);
        }
        assertEquals("异常路径下缓存未被清理 —— 与 javadoc 契约不一致，已记录待决", 1, s.cachedCount());
    }

    // ═══════════════════════════════════════════════════════════════════════════
    // 子代理 4 P4：实例级注入的结构守卫（替代此前的「进程级静态 + 各测试类自觉复位」约定）
    // ═══════════════════════════════════════════════════════════════════════════

    /**
     * 实例级注入**不跨实例泄漏**：两个实例各注入不同替身、互不影响。
     *
     * <p><b>为什么这条守卫生效</b>（子代理 4 P4）：此前接缝是 {@code FileStoreUtils} 上的
     * {@code private static volatile FileOps}，而 {@code :app:testDebugUnitTest} 的所有测试类在
     * **同一 JVM 内串行**执行 —— 任一类注入后忘记复位，就会污染其后所有走 {@code deleteWithBackup}
     * 的类。改为实例字段后，注入只作用在**本实例**上；本用例把这个不变量钉死。
     *
     * <p><b>打红方式</b>（改任一处即红）：
     * <ul>
     *   <li>让 {@code PersonDisplayNameStore#setFileOpsForTesting} 写回 {@code FileStoreUtils} 的
     *       **静态**字段，或让 {@code deleteWithBackup} 读任何**静态** {@code fileOps} —— 则对 {@code b}
     *       的注入会覆盖对 {@code a} 的注入，{@code a.remove} 将改用 {@code opsB} ⇒
     *       「实例 a 的替身应被触达」与「实例 b 的替身不得被 a 触达」两断言同时红；</li>
     *   <li>让 {@code remove}/{@code removeOrphans} 不再传本实例的 {@code fileOps}（例如误传
     *       全局默认）⇒ 「实例 a 的替身应被触达」红。</li>
     * </ul>
     */
    @Test
    public void injectedOpsArePerInstanceAndDoNotLeak() throws Exception {
        File dirA = tmp.newFolder("a");
        File dirB = tmp.newFolder("b");
        PersonDisplayNameStore a = new PersonDisplayNameStore(dirA);
        PersonDisplayNameStore b = new PersonDisplayNameStore(dirB);
        assertTrue(a.save("alice", "A"));
        assertTrue(b.save("bob", "B"));

        String nameA = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dirA, "alice").getName();
        String nameB = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dirB, "bob").getName();
        assertFalse("前置：两个实例的文件名应不同（否则名字断言无区分度）", nameA.equals(nameB));

        UndeletableOps opsA = new UndeletableOps();
        UndeletableOps opsB = new UndeletableOps();
        a.setFileOpsForTesting(opsA);
        b.setFileOpsForTesting(opsB);

        a.remove("alice");   // 失败替身 ⇒ a 的文件删不掉

        assertTrue("实例 a 的替身应被触达", opsA.attempted.contains(nameA));
        assertTrue("实例 a 的替身应触达 .bak", opsA.attempted.contains(nameA + ".bak"));
        assertTrue("实例 b 的替身不得被 a 的操作触达（否则存在跨实例泄漏）",
                opsB.attempted.isEmpty());
        assertTrue("a 的删除失败 ⇒ a 的文件仍在（不得被谎报）",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dirA, "alice").exists());
        assertTrue("b 的文件不得受 a 的操作影响",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dirB, "bob").exists());

        int aCallsAfterRemove = opsA.attempted.size();
        b.remove("bob");
        assertTrue("实例 b 的替身应被触达", opsB.attempted.contains(nameB));
        assertEquals("a 的替身调用数不得因 b 的操作而变化", aCallsAfterRemove, opsA.attempted.size());
    }

    /**
     * **结构守卫**：{@code FileStoreUtils} 上**不再存在**静态注入入口 / 任意静态 {@code FileOps} 注入字段。
     *
     * <p>反例原理：只要类上还能「一次注入、全局生效」，本仓的 JVM 内串行测试模型就可能被跨类污染
     * （子代理 4 P4）。故用反射把「没有静态接缝」钉成可执行断言。
     *
     * <p><b>打红方式</b>（改任一处即红）：
     * <ul>
     *   <li>新增**任何**接受单个 {@code FileOps} 参数的静态方法（**不限名**，含把
     *       {@code setFileOpsForTesting} 改名后重新加回）⇒ 方法断言红；</li>
     *   <li>新增**任意**静态 {@code FileOps} 字段（含 {@code private static final FileOps fileOps}）——
     *       仅放行唯一合法常量 {@code DEFAULT_FILE_OPS} ⇒ 字段断言红。</li>
     * </ul>
     *
     * <p><b>本守卫的边界（如实声明）</b>：
     * <ul>
     *   <li>只按**形态**判据：静态方法「参数恰为一个 {@code FileOps}」、静态字段「类型为
     *       {@code FileOps}」。若有人以**其它类型**承载注入（如静态 {@code Consumer<FileOps>}、
     *       静态 {@code Map} 缓存替身），本条**不覆盖**；</li>
     *   <li>合法的 {@code deleteWithBackup(File, FileOps)}（静态、双参）不在拦截范围内——
     *       它是生产调用方显式传入 {@code ops} 的正规入口，非注入接缝。</li>
     * </ul>
     */
    @Test
    public void fileStoreUtilsExposesNoStaticInjectionSeam() {
        for (Method m : FileStoreUtils.class.getDeclaredMethods()) {
            boolean staticMethod = Modifier.isStatic(m.getModifiers());
            // 注入接缝形态：任意名静态方法，参数恰为一个 FileOps
            boolean takesSingleFileOps = m.getParameterCount() == 1
                    && FileStoreUtils.FileOps.class.equals(m.getParameterTypes()[0]);
            assertFalse("FileStoreUtils 不应再暴露静态注入入口（不限名的单 FileOps 参数静态方法）："
                            + m.getName(),
                    staticMethod && takesSingleFileOps);
        }
        for (Field f : FileStoreUtils.class.getDeclaredFields()) {
            boolean staticFileOps = Modifier.isStatic(f.getModifiers())
                    && FileStoreUtils.FileOps.class.isAssignableFrom(f.getType());
            if (!staticFileOps) {
                continue;
            }
            assertTrue("静态 FileOps 字段必须是 final 不可变常量：" + f.getName(),
                    Modifier.isFinal(f.getModifiers()));
            assertTrue("静态 FileOps 字段仅允许唯一合法常量 DEFAULT_FILE_OPS，不得新增注入字段："
                            + f.getName(),
                    "DEFAULT_FILE_OPS".equals(f.getName()));
        }
        // 编译期引用：证明 DEFAULT_FILE_OPS 仍对同包可见（生产调用方的默认实例）。
        assertTrue("DEFAULT_FILE_OPS 应对同包可见且非空", FileStoreUtils.DEFAULT_FILE_OPS != null);
    }
}
