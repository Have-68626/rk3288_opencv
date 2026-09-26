package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.lang.reflect.Field;
import java.lang.reflect.GenericArrayType;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.lang.reflect.WildcardType;
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

    /**
     * 供各用例显式注入的密钥提供者替身（内存 JCE 密钥）。此前是进程级静态注入
     * （{@code setPlatformKeyProviderForTesting}），改造后经各实例的 {@code setKeyProviderForTesting}
     * 注入到**本用例创建的实例**上（见 {@link #newStore(File)}），故持为字段。
     */
    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
        // §2.5-D / 子代理 4 P4：删除行为接缝已由「进程级静态」改为**实例级注入**
        // （PersonDisplayNameStore#setFileOpsForTesting）；本轮密钥提供者接缝同样改为实例级注入
        // （PersonDisplayNameStore#setKeyProviderForTesting）。每个测试在**自己创建的实例**上注入，
        // 故无需（也无法）在类级 setUp/tearDown 里做跨类复位 —— 跨实例泄漏在结构上不可能发生。
        KeyGenerator kg = KeyGenerator.getInstance("AES");
        kg.init(128);
        final SecretKey key = kg.generateKey();
        provider = new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
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

    // 无 @After（改造前它用于复位进程级静态 provider）：密钥提供者接缝已是实例级，
    // 各实例由 newStore(File) 就地注入，本测试类不再改写任何静态状态 ⇒ 无静态状态可复位。

    /** 构造并注入测试用密钥提供者替身（避免 21 处构造点各自重复注入）。 */
    private PersonDisplayNameStore newStore(File storeDir) {
        PersonDisplayNameStore s = new PersonDisplayNameStore(storeDir);
        s.setKeyProviderForTesting(provider);
        return s;
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

    /** 注入「delete 抛 SecurityException」的替身：用于覆盖异常路径。 */
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

    /**
     * 首次 {@code delete} 抛 {@code SecurityException}，其后转为**真实**删除。
     *
     * <p>用于证明 {@code removeOrphans}「单个孤儿删除失败**不中断整轮**」：无论 {@code listFiles()}
     * 的返回顺序如何，整轮恰好有一次 delete 调用抛异常，其余均真实执行 ⇒ 恰有一个孤儿删不掉、其余
     * 全被删除。断言不依赖遍历顺序，因此是确定性的。
     */
    private static final class ThrowOnceThenRealOps implements FileStoreUtils.FileOps {
        private boolean thrown = false;

        @Override
        public boolean delete(File f) {
            if (!thrown) {
                thrown = true;
                throw new SecurityException("probe: delete denied once");
            }
            return f.delete();
        }

        @Override
        public boolean exists(File f) {
            return f.exists();
        }
    }

    /**
     * **三态混合替身**：按**目标文件基名**分流 —— 指定基名恒抛 {@code SecurityException}、另一指定
     * 基名恒返回 {@code false}（删不掉）、其余真实删除。
     *
     * <p><b>为什么按名字而非调用序分流</b>：{@link FileStoreUtils#deleteWithBackup} 对每个孤儿会先删
     * {@code .bak}、再删主文件（共 2 次 {@code delete} 调用），故「第 N 次调用」的分流会随
     * {@code listFiles()} 遍历顺序漂移；按**基名**（剥掉 {@code .bak} 后缀）分流则与遍历顺序无关，
     * 断言是**确定性**的。
     */
    private static final class ThrowThenUndeletableThenRealOps implements FileStoreUtils.FileOps {
        private final String throwingBase;     // 该基名：delete 恒抛 SecurityException
        private final String undeletableBase;  // 该基名：delete 恒返回 false

        ThrowThenUndeletableThenRealOps(String throwingBase, String undeletableBase) {
            this.throwingBase = throwingBase;
            this.undeletableBase = undeletableBase;
        }

        private static String baseName(File f) {
            String n = f.getName();
            return n.endsWith(".bak") ? n.substring(0, n.length() - 4) : n;
        }

        @Override
        public boolean delete(File f) {
            String b = baseName(f);
            if (b.equals(throwingBase)) {
                throw new SecurityException("probe: delete denied (mixed)");
            }
            if (b.equals(undeletableBase)) {
                return false;
            }
            return f.delete();
        }

        @Override
        public boolean exists(File f) {
            return f.exists();
        }
    }

    /** 未缓存时 get 必须降级为 personId（UI 线程纯读、不触发 IO）。 */
    @Test
    public void getFallsBackToPersonIdWhenUncached() throws Exception {
        PersonDisplayNameStore s = newStore(tmp.newFolder());
        assertEquals("alice", s.get("alice"));
        assertEquals(0, s.cachedCount());
    }

    /** save 应写穿缓存，随后的 get 立即命中。 */
    @Test
    public void saveWritesThroughCache() throws Exception {
        PersonDisplayNameStore s = newStore(tmp.newFolder());
        assertTrue(s.save("alice", "张三"));
        assertEquals("张三", s.get("alice"));
        assertEquals(1, s.cachedCount());
    }

    /** 换一个实例（空缓存）后 prefetch 应从磁盘解密回来 —— 证明真的落了盘且可读回。 */
    @Test
    public void prefetchDecryptsFromDisk() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore writer = newStore(dir);
        assertTrue(writer.save("alice", "张三"));

        PersonDisplayNameStore reader = newStore(dir);
        assertEquals("换实例后未预取应降级", "alice", reader.get("alice"));
        reader.prefetch(Collections.singletonList("alice"));
        assertEquals("张三", reader.get("alice"));
    }

    /** 空显示名一律拒绝且不落盘（get 自会降级为 personId）。 */
    @Test
    public void saveRejectsBlankName() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
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
        PersonDisplayNameStore s = newStore(dir);
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
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "张三"));
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        assertTrue(f.isFile());

        assertTrue("删除成功 ⇒ 必须报告 true", s.remove("alice"));

        assertFalse("文件应被删除", f.exists());
        assertEquals("缓存应被清理", 0, s.cachedCount());
        assertEquals("alice", s.get("alice"));
    }

    /** removeOrphans 只删「显示名文件」中未被列出的，且不得误删模板文件。 */
    @Test
    public void removeOrphansDeletesOnlyUnlistedNameFiles() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
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

    /**
     * A4 修复（Med）判别性用例：removeOrphans 的谓词必须**同时匹配写入器备份** {@code n_*.enc.bak}。
     *
     * <p><b>场景</b>：某人已注销/数据陈旧，其显示名主文件已不在、**只剩 {@code .bak}**（上次写入崩在
     * 「dst→bak」之后、「tmp→dst」完成前）。此前谓词只认 {@code endsWith(".enc")} ⇒ 该 {@code .bak}
     * **不进入循环**、永不被清理；而读路径的「阶段 1 恢复基线」会在下次读取时把它改名还原为主文件并
     * 解密 ⇒ **持久的 PII 复活种子**（与已修的「+207」同类，那次只覆盖 remove 路径）。
     *
     * <p><b>为何断言 {@code readFromDisk} 为 null</b>：这直接把「不得复活」钉在**读路径的真实行为**上
     * ——清干净后，新实例经 {@code readEnvelope} 读 bob 应取不到任何明文；若 {@code .bak} 未删，读路径
     * 会把它还原为主文件并成功解密（返回「李四」）⇒ 本断言同样红。
     *
     * <p><b>打红方式</b>：把谓词改回只匹配 {@code .enc}（不剥 {@code .bak} 后缀）⇒ 下面三条断言全红。
     */
    @Test
    public void removeOrphansDeletesOrphanedBackupWhenMainFileGone() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "张三"));
        assertTrue(s.save("bob", "李四"));

        File bobMain = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob");
        File bobBak = new File(dir, bobMain.getName() + ".bak");
        // 构造崩溃现场：主文件已不在，只剩 .bak。
        assertTrue("前置：bob 主文件应存在", bobMain.isFile());
        assertTrue("前置：应能把主文件改名为 .bak", bobMain.renameTo(bobBak));
        assertFalse("前置：主文件应已不在", bobMain.exists());
        assertTrue("前置：只剩 .bak", bobBak.isFile());

        int removed = s.removeOrphans(Collections.singleton("alice"));

        assertEquals("只剩 .bak 的孤儿必须被清理并计入删除数", 1, removed);
        assertFalse("孤儿 .bak 应已被删除", bobBak.exists());
        assertFalse("主文件不得被读路径的阶段 1 恢复基线复活", bobMain.exists());
        assertNull("清理后新实例不得再读到 bob 的显示名（无 .bak 可复活）",
                newStore(dir).readFromDisk("bob"));
        assertTrue("在册的 alice 主文件应保留",
                FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice").exists());
    }

    /** 平台不支持（API<23）：save 返回 false、prefetch 不崩、get 降级 —— 且绝不落盘明文。 */
    @Test
    public void unsupportedPlatformDegradesWithoutPlaintext() throws Exception {
        // 本用例需要「平台不支持」的替身（与 setUp 的可用替身相反）；接缝已是**实例级**，
        // 故在本用例创建的实例上注入，不影响其它实例。
        FeatureTemplateEncryptedStore.PlatformKeyProvider unsupported =
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return false;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        throw new AssertionError("不支持时不得取密钥");
                    }
                };

        File dir = tmp.newFolder();
        PersonDisplayNameStore s = new PersonDisplayNameStore(dir);
        s.setKeyProviderForTesting(unsupported);
        assertFalse(s.save("alice", "张三"));
        assertEquals(0, s.cachedCount());
        s.prefetch(Arrays.asList("alice"));            // 不得抛异常
        assertEquals("alice", s.get("alice"));
        assertEquals("降级时目录内不得出现任何文件", 0, dir.listFiles().length);
    }

    /**
     * 判别性用例（FACE-JAVA-001）：{@code isSupported()} 已由 static 改为**实例方法**，必须读**本实例**
     * 的 {@code keyProvider} 字段 —— 使「平台支持判定」这一维度在测试中**可注入**。
     *
     * <p><b>为何是判别性的</b>：两个实例注入**相反**的替身，断言结果相反。若 {@code isSupported()} 仍读
     * **静态默认** {@code FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER}，则两次调用必返回**同一**值，
     * 「一真一假」不可能同时成立 ⇒ 至少一条断言红（且若默认实现在纯 JVM 下触达未 mock 的
     * {@code android.os.Build}，会直接抛异常 ⇒ 同样红）。
     */
    @Test
    public void isSupportedReflectsInjectedProviderPerInstance() throws Exception {
        // setUp 注入的 provider.isSupported() == true。
        PersonDisplayNameStore supported = newStore(tmp.newFolder());
        assertTrue("支持态实例的支持判定应为 true（读本实例 provider）", supported.isSupported());

        FeatureTemplateEncryptedStore.PlatformKeyProvider unsupported =
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return false;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        throw new AssertionError("不支持时不得取密钥");
                    }
                };
        PersonDisplayNameStore notSupported = new PersonDisplayNameStore(tmp.newFolder());
        notSupported.setKeyProviderForTesting(unsupported);
        assertFalse("不支持态实例的支持判定应为 false（读本实例 provider）", notSupported.isSupported());
    }

    /**
     * 判别性用例（FACE-JAVA-002）：构造 {@link PersonDisplayNameStore} 与查询平台能力**不得**触碰文件
     * 系统 —— 显示名目录只在**真正写入**时才惰性创建。
     *
     * <p><b>背景</b>：{@code MainActivity.displayNameStore()}（惰性双检锁 accessor）此前在**构造路径**里就
     * {@code mkdirs()} 出 {@code files/face_names/}；而 accessor 也是「平台不支持」早退路径取实例的入口
     * （{@code migrateLegacyDisplayNames} 只做 {@code displayNameStore().isSupported()} 这个纯能力查询），
     * 于是 API&lt;23 设备上凭空多出一个**永远用不到**的空目录。修法：把目录创建从 accessor 的构造路径移出，
     * 交还**写入**路径惰性完成。
     *
     * <p><b>为何是判别性的</b>：以**不存在的目录**构造实例（{@link TemporaryFolder} 下的子路径），只做能力
     * 查询与纯缓存读，断言目录仍 {@code !exists()}。若有人把 {@code mkdirs()} 重新塞回本类构造器、或塞进
     * {@link PersonDisplayNameStore#isSupported()} / {@code get()}，本断言立即红 —— 正对应「早退路径凭空建
     * 目录」的回归。（打红方式：在构造器里加 {@code if (!dir.exists()) dir.mkdirs();}。）
     *
     * <p><b>边界（如实声明）</b>：真正的早退路径在 {@code MainActivity}，其目录名取自
     * {@code getFilesDir()}（Android 运行时），且 {@code MainActivity} 是 {@code AppCompatActivity}
     * （纯 JVM 不可构造、本仓无 Robolectric）⇒ **JVM 单测无法逐字观测 MainActivity 那一半**。本用例钉住的是
     * 使该修法成立的**存储层不变量**（构造 + 能力查询无文件系统副作用），并配合下方「写入才建目录」用例
     * 证明惰性创建确实发生，而非「干脆不建目录」。
     */
    @Test
    public void constructionAndCapabilityQueryDoNotCreateDir() throws Exception {
        File absent = new File(tmp.newFolder(), "face_names_absent");
        assertFalse("前置：目标目录不应存在", absent.exists());

        PersonDisplayNameStore s = new PersonDisplayNameStore(absent);
        s.setKeyProviderForTesting(provider);

        assertTrue("能力查询应仍为真（读本实例 provider，与目录无关）", s.isSupported());
        assertEquals("未缓存应降级为 personId", "alice", s.get("alice"));
        assertFalse("构造 + 能力查询不得创建目录（FACE-JAVA-002 早退路径副作用）", absent.exists());
    }

    /**
     * 与上一条配对：目录应在**首次写入**时惰性出现（证明修法是「推迟创建」而非「不创建」）。
     *
     * <p>写入路径经 {@code FeatureTemplateEncryptedStore.writeEnvelopeAtomically} 在父目录缺失时才
     * {@code mkdirs} —— 故 save 成功后目录必已就位。
     */
    @Test
    public void saveLazilyCreatesDir() throws Exception {
        File absent = new File(tmp.newFolder(), "face_names_lazy");
        assertFalse("前置：目标目录不应存在", absent.exists());

        PersonDisplayNameStore s = new PersonDisplayNameStore(absent);
        s.setKeyProviderForTesting(provider);

        assertTrue(s.save("alice", "张三"));
        assertTrue("写入才是目录被创建的时机", absent.isDirectory());
        assertEquals("张三", s.get("alice"));
    }

    /**
     * 预取必须**重新解密**而非「命中即跳过」（A2 审计 Low 修复）：文件被带外改动后，下一次预取
     * 应反映磁盘现状，而不是一直显示陈旧名字。
     */
    @Test
    public void prefetchRefreshesChangedFileInsteadOfServingStaleCache() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore cache = newStore(dir);
        assertTrue(cache.save("alice", "旧名"));
        assertEquals("旧名", cache.get("alice"));

        // 带外改写：另一个实例写入新值（模拟外部改动 / 密钥轮换后的重写）。
        PersonDisplayNameStore writer = newStore(dir);
        assertTrue(writer.save("alice", "新名"));

        cache.prefetch(Collections.singletonList("alice"));
        assertEquals("预取应重新解密、反映磁盘现状", "新名", cache.get("alice"));
    }

    /** 磁盘字节不得含明文显示名。 */
    @Test
    public void persistedFileHasNoPlaintext() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
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
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "张三"));
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        assertTrue(f.isFile());

        UndeletableOps ops = new UndeletableOps();
        s.setFileOpsForTesting(ops);
        try {
            // 与 f.exists() 断言同向：删不掉就必须**如实报告 false**（返回值不得恒真）。
            assertFalse("删除静默失败（delete 返回 false）⇒ 不得谎报成功", s.remove("alice"));
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
        PersonDisplayNameStore s = newStore(dir);
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
     * 修法①后：{@code remove} 在**调用点**吞掉 {@link SecurityException}（best-effort 收尾）⇒
     * **不抛异常**，且缓存照清（清理放 {@code finally}），{@code get} 随之降级为 {@code personId}；
     * 同时删除**确实失败**（文件仍在，不得被谎报为已删）。
     *
     * <p>本用例取代此前的特征化测试 {@code removePropagatesSecurityExceptionAndSkipsCacheClear}
     * （它钉的是「异常传播且跳过缓存清理」的旧现状）。新契约即 {@code remove} 的 javadoc 承诺：
     * 「文件删除失败不影响缓存清理」—— 缓存里是可擦除的显示名明文，异常路径下**必须**擦除（PII）。
     */
    @Test
    public void removeSwallowsSecurityExceptionAndClearsCache() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "张三"));
        assertEquals(1, s.cachedCount());
        File f = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        assertTrue(f.isFile());

        s.setFileOpsForTesting(new ThrowingOps());
        try {
            // 修法①：调用点自行包装，**不再抛**异常（若抛，本用例即红）。
            // 同时证明：吞掉异常 ≠ 谎报成功 —— 删除确实被拒时必须返回 false。
            assertFalse("删除被拒（SecurityException）⇒ 不得谎报成功", s.remove("alice"));
        } finally {
            s.setFileOpsForTesting(null);
        }

        assertEquals("无论文件删除成败都必须清理缓存（finally）", 0, s.cachedCount());
        assertEquals("缓存已清 ⇒ get 降级为 personId", "alice", s.get("alice"));
        assertTrue("删除确实失败（SecurityException）⇒ 文件仍在（不得被谎报为已删）", f.exists());
    }

    /**
     * 修法①后：{@code removeOrphans} 中**单个孤儿删除抛异常不得中断整轮清理**，且抛异常项
     * **不计入返回值**（返回值始终是「确实被删除的文件数」）。
     *
     * <p>用 {@link ThrowOnceThenRealOps}（首次 delete 抛、其后真实删除）＋三个孤儿：无论遍历顺序，
     * 恰有一次 delete 调用抛异常 ⇒ 恰好一个孤儿删不掉、其余两个被删 ⇒ {@code removed == 2}。
     * 若实现未 catch（异常穿透循环），则 {@code removed} 会随遍历顺序退化为 0 或 1，且可能残留多个
     * 文件 ⇒ 本用例红。
     */
    @Test
    public void removeOrphansContinuesPastThrowingOrphan() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "A"));
        assertTrue(s.save("bob", "B"));
        assertTrue(s.save("carol", "C"));
        File fa = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        File fb = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob");
        File fc = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "carol");
        assertTrue(fa.isFile() && fb.isFile() && fc.isFile());

        s.setFileOpsForTesting(new ThrowOnceThenRealOps());
        int removed;
        try {
            removed = s.removeOrphans(Collections.emptySet());  // 三个都是孤儿
        } finally {
            s.setFileOpsForTesting(null);
        }

        assertEquals("抛异常项不计入、其余照删 ⇒ 恰好 2（异常未中断整轮）", 2, removed);
        int stillThere = 0;
        for (File f : new File[]{fa, fb, fc}) {
            if (f.exists()) stillThere++;
        }
        assertEquals("恰好一个孤儿删不掉（即抛异常那一项），其余两个确已删除", 1, stillThere);
    }

    /**
     * **混合场景**（三条既有用例各覆盖一半，本用例钉住三者**同时出现**的组合）：
     * {@code removeOrphans} 一轮内同时遇到「抛 {@code SecurityException}」「返回 false（删不掉）」
     * 「真实删除成功」三种结果。
     *
     * <p>期望：**仅真实删除成功的那一项**计入返回值 ⇒ {@code removed == 1}；另两项**原样残留**
     * （抛异常项被 catch 跳过、返回 false 项不计入，二者都不删）。用
     * {@link ThrowThenUndeletableThenRealOps} 按**基名**分流，故断言不依赖 {@code listFiles()} 顺序。
     *
     * <p><b>一旦 catch 缺失 / 计数口径漂移即红</b>：若实现未 catch（异常穿透整轮）⇒ 本用例抛
     * {@code SecurityException} 而红；若把「返回 false」也计入 ⇒ {@code removed == 2} 而红。
     */
    @Test
    public void removeOrphansMixedThrowUndeletableAndRealPinsExactCountAndResidue() throws Exception {
        File dir = tmp.newFolder();
        PersonDisplayNameStore s = newStore(dir);
        assertTrue(s.save("alice", "A"));
        assertTrue(s.save("bob", "B"));
        assertTrue(s.save("carol", "C"));
        File fa = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        File fb = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob");
        File fc = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "carol");
        assertTrue(fa.isFile() && fb.isFile() && fc.isFile());

        // alice → 恒抛；bob → 恒 false（删不掉）；carol → 真实删除。
        s.setFileOpsForTesting(new ThrowThenUndeletableThenRealOps(fa.getName(), fb.getName()));
        int removed;
        try {
            removed = s.removeOrphans(Collections.emptySet());  // 三个都是孤儿
        } finally {
            s.setFileOpsForTesting(null);
        }

        assertEquals("三态并存：仅「真实删除成功」的 carol 计入 ⇒ 恰为 1", 1, removed);
        assertTrue("alice（抛 SecurityException）应原样残留", fa.exists());
        assertTrue("bob（deleteWithBackup 返回 false）应原样残留", fb.exists());
        assertFalse("carol（真实删除）应已删除", fc.exists());
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
        PersonDisplayNameStore a = newStore(dirA);
        PersonDisplayNameStore b = newStore(dirB);
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
     *   <li>新增**任何**静态方法、恰一个「提及接缝类型」的参数（**不限名、不限返回类型**，含把
     *       {@code setFileOpsForTesting} 改名后重新加回，或写成 {@code Consumer<FileOps>} /
     *       {@code Map<String, FileOps>} / {@code FileOps[]} 等承载形态），**或** {@code set*} 名带
     *       **任一**接缝参数（**不限参数个数**）⇒ 方法断言红；</li>
     *   <li>新增**任意**静态接缝字段（含 {@code private static final FileOps fileOps}）——
     *       仅放行唯一合法常量 {@code DEFAULT_FILE_OPS} ⇒ 字段断言红。</li>
     * </ul>
     *
     * <p><b>本守卫的边界（如实声明，仅述本守卫自身收紧后的口径）</b>：类型判据已**递归下探泛型实参**
     * （见 {@link #mentionsSeamType(Type)}）——静态 {@code Consumer<FileOps>}、{@code Map<String, FileOps>}、
     * {@code Optional<FileOps>}、{@code FileOps[]}、{@code ? extends FileOps} 这类「以其它类型承载」的
     * 静态注入**同样命中**。仍**不覆盖**的只有：把接缝藏进**自定义 Holder 类内部**（例如静态字段
     * {@code Holder h}，而 {@code Holder} 类里才有 {@code FileOps} 字段）——递归只看该成员**自身**的
     * 类型签名，不做跨类型的成员下探。
     * <ul>
     *   <li>合法的 {@code deleteWithBackup(File, FileOps)}（静态、双参）不在拦截范围内——
     *       它是生产调用方显式传入 {@code ops} 的正规入口，非注入接缝；</li>
     *   <li><b>与本仓其余四处守卫的关系（<u>不得</u>写成「同口径」）</b>：**类型判据**一致（同为递归
     *       {@link #mentionsSeamType(Type)}）；**方法判据**为本条**严格覆盖**其余四处——
     *       判据①「{@code set*} + 任一接缝参数」与它们**同口径**，判据②「静态 + 恰一个接缝参数」
     *       比它们的「返回 {@code void} + 恰一个接缝参数」**更宽**（本条不限返回类型、不限方法名）。
     *       ⚠️ 因此**不得**照抄那两条来"统一口径"：那会**丢失**本条能抓的形态（非 {@code set} 名、
     *       非 {@code void} 返回、单接缝参数的静态方法），构成覆盖回归。</li>
     *   <li><b>修过的窄口（2026-09-26 二次审计）</b>：本条原先只判「恰一个接缝参数」⇒
     *       **多参静态 setter**（如 {@code static X setFileOpsForTesting(FileOps, boolean)}）会被静默放过；
     *       已按上面判据①补上。仍**不覆盖**的是「**非** {@code set} 名 + 多参 + 含接缝参数」
     *       （如 {@code static X apply(FileOps, int)}）——其余四处同样不覆盖，属共同边界、非本条净缺口。</li>
     *   <li><b>已知代价（不得隐瞒）</b>：递归判据会命中未来**合法**的静态 {@code Map}/{@code Optional}
     *       &lt;接缝类型&gt; 常量 ⇒ 会真红。那属**策略边界被推进**、需要白名单 + 评审，
     *       **不是假红，切勿为绕过它去削判据或给 {@code Map}/{@code Optional} 开豁免**。</li>
     * </ul>
     */
    @Test
    public void fileStoreUtilsExposesNoStaticInjectionSeam() {
        for (Method m : FileStoreUtils.class.getDeclaredMethods()) {
            boolean staticMethod = Modifier.isStatic(m.getModifiers());
            // 注入接缝形态①：`set*` 名 + **任一**带接缝类型的参数（**不限参数个数**、不限返回类型）
            //   —— 与其余四处守卫的判据①同口径。补它的原因（2026-09-26 二次审计）：本条原先只判
            //   「恰一个接缝参数」，于是「多参静态 setter」（如 `static X setFileOpsForTesting(FileOps, boolean)`）
            //   会被**静默放过**，而本仓教义明确把多参静态 setter 当作真实注入形态。
            boolean setNamedWithSeamParam = false;
            if (m.getName().startsWith("set")) {
                for (Type p : m.getGenericParameterTypes()) {
                    if (mentionsSeamType(p)) {
                        setNamedWithSeamParam = true;
                        break;
                    }
                }
            }
            // 注入接缝形态②：**任意名**静态方法，恰一个「提及接缝类型」的参数（类型判据递归，见 mentionsSeamType）。
            boolean takesSingleSeamParam = m.getParameterCount() == 1
                    && mentionsSeamType(m.getGenericParameterTypes()[0]);
            assertFalse("FileStoreUtils 不应再暴露静态注入入口（`set*` 名 + 任一接缝参数，或恰一个接缝参数类型的静态方法）："
                            + m.getName(),
                    staticMethod && (setNamedWithSeamParam || takesSingleSeamParam));
        }
        for (Field f : FileStoreUtils.class.getDeclaredFields()) {
            // 字段类型判据同样递归：以 Map/Optional/数组等承载的静态接缝字段也会命中。
            boolean staticSeamField = Modifier.isStatic(f.getModifiers())
                    && mentionsSeamType(f.getGenericType());
            if (!staticSeamField) {
                continue;
            }
            assertTrue("静态接缝字段必须是 final 不可变常量：" + f.getName(),
                    Modifier.isFinal(f.getModifiers()));
            assertTrue("静态接缝字段仅允许唯一合法常量 DEFAULT_FILE_OPS，不得新增注入字段："
                            + f.getName(),
                    "DEFAULT_FILE_OPS".equals(f.getName()));
        }
        // 编译期引用：证明 DEFAULT_FILE_OPS 仍对同包可见（生产调用方的默认实例）。
        assertTrue("DEFAULT_FILE_OPS 应对同包可见且非空", FileStoreUtils.DEFAULT_FILE_OPS != null);
    }

    /**
     * **结构守卫**（FACE-TEST-003 补齐）：{@code PersonDisplayNameStore} 上**不存在**静态注入入口 /
     * 静态接缝类型字段（本类的两个接缝 {@link FileStoreUtils.FileOps} 与 {@link
     * FeatureTemplateEncryptedStore.PlatformKeyProvider} 均为**实例字段**，注入只作用在本实例）。
     *
     * <p>与 {@code FeatureTemplateEncryptedStoreTest.featureTemplateEncryptedStoreExposesNoStaticInjectionSeam}
     * 同口径（本仓 JVM 内串行测试模型下，任何「一次注入、全局生效」都会跨类污染）。
     *
     * <p><b>判据（含 FACE-TEST-003 收紧）</b>：
     * <ul>
     *   <li><b>方法①（收紧）</b>：不存在「名为 {@code set*} 且带接缝类型参数」的**静态**方法 ——
     *       **不限返回类型**（堵「返回非 void 的静态 setter」）、**不限参数个数**（堵「多参静态 setter」）；</li>
     *   <li><b>方法②</b>：不存在「返回 {@code void} 且参数恰为一个接缝类型」的静态方法；</li>
     *   <li><b>字段③</b>：不存在任何静态接缝类型字段（默认值请引用
     *       {@code FileStoreUtils.DEFAULT_FILE_OPS} / {@code FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER}，
     *       静态字段会成为进程级可变注入入口）。</li>
     * </ul>
     *
     * <p><b>为何不误伤既有合法 API</b>：本类的合法注入方法 {@code setFileOpsForTesting} /
     * {@code setKeyProviderForTesting} 均为**实例**方法 ⇒ 方法判据（先过滤 {@code static}）不命中；
     * {@code isSupported()} 修复后亦为实例方法。
     *
     * <p><b>会红在哪些改法上</b>：
     * <ul>
     *   <li>把 {@code setFileOpsForTesting} 或 {@code setKeyProviderForTesting} 改为 {@code static}（或
     *       新增任一同名形态的静态 setter）⇒ 判据①红；</li>
     *   <li><b>新增返回非 void 的静态 setter</b>（如 {@code static FileOps setFileOpsForTesting(FileOps o)}）
     *       ⇒ 判据①红（旧形态判据会漏过）；</li>
     *   <li><b>新增多参静态 setter</b>（如 {@code static void setFileOpsForTesting(FileOps o, boolean reset)}）
     *       ⇒ 判据①红（旧形态判据会漏过）；</li>
     *   <li>新增**任意**静态接缝类型字段（如 {@code private static FileOps fileOps}）⇒ 字段判据红。</li>
     * </ul>
     *
     * <p><b>边界（如实声明，收紧后）</b>：类型判据已**递归下探泛型实参**（见 {@link #mentionsSeamType(Type)}）
     * —— {@code Consumer<FileOps>} / {@code Map<String, FileOps>} / {@code Optional<FileOps>} /
     * {@code FileOps[]} / {@code ? extends FileOps} 等「以其它类型承载」的静态注入**已被覆盖**；
     * **非 setter 命名**承载（{@code static void install(FileOps)}）由判据②覆盖。仍**不覆盖**的只有：
     * 把接缝藏进**自定义 Holder 类内部**（如 {@code static final Holder H} 而 {@code Holder} 内才有
     * {@code FileOps} 字段）—— 判据只作用于成员自身声明的（泛型）类型，**不反射进任意 Holder 类的内部**。
     *
     * <p><b>净缺口（如实登记，本批收紧判据的代价）</b>：存在一类形态**旧判据覆盖、新判据不覆盖** ——
     * 「<b>静态</b>、<b>非 {@code set*} 命名</b>、<b>返回非 void</b>、<b>恰一个接缝类型参数</b>」的注入方法
     * （如 {@code static FileOps installForTesting(FileOps)}）：**旧**形态判据（任意静态、参数恰为一个接缝
     * 类型）会判红；**新**判据①需 {@code set} 前缀（不命中）、②需 {@code void} 返回（不命中）⇒ 静默放过。
     * **为何接受**：与其余三处同类守卫采用同一套两条口径 —— {@code FeatureTemplateEncryptedStoreTest.
     * featureTemplateEncryptedStoreExposesNoStaticInjectionSeam}、{@code FaceModelManagerTest.
     * faceModelManagerExposesNoStaticFileOpsSeam}、{@code AndroidEnrollmentRepositoryTest.
     * androidEnrollmentRepositoryExposesNoStaticInjectionSeam}（均为 ①{@code set*}+接缝参数 /
     * ②{@code void}+单接缝参数）。补回该形态需**单独评估放宽面**（放宽会引入新的误伤面），故此处只登记、
     * **不放宽判据**。
     *
     * <p><b>已知代价（如实声明，不得隐瞒）</b>：递归判据会命中一批**合法**的静态常量 —— 若将来合法
     * 新增 {@code private static final Map<String, FileOps> DEFAULTS}，本条**会真红**。那不是假红，是
     * **策略边界被推进**，需显式白名单 + 评审。⚠️ **不要**为了绕过它而放行 {@code Map}/{@code Optional}
     * （那等于把判据换回空气），也不要直接删判据。
     */
    @Test
    public void personDisplayNameStoreExposesNoStaticInjectionSeam() {
        for (Method m : PersonDisplayNameStore.class.getDeclaredMethods()) {
            if (!Modifier.isStatic(m.getModifiers())) {
                continue;   // 合法注入方法在本类均为**实例**方法，故只查静态
            }
            boolean hasSeamParam = false;
            for (Type p : m.getGenericParameterTypes()) {
                if (mentionsSeamType(p)) {
                    hasSeamParam = true;
                    break;
                }
            }
            assertFalse("不应暴露静态注入入口（名为 set* 且带接缝类型参数的静态方法，"
                            + "不限返回类型/参数个数）：" + m.getName(),
                    m.getName().startsWith("set") && hasSeamParam);
            boolean voidSingleSeam = m.getReturnType() == void.class
                    && m.getParameterCount() == 1
                    && mentionsSeamType(m.getGenericParameterTypes()[0]);
            assertFalse("不应暴露静态注入入口（返回 void 且参数恰为一个接缝类型）：" + m.getName(),
                    voidSingleSeam);
        }
        for (Field f : PersonDisplayNameStore.class.getDeclaredFields()) {
            assertFalse("PersonDisplayNameStore 不应持有任何静态接缝类型字段（FileOps / PlatformKeyProvider）"
                            + "——默认值请引用 FileStoreUtils.DEFAULT_FILE_OPS / "
                            + "FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER，静态字段会成为进程级可变注入入口："
                            + f.getName(),
                    Modifier.isStatic(f.getModifiers()) && mentionsSeamType(f.getGenericType()));
        }
    }

    /**
     * 类型（含泛型）是否**提及**任一测试接缝（{@link FileStoreUtils.FileOps} /
     * {@link FeatureTemplateEncryptedStore.PlatformKeyProvider}）。
     *
     * <p>递归下探：{@code Class}（数组取组件类型）、{@link ParameterizedType}（递归每个实参）、
     * {@link GenericArrayType}（递归组件类型）、{@link WildcardType}（递归上界）。故
     * {@code Consumer<FileOps>} / {@code Map<String, FileOps>} / {@code Optional<FileOps>} /
     * {@code FileOps[]} / {@code ? extends FileOps} 均命中。
     *
     * <p><b>为何不下探 Holder 类内部</b>：本判据只作用于成员**自身声明的（泛型）类型**，不反射进任意
     * 自定义 Holder 类内部 —— 那是无界的，会把「任何持有接缝的类型」都算成接缝，误伤面失控。
     *
     * <p><b>已知代价（如实声明）</b>：递归会命中一批**合法**的静态常量（如 {@code private static final
     * Map<String, FileOps> DEFAULTS}）—— 那不是假红，而是**策略边界被推进**，需显式白名单 + 评审。
     * 不要为了绕过它而放行 {@code Map}/{@code Optional}（那等于把判据换回空气）。
     */
    private static boolean mentionsSeamType(Type type) {
        if (type instanceof Class<?>) {
            Class<?> c = (Class<?>) type;
            if (c.isArray()) {
                return mentionsSeamType(c.getComponentType());
            }
            return FileStoreUtils.FileOps.class.isAssignableFrom(c)
                    || FeatureTemplateEncryptedStore.PlatformKeyProvider.class.isAssignableFrom(c);
        }
        if (type instanceof ParameterizedType) {
            for (Type arg : ((ParameterizedType) type).getActualTypeArguments()) {
                if (mentionsSeamType(arg)) {
                    return true;
                }
            }
            return false;
        }
        if (type instanceof GenericArrayType) {
            return mentionsSeamType(((GenericArrayType) type).getGenericComponentType());
        }
        if (type instanceof WildcardType) {
            for (Type bound : ((WildcardType) type).getUpperBounds()) {
                if (mentionsSeamType(bound)) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }
}
