package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * 显示名的加密持久化 + 内存缓存（任务 B）。
 *
 * <p><b>为什么必须缓存</b>：{@link FeatureTemplateEncryptedStore#readNameString} 要读盘并做
 * KeyStore 解密，**不能上 UI 线程**；而 UI 侧的展示名查询是**同步**调用（识别结果、人员列表、
 * 删除确认文案等 4 处）。故本类维护一份内存映射：由后台线程预取解密后填充，UI 线程**只读缓存**。
 *
 * <p><b>未命中的降级</b>：尚未预取 / 无该人员 / 密钥不可用 / 文件损坏，{@link #get} 一律返回
 * {@code personId} 本身。这与「密钥不可用时不回落明文」的安全取向一致，且调用方无需区分这些情形
 * ——既有的 4 个调用点本来就以「显示名等于 userId 则只显示 userId」的方式处理。
 *
 * <p><b>线程约定</b>：只有 {@link #get} 可在 UI 线程调用；其余方法都会做文件 IO 与加解密，
 * **必须在后台线程调用**。
 */
final class PersonDisplayNameStore {

    @NonNull
    private final File dir;
    @NonNull
    private final ConcurrentHashMap<String, String> cache = new ConcurrentHashMap<>();

    PersonDisplayNameStore(@NonNull File dir) {
        this.dir = dir;
    }

    /**
     * UI 线程安全：纯读内存映射，**无 IO、无加解密、无锁竞争**（ConcurrentHashMap）。
     *
     * @return 解密后的显示名；未命中或为空时返回 {@code personId} 本身
     */
    @NonNull
    String get(@NonNull String personId) {
        String v = cache.get(personId);
        return (v == null || v.isEmpty()) ? personId : v;
    }

    /** 当前已缓存条数（诊断与测试用）。 */
    int cachedCount() {
        return cache.size();
    }

    /**
     * 后台线程：**绕过缓存**直接从磁盘解密读回。
     *
     * <p>用于一次性迁移后的「验证密文可读」——必须先确认新写入的密文能读回，才允许删除旧明文
     * （绝不先删后验）。也用于诊断与测试。
     *
     * @return 解密后的显示名；不可读（无文件 / 密钥不可用 / 损坏）返回 null
     */
    @Nullable
    String readFromDisk(@NonNull String personId) {
        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.readNameString(
                        FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId), personId, false);
        if (rr.code != FeatureTemplateEncryptedStore.Code.OK || rr.plaintext == null) {
            return null;
        }
        try {
            return new String(rr.plaintext, StandardCharsets.UTF_8);
        } finally {
            Arrays.fill(rr.plaintext, (byte) 0);
        }
    }

    /**
     * 后台线程：为给定人员集合预取并解密；同时**丢弃缓存中已不在集合内的条目**（人员已被删除）。
     *
     * <p>只清缓存、**不删文件**——传入的集合未必权威（例如仓库读取失败时可能为空），
     * 据此删文件风险过高；孤儿文件请用 {@link #removeOrphans(Set)} 显式清理。
     */
    void prefetch(@NonNull Iterable<String> personIds) {
        Set<String> live = new HashSet<>();
        for (String id : personIds) {
            if (id == null || id.isEmpty()) continue;
            live.add(id);
            if (cache.containsKey(id)) continue;   // 已有缓存：不重复解密

            FeatureTemplateEncryptedStore.ReadResult rr =
                    FeatureTemplateEncryptedStore.readNameString(
                            FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, id), id, false);
            if (rr.code != FeatureTemplateEncryptedStore.Code.OK || rr.plaintext == null) {
                continue;   // 未命中/损坏/密钥不可用 → 保持未缓存（get 会降级为 personId）
            }
            try {
                cache.put(id, new String(rr.plaintext, StandardCharsets.UTF_8));
            } finally {
                Arrays.fill(rr.plaintext, (byte) 0);   // 明文用毕立即清零
            }
        }
        cache.keySet().retainAll(live);
    }

    /**
     * 后台线程：加密落盘并**写穿缓存**。
     *
     * @return 是否成功；失败时不改动缓存、不留下明文
     */
    boolean save(@NonNull String personId, @Nullable String displayName) {
        String name = (displayName == null) ? "" : displayName.trim();
        if (name.isEmpty()) {
            return false;   // 空显示名无意义：不落盘，get 自会降级为 personId
        }
        FeatureTemplateEncryptedStore.WriteResult wr =
                FeatureTemplateEncryptedStore.writeNameString(
                        FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId), personId, name);
        if (wr.code != FeatureTemplateEncryptedStore.Code.OK) {
            return false;
        }
        cache.put(personId, name);
        return true;
    }

    /** 后台线程：删除映射（文件 + 缓存）。文件删除失败不影响缓存清理。 */
    void remove(@NonNull String personId) {
        //noinspection ResultOfMethodCallIgnored
        FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId).delete();
        cache.remove(personId);
    }

    /**
     * 后台线程：清理**孤儿文件**——目录内存在但其 personId 已不在 {@code liveIds} 中的显示名文件。
     *
     * <p><b>调用方必须确保 {@code liveIds} 权威</b>（注册表已成功载入）。传空集合等于删除全部，
     * 故本方法刻意不自动调用，也不与 {@link #prefetch} 合并。
     *
     * @return 实际删除的文件数
     */
    int removeOrphans(@NonNull Set<String> liveIds) {
        File[] files = dir.listFiles();
        if (files == null) return 0;
        Set<String> liveNames = new HashSet<>();
        for (String id : liveIds) {
            if (id == null || id.isEmpty()) continue;
            liveNames.add(FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, id).getName());
        }
        int removed = 0;
        for (File f : files) {
            String n = f.getName();
            if (!n.startsWith("n_") || !n.endsWith(".enc")) continue;
            if (liveNames.contains(n)) continue;
            if (f.delete()) removed++;
        }
        return removed;
    }
}
