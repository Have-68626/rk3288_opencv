package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
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
 * <p><b>缓存存 {@code byte[]} 而非 {@code String}</b>（A2 审计 Low 修复）：{@code String} 不可清零，
 * 会被 GC 长期滞留；{@code byte[]} 可在逐出/删除时 {@code Arrays.fill} 擦除，使**长期驻留**的那份
 * 明文可控。仅在 {@link #get} 调用点生成短命 {@code String}（UI 需要 {@code CharSequence}，无法避免）。
 *
 * <p><b>线程约定</b>：只有 {@link #get} 可在 UI 线程调用；其余方法都会做文件 IO 与加解密，
 * **必须在后台线程调用**。
 */
final class PersonDisplayNameStore {

    @NonNull
    private final File dir;
    /** personId → 显示名的 UTF-8 字节（可擦除）。 */
    @NonNull
    private final ConcurrentHashMap<String, byte[]> cache = new ConcurrentHashMap<>();

    /**
     * 删除行为接缝（**仅测试注入用**）：默认走真实文件系统；测试可经
     * {@link #setFileOpsForTesting} 注入替身以在纯 JVM 下构造「删除失败」场景。
     *
     * <p><b>实例字段而非进程级静态</b>（子代理 4 P4）：本字段只属于本实例，注入**不会**影响其它
     * 实例或其它测试类，跨实例泄漏在结构上不可能发生。
     *
     * <p><b>为何非 {@code final}（安全发布前提，如实固化）</b>：字段须可经
     * {@link #setFileOpsForTesting} 在**构造后**改写（测试注入），故不能标 {@code final}；即它
     * **非 volatile、非 final**。理论上若本实例被**不安全发布**，并发读者可能见到 {@code null}。
     * 当前仓库**安全**：本类实例在生产中仅经 {@code MainActivity.displayNameStore} 的
     * {@code volatile} 字段 + 双检锁发布（见该字段声明与 {@code displayNameStore()}），volatile 写
     * 建立了 happens-before ⇒ 构造期对该字段的赋值先于其它线程的读取而可见。**改动发布路径
     * （去掉 volatile / 改为常规共享）前须复核本前提**。
     */
    @NonNull
    private FileStoreUtils.FileOps fileOps = FileStoreUtils.DEFAULT_FILE_OPS;

    /**
     * 平台密钥提供者接缝（**仅测试注入用**）：默认 {@link FeatureTemplateEncryptedStore#DEFAULT_KEY_PROVIDER}
     * （生产走 AndroidKeyStore）；测试可经 {@link #setKeyProviderForTesting} 注入替身，以在纯 JVM 下
     * 跑真实 AES-GCM 或构造「密钥不可用」场景。
     *
     * <p><b>实例字段而非进程级静态</b>（对 {@code FeatureTemplateEncryptedStore} 静态 provider 的扩面
     * 改造，与 {@link #fileOps} 同构）：本字段只属于本实例，注入**不会**影响其它实例或其它测试类，
     * 跨实例泄漏在结构上不可能发生。本类的 2 处加密调用（{@link #readPlainBytes}、{@link #save}）
     * 都以它为首个形参显式传给存储层。
     */
    @NonNull
    private FeatureTemplateEncryptedStore.PlatformKeyProvider keyProvider =
            FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER;

    PersonDisplayNameStore(@NonNull File dir) {
        this.dir = dir;
    }

    /** 仅供测试注入删除行为替身；传 null 恢复默认（真实文件系统）实现。**仅影响本实例**。 */
    @androidx.annotation.VisibleForTesting
    void setFileOpsForTesting(@Nullable FileStoreUtils.FileOps ops) {
        fileOps = (ops != null) ? ops : FileStoreUtils.DEFAULT_FILE_OPS;
    }

    /** 仅供测试注入密钥提供者替身；传 null 恢复默认（AndroidKeyStore）实现。**仅影响本实例**。 */
    @androidx.annotation.VisibleForTesting
    void setKeyProviderForTesting(@Nullable FeatureTemplateEncryptedStore.PlatformKeyProvider p) {
        keyProvider = (p != null) ? p : FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER;
    }

    /**
     * 本机是否具备加密能力（API &gt;= 23 且密钥可用）。供调用方避免徒劳的迁移重试。
     *
     * <p><b>实例方法而非 static</b>（FACE-JAVA-001 修复，与 {@code AndroidEnrollmentRepository.
     * isEnrollmentEnabled()} 这一同类能力查询对齐）：读本实例的 {@link #keyProvider} 字段，故「平台
     * 支持判定」这一维度在测试中**可注入** —— 注入「不支持」替身即可在纯 JVM 下断言判定为 false。
     *
     * <p><b>调用点</b>：唯一生产调用方 {@code MainActivity.migrateLegacyDisplayNames} 改为
     * {@code displayNameStore().isSupported()}；该实例经惰性双检锁取得（与本方法内既有的
     * {@code store.save(...)} 同一实例），获取无额外代价。默认（未注入）时 {@link #keyProvider} 即
     * {@link FeatureTemplateEncryptedStore#DEFAULT_KEY_PROVIDER}，判定恒等于真实平台能力。
     */
    boolean isSupported() {
        return FeatureTemplateEncryptedStore.isPlatformSupported(keyProvider);
    }

    /**
     * UI 线程安全：纯读内存映射，**无 IO、无加解密、无锁竞争**（ConcurrentHashMap）。
     *
     * <p>每次调用生成一个短命 {@code String}（UI 需要），但**长期驻留**的那份是 {@code byte[]}，
     * 可在逐出或删除时被擦除。
     *
     * @return 解密后的显示名；未命中或为空时返回 {@code personId} 本身
     */
    @NonNull
    String get(@NonNull String personId) {
        byte[] v = cache.get(personId);
        if (v == null || v.length == 0) return personId;
        return new String(v, StandardCharsets.UTF_8);
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
        byte[] plain = readPlainBytes(personId);
        if (plain == null) return null;
        try {
            return new String(plain, StandardCharsets.UTF_8);
        } finally {
            Arrays.fill(plain, (byte) 0);
        }
    }

    @Nullable
    private byte[] readPlainBytes(@NonNull String personId) {
        FeatureTemplateEncryptedStore.ReadResult rr =
                FeatureTemplateEncryptedStore.readNameString(
                        keyProvider, FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId),
                        personId, false);
        if (rr.code != FeatureTemplateEncryptedStore.Code.OK || rr.plaintext == null) {
            return null;
        }
        return rr.plaintext;   // 所有权移交调用方（缓存或就地清零）
    }

    /**
     * 后台线程：为给定人员集合预取并解密；同时**逐出缓存中已不在集合内的条目**（人员已被删除，
     * 逐出时擦除其明文字节）。
     *
     * <p>**每次都重新解密**（A2 审计 Low 修复）：此前对已缓存条目直接跳过，导致「文件被带外删除
     * 或密钥轮换」后缓存**永不失效**、一直显示陈旧名字。预取只在启动 / 注册 / 删除时发生（非逐帧），
     * 重解密的代价可接受。
     *
     * <p>只清缓存、**不删文件**——传入的集合未必权威（例如仓库读取失败时可能为空），
     * 据此删文件风险过高；孤儿文件请用 {@link #removeOrphans(Set)} 显式清理。
     */
    void prefetch(@NonNull Iterable<String> personIds) {
        Set<String> live = new HashSet<>();
        for (String id : personIds) {
            if (id == null || id.isEmpty()) continue;
            live.add(id);

            byte[] fresh = readPlainBytes(id);
            byte[] previous = (fresh != null) ? cache.put(id, fresh) : cache.get(id);
            if (fresh == null) continue;          // 读不到就保留旧值（不可读≠已注销）
            if (previous != null && previous != fresh) {
                Arrays.fill(previous, (byte) 0);  // 换值即擦除旧明文
            }
        }
        // 逐出已不在集合内的条目，并擦除其明文。
        for (String key : new ArrayList<>(cache.keySet())) {
            if (live.contains(key)) continue;
            byte[] removed = cache.remove(key);
            if (removed != null) Arrays.fill(removed, (byte) 0);
        }
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
                        keyProvider, FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId),
                        personId, name);
        if (wr.code != FeatureTemplateEncryptedStore.Code.OK) {
            return false;
        }
        byte[] previous = cache.put(personId, name.getBytes(StandardCharsets.UTF_8));
        if (previous != null) Arrays.fill(previous, (byte) 0);
        return true;
    }

    /**
     * 后台线程：删除映射（文件 + 缓存）。
     *
     * <p><b>⚠️ 异常契约已变更（A4）—— 阅读旧审计记录时须先看这一段</b>：此前
     * {@link FileStoreUtils#deleteWithBackup} 抛出的 {@code SecurityException} 会**向上传播**，
     * 并且据此**跳过**缓存清理，与「文件删除失败不影响缓存清理」的承诺相抵触。现在改为在**本方法内**
     * catch，缓存清理移入 {@code finally}。故：若在变更前的审计 / 文档 / 用例里读到「remove 会抛
     * {@code SecurityException}」「异常会跳过缓存清理」，那是**变更前**的描述，**不是**回归
     * ——不要据此把异常传播「修」回来。（对应地，旧用例已被新用例**替换**而非并存：
     * {@code removePropagatesSecurityExceptionAndSkipsCacheClear} 删除、
     * {@code removeSwallowsSecurityExceptionAndClearsCache} 取而代之。）
     *
     * <p><b>契约（现行为）</b>：文件删除失败 —— 实为「任何异常」—— 都不影响缓存清理。
     * 缓存里存的是**可擦除的显示名明文**（本类刻意用 {@code byte[]} 以便 {@code Arrays.fill} 擦除）；
     * 人已删除却因异常把明文留在内存缓存 = **PII 未如期清除**。故缓存清理放在 {@code finally}，
     * 直白表达「**无论如何都清**」。措辞如实：{@code finally} 对**任何**逃逸出 try 块的异常都会执行
     * 清理，而 try 块内**只 catch 了 {@link SecurityException}** —— 故「会被清理」的原因集合是
     * 「会被吞掉」的原因集合的**超集**（任何异常都清缓存，但只有 {@code SecurityException} 被吞）。
     *
     * <p><b>为什么只 catch {@code SecurityException}（有意的窄捕获）</b>：文件删除被拒
     * （{@code File.delete()} 抛 {@code SecurityException}）属可预期的 best-effort 失败，吞掉即可；
     * 而 {@code NullPointerException} 之类**真正的编程错误**必须照常向外暴露、不得被静默掩盖。故此处
     * **刻意只捕 {@code SecurityException}**。⚠️ 这与本仓 {@code …Quietly} 惯例**并不同构**：对照
     * {@code FaceModelManager.deleteRecursivelyQuietly} 捕的是 {@code Exception}（连编程错误一并吞）。
     * 本处更窄、也更安全：删除只是收尾，但吞掉 NPE 会掩盖真正的 bug。二者是**有意**的差异，非疏漏。
     *
     * <p><b>为什么在此 catch 而非改 {@link FileStoreUtils#deleteWithBackup}</b>：后者是共享工具，其
     * 「不吞 {@code SecurityException}」的语义被 {@code AndroidEnrollmentRepository.remove} 依赖
     * （它据此回滚注册表并返回 {@code IO_ERROR}）；让共享工具吞异常会让那个调用点的 catch 沦为
     * **静默死代码**。故本调用点自行包装（与之一起，构成本仓两处「调用点自行包装
     * {@code deleteWithBackup}」的实例）。
     *
     * <p><b>可观测性（本轮修复的缺口）</b>：此前本方法返回 {@code void} 且不记日志 ⇒ 无论删除是被
     * **静默拒绝**（{@code delete} 返回 false，不抛异常）还是**抛 {@code SecurityException}**，
     * 调用方都**无法得知文件是否真的被删掉**，失败完全无痕。现改为**返回 {@code boolean}**，
     * 两条失败路径都会如实报 {@code false}。
     *
     * <p><b>返回值只用于上报/诊断，不得用作「要不要清缓存」的条件</b>：缓存清理在 {@code finally}，
     * 与返回值**无关** —— 内存明文的 PII 擦除优先级最高。
     *
     * <p><b>为什么是「返回 boolean」而非「在本方法内记日志」</b>：本类在
     * {@code PersonDisplayNameStoreTest} 中是**纯 JVM** 构造的（无 Robolectric，且未开
     * {@code returnDefaultValues}），而本仓日志走 {@link AppLog} → {@code android.util.Log}
     * ⇒ 在纯 JVM 单测里会抛 "not mocked"。故本类**不引入 Android 日志依赖**、只负责报告；日志留在
     * 平台层（{@code MainActivity.removeDisplayName} 消费本返回值后记一条）。**不要**改回「向上传播
     * 异常」来替代本返回值 —— 那会重新引入「异常跳过缓存清理」的 PII 缺口。
     *
     * <p><b>失败后的残留与收敛（边界写准）</b>：磁盘上留下的是**密文**（{@code n_*.enc}，
     * AES-GCM 加密件），**不是明文**；它由后续**注册表条目集权威（{@code list.authoritative}）**时的
     * {@code MainActivity.refreshGalleryFromRepository} → {@link #removeOrphans} 按孤儿回收
     * （该谓词现已覆盖 {@code .bak}）。⚠️ **不是「下次一定收敛」**：非权威时该路径整体跳过，残留可能
     * 跨越多次刷新。故契约边界是「**最终会收敛 + 失败可观测**」，而非「立即删除」。
     *
     * @return 主文件是否**确认已不存在**：{@code true} = 确实删除（含本就无文件，幂等口径，与
     *         {@link FileStoreUtils#deleteWithBackup} 一致）；{@code false} = 删除被拒
     *         （{@code SecurityException}）**或**静默未生效（{@code delete} 返回 false）。
     *         **无论返回什么，缓存都已清理。**
     */
    boolean remove(@NonNull String personId) {
        boolean gone = false;
        try {
            // 一并删掉写入器备份（.bak）：否则读路径的「阶段 1 恢复基线」会在下次读取时把已删除的
            // 显示名复活（A2-Med3）。返回值即「主文件确认已不存在」（幂等口径）。
            gone = FileStoreUtils.deleteWithBackup(
                    FeatureTemplateEncryptedStore.nameFileForPersonInDir(dir, personId), fileOps);
        } catch (SecurityException ignored) {
            // best-effort：删除被拒（SecurityManager）不应阻断缓存清理 —— 见上方 javadoc（PII 擦除优先）。
            // gone 保持 false：**不谎报删除成功** —— 这正是本方法返回值存在的意义。
            //
            // 本分支**刻意不记日志**：本类在 PersonDisplayNameStoreTest 中是**纯 JVM** 构造的
            // （无 Robolectric，未开 returnDefaultValues），而本仓日志走 AppLog → android.util.Log，
            // 在此会抛 "not mocked"。日志留在平台层（MainActivity.removeDisplayName 消费返回值后记录）。
        } finally {
            byte[] removed = cache.remove(personId);
            if (removed != null) Arrays.fill(removed, (byte) 0);
        }
        // 缓存清理**无条件**发生（finally），与本返回值无关；归还的是「文件是否确认已删」。
        return gone;
    }

    /**
     * 后台线程：清理**孤儿文件**——目录内存在但其 personId 已不在 {@code liveIds} 中的显示名文件。
     *
     * <p><b>调用方必须确保 {@code liveIds} 权威</b>（注册表条目集未被降级）。传空集合等于删除全部，
     * 故本方法刻意不自动调用，也不与 {@link #prefetch} 合并。
     *
     * <p><b>单个孤儿删除失败不得中断整轮清理</b>：若某项删除抛 {@link SecurityException}（或
     * {@link FileStoreUtils#deleteWithBackup} 返回 false），**跳过该项并继续**处理其余孤儿，绝不因
     * 一个坏项放弃整轮。异常项**不计入返回值**。
     *
     * @return 实际删除的文件数（**仅在确实删除成功时 +1**；被拒/删不掉的项不计入）
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
            // 谓词须**同时**匹配主文件（n_*.enc）与其写入器备份（n_*.enc.bak）：二者都参与孤儿判定。
            // A4 修复（Med）：此前谓词只认 `endsWith(".enc")`，使「主文件已不在、只剩 .bak」的孤儿
            // **永不进入本循环**，而读路径的阶段 1 恢复基线会在下次读取时把该 .bak 改名还原为主文件
            // 并解密 ⇒ **持久的 PII 复活种子**。注意：已修的「+207」只覆盖 remove 路径（一并删 .bak），
            // **本谓词当时漏了**；且须保持 `n_` 前缀，免得误删模板文件 u_*.bin
            // （removeOrphansDeletesOnlyUnlistedNameFiles 明确断言不得误删）。
            String mainName = n.endsWith(".bak") ? n.substring(0, n.length() - 4) : n;
            if (!mainName.startsWith("n_") || !mainName.endsWith(".enc")) continue;
            // 以**主文件基名**判存活：主文件在册时其 .bak 亦视为在册（不得误删在册者的备份）。
            if (liveNames.contains(mainName)) continue;
            // 一并删掉写入器备份（n_*.enc.bak）：否则读路径的「阶段 1 恢复基线」会在下次读取时把
            // **已删除**的显示名复活（A2-Med3）。
            //
            // 异常语义（本轮选定）：**跳过并继续，且不计入 removed** —— 与本方法对「deleteWithBackup
            // 返回 false」的既有口径一致（见 removeOrphansDoesNotCountFailedDeletes 用例）；返回值
            // 始终是「确实被删除的文件数」。此处 catch 只为「一个坏项不拖垮整轮」，不暴露失败明细
            // （本方法无返回值通道承载之，且调用方 MainActivity 只需 best-effort 收尾）。
            try {
                if (FileStoreUtils.deleteWithBackup(f, fileOps)) removed++;
            } catch (SecurityException ignored) {
                // 该项删不掉：不计入 removed，继续下一项。
            }
        }
        return removed;
    }
}
