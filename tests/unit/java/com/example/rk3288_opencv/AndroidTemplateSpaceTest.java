package com.example.rk3288_opencv;

import java.io.File;
import java.nio.file.Files;
import java.util.Collections;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import static org.junit.Assert.*;

public class AndroidTemplateSpaceTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    @Test public void spacesIsolateTemplatesAndPrecisionVariantsShareOneSpace() throws Exception {
        File root = tmp.newFolder();
        File legacy = new File(root, "legacy_probe.bin");
        Files.write(legacy.toPath(), new byte[]{1, 2, 3});
        String a = String.join("", Collections.nCopies(64, "a"));
        String b = String.join("", Collections.nCopies(64, "b"));
        KeyGenerator generator = KeyGenerator.getInstance("AES");
        generator.init(128);
        SecretKey key = generator.generateKey();
        FeatureTemplateEncryptedStore.PlatformKeyProvider provider = new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            public boolean isSupported() { return true; }
            public SecretKey getOrCreateKeyV1() { return key; }
        };
        AndroidEnrollmentRepository fp32 = repository(root, a, provider);
        AndroidEnrollmentRepository quantized = repository(root, a, provider);
        AndroidEnrollmentRepository other = repository(root, b, provider);
        assertTrue(fp32.add("synthetic_person", new byte[2048], 1, 1).isOk());
        assertEquals(1, quantized.list().entries.size());
        assertEquals(0, other.list().entries.size());
        assertEquals(root, AndroidEnrollmentRepository.templateSpaceDir(root, ""));
        assertTrue("新空间不搬动旧库", legacy.isFile());
        assertEquals(new File(new File(root, "spaces"), a), fp32.scopeDir());
    }

    @Test public void unsafeSpaceCannotEscapeGalleryRoot() throws Exception {
        File root = tmp.newFolder();
        for (String invalid : new String[]{"../legacy", String.join("", Collections.nCopies(64, "A")), "abc"}) {
            try {
                AndroidEnrollmentRepository.templateSpaceDir(root, invalid);
                fail("必须拒绝非法空间");
            } catch (IllegalArgumentException expected) {}
        }
    }

    private AndroidEnrollmentRepository repository(File root, String space,
            FeatureTemplateEncryptedStore.PlatformKeyProvider provider) {
        AndroidEnrollmentRepository repo = new AndroidEnrollmentRepository(null,
                AndroidEnrollmentRepository.templateSpaceDir(root, space));
        repo.setKeyProviderForTesting(provider);
        return repo;
    }
}
