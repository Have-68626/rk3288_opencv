package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.SecureRandom;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

public class FeatureTemplateEncryptedStoreTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    /**
     * 供各用例显式传入的密钥提供者替身（内存 JCE 密钥）。此前是进程级静态注入
     * （{@code setPlatformKeyProviderForTesting}），改造后由**方法形参**逐调用点传递，故持为字段。
     */
    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
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

    // 无 @After（改造前它用于复位进程级静态 provider）：provider 现由形参逐调用点传入，
    // 本测试类不再改写任何静态状态 ⇒ **无静态状态可复位**，故该 @After 已删除。

    // ── 任务 B：显示名加密存储 ────────────────────────────────────────────────
    // 注意：替身只实现 getOrCreateKeyV1，而 getOrCreateNameKey 默认复用 v1 → 这些用例在**不改动
    // 任何既有替身**的前提下就能跑真实 AES-GCM（这正是把 name 密钥做成 default 方法的目的）。

    /** 写入后读回，明文应逐字节一致。 */
    @Test
    public void nameRoundTripsThroughEncryptedFile() throws Exception {
        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-001");

        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.writeNameString(provider, dst, "p-001", "张三").code);
        assertTrue("显示名文件应已落盘", dst.isFile());

        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.readNameString(provider, dst, "p-001", false);
        assertEquals(FeatureTemplateEncryptedStore.Code.OK, rr.code);
        assertNotNull(rr.plaintext);
        assertEquals("张三", new String(rr.plaintext, StandardCharsets.UTF_8));
    }

    /** 落在磁盘上的字节不得含明文显示名（加密而非编码）。 */
    @Test
    public void nameFileOnDiskDoesNotContainPlaintext() throws Exception {
        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-002");
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.writeNameString(
                        provider, dst, "p-002", "UniquelyNamedPerson").code);

        byte[] raw = java.nio.file.Files.readAllBytes(dst.toPath());
        String asText = new String(raw, StandardCharsets.UTF_8);
        assertFalse("磁盘字节不得含显示名明文", asText.contains("UniquelyNamedPerson"));
    }

    /** 空显示名与超长显示名一律拒绝（拒绝而非截断）。 */
    @Test
    public void nameRejectsEmptyAndOversized() throws Exception {
        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-003");

        assertEquals(FeatureTemplateEncryptedStore.Code.BAD_FORMAT,
                FeatureTemplateEncryptedStore.writeNameString(provider, dst, "p-003", "").code);

        StringBuilder big = new StringBuilder();
        for (int i = 0; i < 257; i++) big.append('x');   // 257 字节 > MAX_NAME_BYTES(256)
        assertEquals(FeatureTemplateEncryptedStore.Code.BAD_FORMAT,
                FeatureTemplateEncryptedStore.writeNameString(provider, dst, "p-003", big.toString()).code);
        assertFalse("拒绝的写入不得留下文件", dst.exists());
    }

    /** 域分离：模板文件当显示名读 → AAD 不匹配 → 拒绝（非 OK），反之亦然。 */
    @Test
    public void nameAndTemplateDomainsAreNotInterchangeable() throws Exception {
        File dir = tmp.newFolder();

        File tpl = new File(dir, "u_x.bin");
        byte[] payload = new byte[]{1, 2, 3};
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(provider, null, tpl, "same-id", payload, 1, 1).code);
        assertEquals("模板文件不得被当作显示名读出",
                FeatureTemplateEncryptedStore.Code.CORRUPTED,
                FeatureTemplateEncryptedStore.readNameString(provider, tpl, "same-id", false).code);

        File nameFile = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "same-id");
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.writeNameString(provider, nameFile, "same-id", "n").code);
        assertFalse("显示名文件不得被当作模板读出",
                FeatureTemplateEncryptedStore.read(provider, null, nameFile, "same-id", 1, 1, false).code
                        == FeatureTemplateEncryptedStore.Code.OK);
    }

    /** 平台不支持（API<23）时返回 UNSUPPORTED_PLATFORM 而非抛异常。 */
    @Test
    public void nameUnsupportedPlatformReturnsCodeNotThrow() throws Exception {
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
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-004");
        assertEquals(FeatureTemplateEncryptedStore.Code.UNSUPPORTED_PLATFORM,
                FeatureTemplateEncryptedStore.writeNameString(unsupported, dst, "p-004", "n").code);
        assertEquals(FeatureTemplateEncryptedStore.Code.UNSUPPORTED_PLATFORM,
                FeatureTemplateEncryptedStore.readNameString(unsupported, dst, "p-004", false).code);
    }

    /** 密钥不可用（失效/取用异常）→ KEYSTORE_ERROR，**绝不回落到明文**。 */
    @Test
    public void nameKeystoreErrorNeverFallsBackToPlaintext() throws Exception {
        FeatureTemplateEncryptedStore.PlatformKeyProvider broken =
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return true;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        return null;
                    }

                    @Override
                    public SecretKey getOrCreateNameKey() {
                        throw new IllegalStateException("key permanently invalidated");
                    }
                };

        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-005");
        assertEquals(FeatureTemplateEncryptedStore.Code.KEYSTORE_ERROR,
                FeatureTemplateEncryptedStore.writeNameString(broken, dst, "p-005", "n").code);
        assertFalse("密钥不可用时绝不落盘明文", dst.exists());
    }

    /** 文件名由 personId 单向派生：前缀区分、不含 personId 明文、不同人不同名。 */
    @Test
    public void nameFileNameIsOneWayAndDistinct() throws Exception {
        File dir = tmp.newFolder();
        File a = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "alice");
        File b = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "bob");
        assertTrue(a.getName().startsWith("n_"));
        assertTrue(a.getName().endsWith(".enc"));
        assertFalse("文件名不得含 personId 明文", a.getName().contains("alice"));
        assertFalse("不同人员不得同文件", a.getName().equals(b.getName()));
    }

    /**
     * 源码结构守卫（A2 审计 H1）：加密路径**不得**向 {@code Cipher.init} 传调用方 IV。
     *
     * <p>密钥以 {@code setRandomizedEncryptionRequired(true)} 生成，AndroidKeyStore 在这种情况下
     * 会**拒绝调用方提供的 IV**（加密时抛
     * {@code InvalidAlgorithmParameterException: Caller-provided IV not permitted}），必须让 Keystore
     * 生成、再经 {@code getIV()} 回读。**JVM 的 JCE 替身没有这条限制**，故单元测试无法覆盖该约束
     * ——这是本批唯一只能用结构锁定的契约，模板与显示名两条写入路径都要遵守。
     *
     * <p>历史：修复前两条路径都是「自造 IV 传给 init」，在 API≥23 真机上会**恒失败**（本项目尚无
     * 实机留档，故一直未被发现）。
     *
     * <p><b>局限（A2-Low6，如实声明）</b>：本守卫是**字面匹配**——换行、空格或包装成辅助方法
     *（如 {@code init(mode, key, params)}）都能绕过。它**不是**语义保证，只是把最容易复发的写法钉住。
     */
    @Test
    public void encryptionNeverPassesCallerProvidedIv() throws IOException {
        File src = new File(findRepoRoot(),
                "src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java");
        assertTrue(src.isFile());
        String text = new String(Files.readAllBytes(src.toPath()), StandardCharsets.UTF_8);

        assertFalse("加密不得向 Cipher.init 传 caller IV（真机 KeyStore 会拒绝）",
                text.contains("Cipher.ENCRYPT_MODE, key, "));
        assertTrue("IV 必须由 Keystore 生成并回读",
                text.contains("byte[] iv = requireIv(cipher);"));
        assertTrue("应保留随机化加密要求（不靠关掉该要求绕过）",
                text.contains("setRandomizedEncryptionRequired(true)"));
    }

    /**
     * 读路径优先级（A1 审计 P3-2 沉淀为回归用例）：**密钥不可用且文件不存在时，必须报
     * {@code NOT_FOUND} 而非 {@code KEYSTORE_ERROR}**。
     *
     * <p>这是提取 {@code readEnvelope} 时刻意保住的顺序（先判文件存在性 → 再取密钥）。此前仅由
     * 审计的临时探针验证过，现固化下来，防止日后重构悄然改变优先级。
     */
    @Test
    public void missingFileBeatsKeystoreErrorOnRead() throws Exception {
        FeatureTemplateEncryptedStore.PlatformKeyProvider keystoreDown =
                new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
                    @Override
                    public boolean isSupported() {
                        return true;
                    }

                    @Override
                    public SecretKey getOrCreateKeyV1() {
                        throw new IllegalStateException("keystore down");
                    }

                    @Override
                    public SecretKey getOrCreateNameKey() {
                        throw new IllegalStateException("keystore down");
                    }
                };

        File missing = new File(tmp.newFolder(), "nope.bin");
        assertEquals("模板读路径：文件缺失优先于密钥错误",
                FeatureTemplateEncryptedStore.Code.NOT_FOUND,
                FeatureTemplateEncryptedStore.read(keystoreDown, null, missing, "u", 1, 1, false).code);
        assertEquals("显示名读路径：同上",
                FeatureTemplateEncryptedStore.Code.NOT_FOUND,
                FeatureTemplateEncryptedStore.readNameString(keystoreDown, missing, "u", false).code);
    }

    /**
     * 提取边界差异修复（A3 确认 α2）：`userId == null` 时必须返回**错误码**而不是抛 NPE。
     *
     * <p>提取 {@code readEnvelope} 时曾把 {@code buildAad(...)} 作为实参提前求值，使 null userId
     * 抛 NPE 穿透；改为 {@code AadSupplier} 延迟求值后恢复「在 try 内求值 → 归入错误映射」的原语义。
     */
    @Test
    public void nullUserIdYieldsErrorCodeInsteadOfNpe() throws Exception {
        File dst = new File(tmp.newFolder(), "u_x.bin");
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(provider, null, dst, "u1", new byte[]{1, 2}, 1, 1).code);

        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.read(provider, null, dst, null, 1, 1, false);
        assertNotNull("不得抛 NPE，应返回 ReadResult", rr);
        assertTrue("null userId 应归为错误码而非成功", rr.code != FeatureTemplateEncryptedStore.Code.OK);
    }

    /**
     * A4：读路径的**阶段 1 恢复基线**——主文件缺失但 {@code .bak} 存在时，应先把备份还原再读，
     * 而不是直接判 {@code NOT_FOUND}（`persistRegistry`/`persistActive` 早有这一步，唯独本类此前缺）。
     */
    @Test
    public void readRecoversFromBackupWhenMainFileIsMissing() throws Exception {
        File dir = tmp.newFolder();
        File dst = new File(dir, "u_x.bin");
        byte[] payload = new byte[]{7, 8, 9};
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(provider, null, dst, "u1", payload, 1, 1).code);

        // 造出「dst 缺失 + dst.bak 存在」的崩溃现场（写入在 dst→bak 之后、tmp→dst 之前中断）。
        File bak = new File(dir, "u_x.bin.bak");
        assertTrue("前置：应能移开主文件", dst.renameTo(bak));

        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.read(provider, null, dst, "u1", 1, 1, false);
        assertEquals("应从 .bak 恢复后读出，而非判 NOT_FOUND",
                FeatureTemplateEncryptedStore.Code.OK, rr.code);
        assertArrayEquals(payload, rr.plaintext);
        assertTrue("恢复后主文件应就位", dst.isFile());
        assertFalse("备份应已被消费", bak.exists());
    }

    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, "src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java").isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录");
    }

    // 内存模板清零：readAndConsume 回调返回后，同一明文数组必须被就地清零。
    @Test
    public void readAndConsumeZeroesPlaintextAfterCallback() throws Exception {
        File dir = tmp.newFolder();
        File dst = new File(dir, "u_test.bin");
        byte[] template = new byte[32];
        new SecureRandom(new byte[]{42}).nextBytes(template);

        // ctx 在 write/read 中不参与加解密，JVM 单元测试可传 null。
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(provider, null, dst, "user-1", template, 3, 1).code);

        final byte[][] seen = new byte[1][];
        FeatureTemplateEncryptedStore.ReadResult rr = FeatureTemplateEncryptedStore.readAndConsume(
                provider, null, dst, "user-1", 3, 1, true,
                new FeatureTemplateEncryptedStore.PlaintextConsumer() {
                    @Override
                    public void accept(byte[] plaintextTemplate) {
                        seen[0] = plaintextTemplate;
                        assertArrayEquals("回调内应拿到原始模板", template, plaintextTemplate);
                    }
                });
        assertEquals(FeatureTemplateEncryptedStore.Code.OK, rr.code);
        assertNull("readAndConsume 不向调用方返回明文", rr.plaintext);
        assertNotNull(seen[0]);
        for (byte b : seen[0]) {
            assertEquals("回调返回后明文必须被清零", 0, b);
        }
    }

    @Test
    public void testSha256Determinism() {
        byte[] input1 = "test_user_123".getBytes(StandardCharsets.UTF_8);
        byte[] input2 = "test_user_456".getBytes(StandardCharsets.UTF_8);

        byte[] hash1a = FeatureTemplateEncryptedStore.sha256(input1);
        byte[] hash1b = FeatureTemplateEncryptedStore.sha256(input1);
        byte[] hash2 = FeatureTemplateEncryptedStore.sha256(input2);

        // Ensure hash is not null and has correct SHA-256 length (32 bytes)
        assertNotNull(hash1a);
        assertEquals(32, hash1a.length);

        assertNotNull(hash2);
        assertEquals(32, hash2.length);

        // Ensure same input produces same hash
        assertArrayEquals("Deterministic hashing failed for identical inputs", hash1a, hash1b);

        // Test against a known expected SHA-256 hash output
        // SHA-256("test") = 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
        byte[] expectedHashForTest = new byte[] {
            (byte)0x9f, (byte)0x86, (byte)0xd0, (byte)0x81, (byte)0x88, (byte)0x4c, (byte)0x7d, (byte)0x65,
            (byte)0x9a, (byte)0x2f, (byte)0xea, (byte)0xa0, (byte)0xc5, (byte)0x5a, (byte)0xd0, (byte)0x15,
            (byte)0xa3, (byte)0xbf, (byte)0x4f, (byte)0x1b, (byte)0x2b, (byte)0x0b, (byte)0x82, (byte)0x2c,
            (byte)0xd1, (byte)0x5d, (byte)0x6c, (byte)0x15, (byte)0xb0, (byte)0xf0, (byte)0x0a, (byte)0x08
        };

        byte[] actualHashForTest = FeatureTemplateEncryptedStore.sha256("test".getBytes(StandardCharsets.UTF_8));
        assertArrayEquals("Hashing 'test' did not match known SHA-256 output", expectedHashForTest, actualHashForTest);
    }

    // ── 静态接缝结构守卫（取代此前的「进程级静态 provider + @After 复位」约定） ──────────

    /**
     * **结构守卫**：{@code FeatureTemplateEncryptedStore} 上**不再存在**静态注入入口 / 可变静态
     * provider 字段（本轮把平台密钥提供者从静态改为方法形参）。
     *
     * <p><b>为什么不用「任意名、参数恰为一个 {@code PlatformKeyProvider} 的静态方法即判为注入入口」
     * 这条原判据</b>：改造后 {@code isPlatformSupported(PlatformKeyProvider)} **本身就是单参静态方法**，
     * 会**误命中**、使守卫恒红 —— 原判据在本类已失效。故改用**明确判据**：
     * <ul>
     *   <li><b>方法维度</b>：不存在「返回 {@code void} 且参数恰为一个 {@code PlatformKeyProvider}」的
     *       静态方法（这正是静态注入入口的形态）；且**不得**存在名为
     *       {@code setPlatformKeyProviderForTesting} 的方法（不限静态）。</li>
     *   <li><b>字段维度</b>：静态 {@code PlatformKeyProvider} 字段必须 {@code final}，且**唯一**允许的
     *       常量名是 {@link FeatureTemplateEncryptedStore#DEFAULT_KEY_PROVIDER}（与
     *       {@code DEFAULT_FILE_OPS} 完全同构）。</li>
     * </ul>
     *
     * <p><b>双向</b>：既断言「无静态注入入口 / 无可变静态字段」，也断言 {@code DEFAULT_KEY_PROVIDER}
     * 仍对同包可见且非空（生产调用方的默认实例）。
     *
     * <p><b>打红方式</b>（改任一处即红）：
     * <ul>
     *   <li>把静态注入方法 {@code setPlatformKeyProviderForTesting} 加回（或新增任一同形态静态方法）
     *       ⇒ 方法断言红；</li>
     *   <li>新增**任意**可变静态 {@code PlatformKeyProvider} 字段（如 {@code private static volatile
     *       PlatformKeyProvider platformKeyProvider}）—— 仅放行唯一合法常量 {@code DEFAULT_KEY_PROVIDER}
     *       ⇒ 字段断言红。</li>
     * </ul>
     *
     * <p><b>边界（如实声明，与 {@code fileStoreUtilsExposesNoStaticInjectionSeam} 同）</b>：只按**形态**
     * 判据。若有人以**其它类型**承载静态注入（如静态 {@code Consumer<PlatformKeyProvider>}、
     * 静态 {@code Map} 缓存替身），本条**不覆盖**。
     */
    @Test
    public void featureTemplateEncryptedStoreExposesNoStaticInjectionSeam() {
        for (Method m : FeatureTemplateEncryptedStore.class.getDeclaredMethods()) {
            boolean voidSingleProvider = Modifier.isStatic(m.getModifiers())
                    && m.getReturnType() == void.class
                    && m.getParameterCount() == 1
                    && FeatureTemplateEncryptedStore.PlatformKeyProvider.class
                            .equals(m.getParameterTypes()[0]);
            assertFalse("不应暴露静态注入入口（返回 void 且参数恰为一个 PlatformKeyProvider 的静态方法）："
                    + m.getName(), voidSingleProvider);
            assertFalse("不应再存在任何 setPlatformKeyProviderForTesting 方法（注入点已下移到调用方）："
                    + m.getName(), "setPlatformKeyProviderForTesting".equals(m.getName()));
        }
        for (Field f : FeatureTemplateEncryptedStore.class.getDeclaredFields()) {
            boolean staticProvider = Modifier.isStatic(f.getModifiers())
                    && FeatureTemplateEncryptedStore.PlatformKeyProvider.class.isAssignableFrom(f.getType());
            if (!staticProvider) {
                continue;
            }
            assertTrue("静态 PlatformKeyProvider 字段必须是 final 不可变常量：" + f.getName(),
                    Modifier.isFinal(f.getModifiers()));
            assertTrue("静态 PlatformKeyProvider 字段仅允许唯一合法常量 DEFAULT_KEY_PROVIDER，"
                            + "不得新增可变注入字段：" + f.getName(),
                    "DEFAULT_KEY_PROVIDER".equals(f.getName()));
        }
        // 编译期引用：证明 DEFAULT_KEY_PROVIDER 仍对同包可见且非空（生产调用方的默认实例）。
        assertNotNull("DEFAULT_KEY_PROVIDER 应对同包可见且非空",
                FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER);
    }
}
