package com.example.rk3288_opencv;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
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
}
