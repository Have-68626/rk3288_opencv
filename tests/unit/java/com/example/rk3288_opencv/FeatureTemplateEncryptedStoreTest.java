package com.example.rk3288_opencv;

import org.junit.After;
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
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

public class FeatureTemplateEncryptedStoreTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    @Before
    public void setUp() throws Exception {
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

    // ── 任务 B：显示名加密存储 ────────────────────────────────────────────────
    // 注意：替身只实现 getOrCreateKeyV1，而 getOrCreateNameKey 默认复用 v1 → 这些用例在**不改动
    // 任何既有替身**的前提下就能跑真实 AES-GCM（这正是把 name 密钥做成 default 方法的目的）。

    /** 写入后读回，明文应逐字节一致。 */
    @Test
    public void nameRoundTripsThroughEncryptedFile() throws Exception {
        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-001");

        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-001", "张三").code);
        assertTrue("显示名文件应已落盘", dst.isFile());

        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.readNameString(dst, "p-001", false);
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
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-002", "UniquelyNamedPerson").code);

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
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-003", "").code);

        StringBuilder big = new StringBuilder();
        for (int i = 0; i < 257; i++) big.append('x');   // 257 字节 > MAX_NAME_BYTES(256)
        assertEquals(FeatureTemplateEncryptedStore.Code.BAD_FORMAT,
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-003", big.toString()).code);
        assertFalse("拒绝的写入不得留下文件", dst.exists());
    }

    /** 域分离：模板文件当显示名读 → AAD 不匹配 → 拒绝（非 OK），反之亦然。 */
    @Test
    public void nameAndTemplateDomainsAreNotInterchangeable() throws Exception {
        File dir = tmp.newFolder();

        File tpl = new File(dir, "u_x.bin");
        byte[] payload = new byte[]{1, 2, 3};
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(null, tpl, "same-id", payload, 1, 1).code);
        assertEquals("模板文件不得被当作显示名读出",
                FeatureTemplateEncryptedStore.Code.CORRUPTED,
                FeatureTemplateEncryptedStore.readNameString(tpl, "same-id", false).code);

        File nameFile = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "same-id");
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.writeNameString(nameFile, "same-id", "n").code);
        assertFalse("显示名文件不得被当作模板读出",
                FeatureTemplateEncryptedStore.read(null, nameFile, "same-id", 1, 1, false).code
                        == FeatureTemplateEncryptedStore.Code.OK);
    }

    /** 平台不支持（API<23）时返回 UNSUPPORTED_PLATFORM 而非抛异常。 */
    @Test
    public void nameUnsupportedPlatformReturnsCodeNotThrow() throws Exception {
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
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-004");
        assertEquals(FeatureTemplateEncryptedStore.Code.UNSUPPORTED_PLATFORM,
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-004", "n").code);
        assertEquals(FeatureTemplateEncryptedStore.Code.UNSUPPORTED_PLATFORM,
                FeatureTemplateEncryptedStore.readNameString(dst, "p-004", false).code);
    }

    /** 密钥不可用（失效/取用异常）→ KEYSTORE_ERROR，**绝不回落到明文**。 */
    @Test
    public void nameKeystoreErrorNeverFallsBackToPlaintext() throws Exception {
        FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting(
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
                });

        File dir = tmp.newFolder();
        File dst = FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, "p-005");
        assertEquals(FeatureTemplateEncryptedStore.Code.KEYSTORE_ERROR,
                FeatureTemplateEncryptedStore.writeNameString(dst, "p-005", "n").code);
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

    // 内存模板清零：readAndConsume 回调返回后，同一明文数组必须被就地清零。
    @Test
    public void readAndConsumeZeroesPlaintextAfterCallback() throws Exception {
        File dir = tmp.newFolder();
        File dst = new File(dir, "u_test.bin");
        byte[] template = new byte[32];
        new SecureRandom(new byte[]{42}).nextBytes(template);

        // ctx 在 write/read 中不参与加解密，JVM 单元测试可传 null。
        assertEquals(FeatureTemplateEncryptedStore.Code.OK,
                FeatureTemplateEncryptedStore.write(null, dst, "user-1", template, 3, 1).code);

        final byte[][] seen = new byte[1][];
        FeatureTemplateEncryptedStore.ReadResult rr = FeatureTemplateEncryptedStore.readAndConsume(
                null, dst, "user-1", 3, 1, true,
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
}
