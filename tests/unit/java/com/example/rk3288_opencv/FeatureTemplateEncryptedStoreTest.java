package com.example.rk3288_opencv;

import org.junit.Test;
import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

import java.nio.charset.StandardCharsets;

public class FeatureTemplateEncryptedStoreTest {

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
