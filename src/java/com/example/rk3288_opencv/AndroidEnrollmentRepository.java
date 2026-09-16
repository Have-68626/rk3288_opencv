package com.example.rk3288_opencv;

import android.content.Context;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Android 人员注册仓库（任务 16）：通用作用域 + gallery 级注册表，并在其上接线
 * {@link FeatureTemplateEncryptedStore}（此前该存储工具 0 调用者）。
 *
 * <h2>存储布局（gallery 目录）</h2>
 * <ul>
 *   <li>{@code face_registry_v1.dat} —— 单表注册表（gallery 级），记录已注册人员的<b>元数据</b>：
 *       userId、模型版本、模板 schema 版本、密文大小、密文 SHA-256、入册时间。
 *       表内<b>不含任何生物特征向量</b>。</li>
 *   <li>{@code u_<sha256(userId)>.bin} —— 每位人员的加密模板，由
 *       {@link FeatureTemplateEncryptedStore} 以 AndroidKeyStore + AES-GCM 落盘。</li>
 * </ul>
 *
 * <h2>为什么注册表用明文单表、模板仍加密</h2>
 * <ul>
 *   <li>设计文档《人员注册与权限系统顶层设计》§3.3/§8.1 要求“人脸模板与人员主数据分离存储”，
 *       敏感的是模板向量，注册表只是索引元数据。</li>
 *   <li>模板文件名是 userId 的 SHA-256（单向），没有注册表就无法从文件名反推 userId，
 *       因此枚举/gallery 索引必须依赖一张表。</li>
 *   <li>API &lt; 23 无 AndroidKeyStore AES-GCM，无法解密任何密文；若注册表也加密，则低版本
 *       连“只读预览”都做不了。注册表明文才能让低版本列出人员元数据。</li>
 *   <li>注册表里只有密文的 SHA-256/大小，没有明文模板或特征向量，泄漏风险远低于模板本身。</li>
 *   <li><b>userId 是可逆编码而非加密（明文档化取舍）</b>：注册表用 hex 编码 userId
 *       （见 {@link #persistRegistry}），hex 只是编码、可还原明文，并非哈希。这是刻意取舍——
 *       {@link #list} 需要在低版本上展示人员标识以支撑预览，改为加盐哈希会直接牺牲该能力；
 *       且设计文档（§3.3/§8.1）认定敏感数据是模板向量，人员标识属索引元数据。若后续要求标识
 *       不可逆，需同步引入“展示名映射表”或放弃 list 展示，不能只改这一处编码。</li>
 * </ul>
 *
 * <h2>并发</h2>
 * 实例方法不再使用实例级 {@code synchronized}：{@link #forDefaultGallery}/{@link #forScope}
 * 每次都会 {@code new}，同目录多实例并发读改写注册表会丢更新。改为按<b>规范化 scopeDir 路径</b>
 * 共享静态锁（{@link FileStoreUtils#dirLockFor}），使同目录的所有实例串行化 add/remove/readTemplate/list/get。
 *
 * <h2>注册表容错</h2>
 * 单行条目损坏时<b>跳过并隔离该行</b>，保留其余可解析条目，{@link ListResult#skippedLineCount}
 * 报告跳过数；只有文件头（魔数/版本）损坏或整体不可读才判定为 {@link Status#CORRUPTED}。
 * 后续一次成功的 add/remove 会重写注册表，顺带清除已跳过的损坏行（自愈）。
 *
 * <p><b>崩溃恢复（P1-3）</b>：{@link #persistRegistry} 采用
 * 「tmp 落盘+fsync → dst→bak → tmp→dst → 清理 bak」阶段替换。若在中间阶段进程退出，
 * 可能出现「主文件缺失/不可读、而 {@code face_registry_v1.dat.bak} 尚存」的窗口；
 * {@link #loadRegistry} 此时会先尝试把备份恢复为主文件再解析（重命名失败则直接解析备份），
 * 避免把唯一索引伪装成空库、并在后续写入时被先行删除，导致人员索引永久丢失。
 *
 * <h2>API &lt; 23 行为</h2>
 * 写操作与需密钥操作（{@link #add}、{@link #remove}、{@link #readTemplate}）返回
 * {@link Status#UNSUPPORTED_PLATFORM} 并附带中文说明；注册表读取（{@link #list}/{@link #get}）
 * 仍然放行，用于“只读预览 + 注册入口禁用并解释”。删除同属写操作，一并纳入该门禁以保持
 * “只读”语义自洽。
 */
public final class AndroidEnrollmentRepository {

    /** gallery 级注册表文件名（单表）。 */
    public static final String REGISTRY_FILE_NAME = "face_registry_v1.dat";

    /** 注册表首行魔数/版本：文件头损坏或版本不符时判定为 CORRUPTED。 */
    private static final String REGISTRY_MAGIC = "FREG1";

    /** 合法条目字段数：hex(userId) mv sv ciphertextSize sha256hex createdAtMs。 */
    private static final int REGISTRY_FIELD_COUNT = 6;

    /** 默认作用域目录名，与 {@link FeatureTemplateEncryptedStore#defaultDir(Context)} 保持一致。 */
    private static final String DEFAULT_SCOPE_DIR = "face_templates";

    // 目录锁与静默改名已收敛到 FileStoreUtils（任务 A4c）。

    /** 仓库操作结果码。模板加解密类错误码沿用存储层语义，避免双端错误码分裂。 */
    public enum Status {
        OK,
        /** 新增时目标 userId 已存在；仓库拒绝覆盖已有模板。 */
        DUPLICATE_USER,
        NOT_FOUND,
        INVALID_ARGUMENT,
        /** API &lt; 23：注册/删除/解密禁用（读取预览放行）。 */
        UNSUPPORTED_PLATFORM,
        IO_ERROR,
        /** Keystore 密钥不可用（创建/加载失败）。 */
        KEYSTORE_ERROR,
        BAD_FORMAT,
        UNSUPPORTED_VERSION,
        DECRYPT_FAILED,
        /** 注册表或模板文件损坏。 */
        CORRUPTED,
        ATOMIC_RENAME_FAILED
    }

    /** 注册表条目：仅元数据，不含模板/向量。 */
    public static final class Entry {
        @NonNull public final String userId;
        public final int modelVersion;
        public final int templateSchemaVersion;
        /** 加密模板文件字节数。 */
        public final long ciphertextSize;
        /** 加密模板密文的 SHA-256（小写十六进制），用于完整性来源核对，不是模板明文摘要。 */
        @NonNull public final String ciphertextSha256Hex;
        public final long createdAtMs;

        Entry(@NonNull String userId, int modelVersion, int templateSchemaVersion,
              long ciphertextSize, @NonNull String ciphertextSha256Hex, long createdAtMs) {
            this.userId = userId;
            this.modelVersion = modelVersion;
            this.templateSchemaVersion = templateSchemaVersion;
            this.ciphertextSize = ciphertextSize;
            this.ciphertextSha256Hex = ciphertextSha256Hex;
            this.createdAtMs = createdAtMs;
        }
    }

    /** 单条操作结果。 */
    public static final class Result {
        @NonNull public final Status status;
        @Nullable public final Entry entry;
        /** 中文说明；只含错误码语义，绝不包含模板/向量内容。 */
        @NonNull public final String message;

        Result(@NonNull Status status, @Nullable Entry entry, @NonNull String message) {
            this.status = status;
            this.entry = entry;
            this.message = message;
        }

        public boolean isOk() {
            return status == Status.OK;
        }
    }

    /** 列表结果；{@code entries} 永不为 null，出错时为空列表。 */
    public static final class ListResult {
        @NonNull public final Status status;
        @NonNull public final List<Entry> entries;
        /**
         * 解析时跳过并隔离的损坏行数；文件正常或无损坏行时为 0。
         * 头部损坏（{@link Status#CORRUPTED}）时该值为 0。
         */
        public final int skippedLineCount;
        @NonNull public final String message;
        /**
         * 条目集是否**权威**（即「就是全部已注册人员」）。跳过损坏行 / 从 {@code .bak} 恢复 /
         * 无注册表文件时均为 false。
         *
         * <p><b>破坏性对账必须以此为前提</b>：用非权威集合做差集删除会误删仍注册者的派生数据
         * （显示名孤儿清理即其一）。
         */
        public boolean authoritative = true;

        ListResult(@NonNull Status status, @NonNull List<Entry> entries,
                   int skippedLineCount, @NonNull String message) {
            this.status = status;
            this.entries = entries;
            this.skippedLineCount = skippedLineCount;
            this.message = message;
        }

        public boolean isOk() {
            return status == Status.OK;
        }
    }

    /** 模板明文消费回调：仅在一次 {@link #readTemplate} 调用内有效，返回后即被清零。 */
    public interface TemplateConsumer {
        void accept(@NonNull byte[] plaintextTemplate);
    }

    /** 仅供默认目录解析与存储层上下文使用；单元测试可传 null（存储层 write/read 不读取该参数）。 */
    @Nullable private final Context ctx;
    @NonNull private final File scopeDir;

    /**
     * 通用作用域构造：模板与注册表都落在 {@code scopeDir} 内。
     *
     * @param ctx       应用上下文；仅默认工厂需要，测试可直接传 null
     * @param scopeDir  gallery 目录（不存在会尝试创建）
     */
    public AndroidEnrollmentRepository(@Nullable Context ctx, @NonNull File scopeDir) {
        this.ctx = ctx;
        this.scopeDir = scopeDir;
        if (!scopeDir.exists()) {
            // 创建失败不在此处抛错，交由后续写入返回 IO_ERROR。
            scopeDir.mkdirs();
        }
    }

    /** 默认 gallery：与存储层默认目录一致（{@code files/face_templates}）。 */
    @NonNull
    public static AndroidEnrollmentRepository forDefaultGallery(@NonNull Context ctx) {
        return new AndroidEnrollmentRepository(ctx, FeatureTemplateEncryptedStore.defaultDir(ctx));
    }

    /** 通用作用域工厂：目录名相对 {@code files/} 目录。 */
    @NonNull
    public static AndroidEnrollmentRepository forScope(@NonNull Context ctx, @NonNull String scopeDirName) {
        return new AndroidEnrollmentRepository(ctx, new File(ctx.getFilesDir(), scopeDirName));
    }

    @NonNull
    public File scopeDir() {
        return scopeDir;
    }

    @NonNull
    public File registryFile() {
        return new File(scopeDir, REGISTRY_FILE_NAME);
    }

    /** 某 userId 在本次作用域内的加密模板文件（命名与存储层一致）。 */
    @NonNull
    public File templateFileFor(@NonNull String userId) {
        return FeatureTemplateEncryptedStore.fileForUserIdInDir(scopeDir, userId);
    }

    /** API &gt;= 23 且密钥可用时为 true。低版本返回 false。 */
    public boolean isEnrollmentEnabled() {
        return FeatureTemplateEncryptedStore.isPlatformSupported();
    }

    /** API &lt; 23：只读预览模式。 */
    public boolean isReadOnlyPreview() {
        return !isEnrollmentEnabled();
    }

    /** 低版本禁用原因（中文，供 UI 直接展示）。 */
    @NonNull
    public String unsupportedReason() {
        return "当前系统低于 Android 6.0（API 23），无 AndroidKeyStore AES-GCM，"
                + "注册入口已禁用；仅支持只读预览。";
    }

    /**
     * 新增人员模板。已存在（注册表条目或同名模板文件）时返回
     * {@link Status#DUPLICATE_USER}，<b>不覆盖</b>原模板。
     */
    @NonNull
    public Result add(@NonNull String userId,
                      @NonNull byte[] template,
                      int modelVersion,
                      int templateSchemaVersion) {
        synchronized (dirLock()) {
            if (userId.isEmpty()) {
                return new Result(Status.INVALID_ARGUMENT, null, "userId 不能为空");
            }
            if (template.length == 0) {
                return new Result(Status.INVALID_ARGUMENT, null, "模板不能为空");
            }
            if (!isEnrollmentEnabled()) {
                return new Result(Status.UNSUPPORTED_PLATFORM, null, unsupportedReason());
            }

            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new Result(snap.status, null, snap.message);
            }
            Entry existing = find(snap.entries, userId);
            if (existing != null) {
                return new Result(Status.DUPLICATE_USER, existing, "该人员已注册，仓库拒绝覆盖");
            }
            File tpl = templateFileFor(userId);
            if (tpl.exists()) {
                return new Result(Status.DUPLICATE_USER, null, "目标模板文件已存在，仓库拒绝覆盖");
            }

            FeatureTemplateEncryptedStore.WriteResult wr = FeatureTemplateEncryptedStore.write(
                    ctx, tpl, userId, template, modelVersion, templateSchemaVersion);
            if (wr.code != FeatureTemplateEncryptedStore.Code.OK) {
                return new Result(mapStoreCode(wr.code), null, describe(wr.code));
            }

            // 写入成功后回读密文，计算长度与密文摘要（注册表只存密文摘要，不存模板明文）。
            Entry entry;
            byte[] raw = readFileQuietly(tpl);
            if (raw == null || raw.length == 0) {
                FileStoreUtils.deleteWithBackup(tpl);
                return new Result(Status.IO_ERROR, null, "写入后无法回读模板文件");
            }
            entry = new Entry(userId, modelVersion, templateSchemaVersion, raw.length,
                    FeatureTemplateEncryptedStore.sha256Hex(raw), System.currentTimeMillis());

            List<Entry> next = new ArrayList<>(snap.entries);
            next.add(entry);
            Status persisted = persistRegistry(next);
            if (persisted != Status.OK) {
                // 注册表写失败 -> 回滚模板，保持模板文件与注册表一致。
                FileStoreUtils.deleteWithBackup(tpl);
                return new Result(persisted, null, "注册表写入失败，已回滚模板");
            }
            return new Result(Status.OK, entry, "");
        }
    }

    /** 列出全部已注册人员（API &lt; 23 也放行，用于只读预览）。 */
    @NonNull
    public ListResult list() {
        synchronized (dirLock()) {
            RegistrySnapshot snap = loadRegistry();
            ListResult result = new ListResult(snap.status, snap.entries, snap.skippedLineCount, snap.message);
            result.authoritative = snap.authoritative;
            return result;
        }
    }

    /** 查询单个人员元数据。 */
    @NonNull
    public Result get(@NonNull String userId) {
        synchronized (dirLock()) {
            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new Result(snap.status, null, snap.message);
            }
            Entry e = find(snap.entries, userId);
            if (e == null) {
                return new Result(Status.NOT_FOUND, null, "未找到该人员");
            }
            return new Result(Status.OK, e, "");
        }
    }

    /**
     * 删除人员：移除模板文件与注册表条目。
     *
     * <p>删除不需要加解密，但同属写操作，因此与 {@link #add} 一致纳入
     * {@link #isEnrollmentEnabled()} 门禁：API &lt; 23 返回 {@link Status#UNSUPPORTED_PLATFORM}，
     * 保持“只读预览”语义自洽。
     *
     * <p>事务顺序：<b>先落盘注册表（移除条目），再删模板</b>。若先删模板后写注册表，写失败时
     * 无法恢复已删除的模板，会留下“悬空条目”（list 有条目、readTemplate 返回 NOT_FOUND）；
     * 反过来，注册表先成功、模板删除失败时，可用旧快照回写注册表恢复一致。
     */
    @NonNull
    public Result remove(@NonNull String userId) {
        synchronized (dirLock()) {
            if (!isEnrollmentEnabled()) {
                return new Result(Status.UNSUPPORTED_PLATFORM, null, unsupportedReason());
            }
            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new Result(snap.status, null, snap.message);
            }
            Entry e = find(snap.entries, userId);
            if (e == null) {
                return new Result(Status.NOT_FOUND, null, "未找到该人员");
            }

            List<Entry> next = new ArrayList<>(snap.entries);
            next.remove(e);
            Status persisted = persistRegistry(next);
            if (persisted != Status.OK) {
                // 注册表未改动，模板也未删除，状态保持一致。
                return new Result(persisted, null, "注册表写入失败");
            }

            File tpl = templateFileFor(userId);
            if (tpl.exists()) {
                boolean deleted;
                try {
                    // 一并删掉写入器备份（.bak）：否则读路径的「阶段 1 恢复基线」会在下次读取时
                    // 把**已删除**的模板复活（A2-Med3）。
                    deleted = FileStoreUtils.deleteWithBackup(tpl);
                } catch (SecurityException ex) {
                    deleted = false;
                }
                if (!deleted) {
                    // 模板未能移除 -> 回写旧注册表，恢复到删除前状态。
                    Status rollback = persistRegistry(snap.entries);
                    String msg = (rollback == Status.OK)
                            ? "模板文件删除失败，已回滚注册表"
                            : "模板文件删除失败，注册表回滚亦失败";
                    return new Result(Status.IO_ERROR, e, msg);
                }
            }
            return new Result(Status.OK, e, "");
        }
    }

    /**
     * 读取并消费模板明文：明文只在 {@code consumer} 回调内有效，回调返回后由存储层
     * {@code Arrays.fill(pt, 0)} 清零，仓库不会向调用方泄漏明文数组。
     *
     * <p>整个读+消费过程持有目录锁，避免并发 {@link #remove} 在读过程中删除条目/文件；
     * 锁可重入，同一线程内回调再调用本仓库方法不会自锁。
     *
     * @return {@code OK} 表示回调已成功执行且明文已清零
     */
    @NonNull
    public Result readTemplate(@NonNull String userId, @NonNull final TemplateConsumer consumer) {
        synchronized (dirLock()) {
            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new Result(snap.status, null, snap.message);
            }
            final Entry e = find(snap.entries, userId);
            if (e == null) {
                return new Result(Status.NOT_FOUND, null, "未找到该人员");
            }
            FeatureTemplateEncryptedStore.ReadResult rr = FeatureTemplateEncryptedStore.readAndConsume(
                    ctx, templateFileFor(userId), userId, e.modelVersion, e.templateSchemaVersion, true,
                    new FeatureTemplateEncryptedStore.PlaintextConsumer() {
                        @Override
                        public void accept(@NonNull byte[] plaintextTemplate) {
                            consumer.accept(plaintextTemplate);
                        }
                    });
            if (rr.code != FeatureTemplateEncryptedStore.Code.OK) {
                return new Result(mapStoreCode(rr.code), e, describe(rr.code));
            }
            return new Result(Status.OK, e, "");
        }
    }

    // ---------------- 按 scopeDir 共享的静态锁 ----------------

    /**
     * 取得规范化 scopeDir 路径对应的锁对象；同目录的任意实例返回同一对象。
     * 便于单元测试直接断言“同目录共享锁”。
     */
    @androidx.annotation.VisibleForTesting
    @NonNull
    static Object dirLockForTesting(@NonNull File scopeDir) {
        return FileStoreUtils.dirLockFor(scopeDir);
    }

    @NonNull
    private Object dirLock() {
        return FileStoreUtils.dirLockFor(scopeDir);
    }

    // ---------------- 注册表序列化（明文元数据，行式文本，无外部依赖） ----------------

    private static final class RegistrySnapshot {
        final Status status;
        final List<Entry> entries;
        /** 解析时跳过的损坏行数；头部损坏时为 0。 */
        final int skippedLineCount;
        final String message;
        /**
         * 条目集是否**权威**（即「就是全部已注册人员」）。以下情形一律 false：
         * 跳过损坏行、从 {@code .bak} 恢复（备份可能早于最近新增）、以及压根没有注册表文件
         * （无法区分「全新安装」与「索引丢失」）。
         *
         * <p>用途：显示名孤儿清理等**破坏性**对账必须以此为前提——用非权威集合做差集删除会误删。
         */
        boolean authoritative = true;

        RegistrySnapshot(Status status, List<Entry> entries, int skippedLineCount, String message) {
            this.status = status;
            this.entries = entries;
            this.skippedLineCount = skippedLineCount;
            this.message = message;
        }
    }

    @NonNull
    private RegistrySnapshot loadRegistry() {
        File dst = registryFile();
        File bak = backupFileFor(dst);

        if (!dst.exists()) {
            if (bak.exists()) {
                // 崩溃恢复：上次 persistRegistry 在「dst→bak」之后、「tmp→dst」完成前退出，
                // 主文件缺失但备份仍是唯一完整索引。若此处按空库处理，后续写入会先删掉该备份，
                // 已注册人员索引将永久丢失（P1-3）。
                RegistrySnapshot fromBak = parseRegistryFile(bak);
                if (fromBak.status == Status.OK) {
                    boolean restored = FileStoreUtils.renameQuietly(bak, dst);
                    RegistrySnapshot recovered = new RegistrySnapshot(Status.OK, fromBak.entries,
                            fromBak.skippedLineCount,
                            restored ? "已从备份恢复注册表" : "注册表主文件缺失，已从备份读取");
                    recovered.authoritative = false;   // 备份可能早于最近新增，条目集未必完整
                    return recovered;
                }
                // 备份也损坏/不可读：如实上报，避免伪装成空库后被一次写入覆盖。
                return fromBak;
            }
            // 无注册表文件：无法区分「全新安装」与「索引丢失」，故不视为权威。
            RegistrySnapshot none = new RegistrySnapshot(Status.OK, new ArrayList<Entry>(), 0, "");
            none.authoritative = false;
            return none;
        }

        RegistrySnapshot snap = parseRegistryFile(dst);
        // 主文件存在但不可读：若备份可解析则从备份恢复，避免「不可读」退化为空库/丢索引。
        if (snap.status == Status.IO_ERROR && bak.exists()) {
            RegistrySnapshot fromBak = parseRegistryFile(bak);
            if (fromBak.status == Status.OK) {
                FileStoreUtils.renameQuietly(bak, dst);
                RegistrySnapshot recovered = new RegistrySnapshot(Status.OK, fromBak.entries,
                        fromBak.skippedLineCount, "注册表主文件不可读，已从备份恢复");
                recovered.authoritative = false;   // 同上：备份未必反映最新状态
                return recovered;
            }
        }
        return snap;
    }

    /** 解析单个注册表文件；文件不存在或不可读返回 IO_ERROR（缺省语义由调用方处理）。 */
    @NonNull
    private static RegistrySnapshot parseRegistryFile(@NonNull File f) {
        List<String> lines = new ArrayList<>();
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                lines.add(line);
            }
        } catch (Exception ex) {
            return new RegistrySnapshot(Status.IO_ERROR, new ArrayList<Entry>(), 0, "注册表读取失败");
        }
        if (lines.isEmpty() || !REGISTRY_MAGIC.equals(lines.get(0).trim())) {
            return new RegistrySnapshot(Status.CORRUPTED, new ArrayList<Entry>(), 0, "注册表头损坏");
        }

        // 单行损坏只跳过并隔离该行，保留其余可解析条目；仅头部/整体不可解析才 CORRUPTED。
        List<Entry> out = new ArrayList<>();
        int skipped = 0;
        for (int i = 1; i < lines.size(); i++) {
            String line = lines.get(i).trim();
            if (line.isEmpty()) {
                // 空行属正常格式容忍，不计入损坏。
                continue;
            }
            Entry parsed = parseEntry(line);
            if (parsed == null) {
                skipped++;
                continue;
            }
            out.add(parsed);
        }
        String msg = (skipped == 0) ? "" : ("注册表有 " + skipped + " 行损坏，已跳过");
        RegistrySnapshot snap = new RegistrySnapshot(Status.OK, out, skipped, msg);
        snap.authoritative = (skipped == 0);   // 跳过了损坏行 ⇒ 条目集不等于全部已注册人员
        return snap;
    }

    /** 静默重命名（用于崩溃恢复）；失败返回 false，由调用方决定后续语义。 */

    /** 解析单行条目；字段非法或损坏返回 null（由调用方跳过隔离）。 */
    @Nullable
    private static Entry parseEntry(@NonNull String line) {
        String[] t = line.split("\\s+");
        if (t.length != REGISTRY_FIELD_COUNT) {
            return null;
        }
        try {
            String userId = new String(hexToBytes(t[0]), StandardCharsets.UTF_8);
            int mv = Integer.parseInt(t[1]);
            int sv = Integer.parseInt(t[2]);
            long size = Long.parseLong(t[3]);
            String sha = t[4];
            long created = Long.parseLong(t[5]);
            if (userId.isEmpty() || size < 0 || sha.length() != 64) {
                return null;
            }
            return new Entry(userId, mv, sv, size, sha, created);
        } catch (Exception ex) {
            return null;
        }
    }

    /** 注册表备份文件（与主文件同目录）；崩溃恢复依赖该命名。 */
    @NonNull
    private static File backupFileFor(@NonNull File registryFile) {
        return new File(registryFile.getParentFile(), registryFile.getName() + ".bak");
    }

    /**
     * 原子落盘注册表：临时文件 + 阶段替换（与存储层写入策略一致）。
     *
     * <p>阶段顺序（P1-3 崩溃安全）：
     * <ol>
     *   <li><b>恢复基线</b>：若上次崩溃遗留「主文件缺失 + 备份存在」，先把备份改名回主文件，
     *       否则会先删备份再写，唯一索引在崩溃窗口中丢失。恢复失败则直接报错，不删任何文件。</li>
     *   <li><b>写 tmp + fsync</b>：新内容先完整落盘。</li>
     *   <li><b>dst→bak</b>：主文件存在时先挪作备份；此步后即便崩溃，{@link #loadRegistry}
     *       也能从 bak 恢复。</li>
     *   <li><b>tmp→dst</b>：新文件就位失败则把 bak 回滚为主文件。</li>
     *   <li><b>清理 bak</b>：仅在新主文件确认就位后才删旧备份。</li>
     * </ol>
     */
    @NonNull
    private Status persistRegistry(@NonNull List<Entry> entries) {
        File dst = registryFile();
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            return Status.IO_ERROR;
        }
        File bak = backupFileFor(dst);

        // 阶段 1：恢复上次崩溃遗留的「主文件缺失 + 备份存在」，确立本轮可回滚基线。
        // 恢复不了就宁可不写：绝不删掉唯一索引。
        if (!dst.exists() && bak.exists() && !FileStoreUtils.renameQuietly(bak, dst)) {
            return Status.IO_ERROR;
        }

        File tmp = new File(parent, dst.getName() + ".tmp");
        StringBuilder sb = new StringBuilder();
        sb.append(REGISTRY_MAGIC).append('\n');
        for (Entry e : entries) {
            // userId 以可逆 hex 编码落盘（非哈希）：list 预览需要展示标识，见类级 javadoc 取舍说明。
            sb.append(toHex(e.userId.getBytes(StandardCharsets.UTF_8))).append(' ')
                    .append(e.modelVersion).append(' ')
                    .append(e.templateSchemaVersion).append(' ')
                    .append(e.ciphertextSize).append(' ')
                    .append(e.ciphertextSha256Hex).append(' ')
                    .append(e.createdAtMs).append('\n');
        }
        // 阶段 2：写 tmp 并 fsync，确保内容先落盘再谈替换。
        try (FileOutputStream fos = new FileOutputStream(tmp, false)) {
            fos.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Exception ignored) {
            }
        } catch (Exception ex) {
            tmp.delete();
            return Status.IO_ERROR;
        }

        // 阶段 3：dst -> bak。仅当主文件存在时才有此步；此阶段后崩溃可由 loadRegistry 恢复。
        if (dst.exists()) {
            if (bak.exists() && !bak.delete()) {
                tmp.delete();
                return Status.IO_ERROR;
            }
            if (!dst.renameTo(bak)) {
                tmp.delete();
                return Status.ATOMIC_RENAME_FAILED;
            }
        }
        // 阶段 4：tmp -> dst。失败则把旧备份回滚为主文件，保证不丢索引。
        if (!tmp.renameTo(dst)) {
            if (bak.exists()) {
                bak.renameTo(dst);
            }
            tmp.delete();
            return Status.ATOMIC_RENAME_FAILED;
        }
        // 阶段 5：新主文件已就位，才允许清理旧备份（清理失败不影响一致性）。
        if (bak.exists()) {
            bak.delete();
        }
        return Status.OK;
    }

    @Nullable
    private static Entry find(@NonNull List<Entry> entries, @NonNull String userId) {
        for (Entry e : entries) {
            if (e.userId.equals(userId)) {
                return e;
            }
        }
        return null;
    }

    @NonNull
    private Status mapStoreCode(@NonNull FeatureTemplateEncryptedStore.Code code) {
        switch (code) {
            case OK:
                return Status.OK;
            case UNSUPPORTED_PLATFORM:
                return Status.UNSUPPORTED_PLATFORM;
            case NOT_FOUND:
                return Status.NOT_FOUND;
            case IO_ERROR:
                return Status.IO_ERROR;
            case KEYSTORE_ERROR:
                return Status.KEYSTORE_ERROR;
            case BAD_FORMAT:
                return Status.BAD_FORMAT;
            case UNSUPPORTED_VERSION:
                return Status.UNSUPPORTED_VERSION;
            case DECRYPT_FAILED:
                return Status.DECRYPT_FAILED;
            case CORRUPTED:
                return Status.CORRUPTED;
            case ATOMIC_RENAME_FAILED:
                return Status.ATOMIC_RENAME_FAILED;
            default:
                return Status.IO_ERROR;
        }
    }

    @NonNull
    private String describe(@NonNull FeatureTemplateEncryptedStore.Code code) {
        switch (code) {
            case OK:
                return "";
            case UNSUPPORTED_PLATFORM:
                return unsupportedReason();
            case NOT_FOUND:
                return "模板文件不存在";
            case IO_ERROR:
                return "模板读写失败";
            case KEYSTORE_ERROR:
                return "Keystore 密钥不可用";
            case BAD_FORMAT:
                return "模板文件格式非法";
            case UNSUPPORTED_VERSION:
                return "模板版本不受支持";
            case DECRYPT_FAILED:
                return "模板解密失败";
            case CORRUPTED:
                return "模板文件损坏";
            case ATOMIC_RENAME_FAILED:
                return "模板文件原子替换失败";
            default:
                return "模板操作失败";
        }
    }

    @Nullable
    private static byte[] readFileQuietly(@NonNull File f) {
        try (FileInputStream fis = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = fis.read(buf)) >= 0) {
                if (n == 0) {
                    continue;
                }
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        } catch (Exception ex) {
            return null;
        }
    }

    private static final char[] HEX = "0123456789abcdef".toCharArray();

    @NonNull
    private static String toHex(@NonNull byte[] bytes) {
        char[] out = new char[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            int v = bytes[i] & 0xFF;
            out[i * 2] = HEX[v >>> 4];
            out[i * 2 + 1] = HEX[v & 0x0F];
        }
        return new String(out);
    }

    @NonNull
    private static byte[] hexToBytes(@NonNull String hex) {
        if ((hex.length() & 1) != 0) {
            throw new IllegalArgumentException("hex 长度非法");
        }
        byte[] out = new byte[hex.length() / 2];
        for (int i = 0; i < out.length; i++) {
            int hi = Character.digit(hex.charAt(i * 2), 16);
            int lo = Character.digit(hex.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) {
                throw new IllegalArgumentException("hex 字符非法");
            }
            out[i] = (byte) ((hi << 4) | lo);
        }
        return out;
    }
}
