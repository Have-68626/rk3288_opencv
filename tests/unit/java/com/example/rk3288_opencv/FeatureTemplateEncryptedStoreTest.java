package com.example.rk3288_opencv;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;

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
