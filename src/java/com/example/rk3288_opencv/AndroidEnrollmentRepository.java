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
 *       userId、模型版本、模板 schema 版本、加密模板<b>文件</b>（envelope 全量）大小、
 *       该文件 SHA-256、入册时间。表内<b>不含任何生物特征向量</b>。</li>
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
 *   <li>注册表里只有加密模板<b>文件</b>（envelope 全量）的 SHA-256/大小，没有明文模板或特征向量，
 *       泄漏风险远低于模板本身。</li>
 *   <li><b>userId 是可逆编码而非加密（明文档化取舍）</b>：注册表用 hex 编码 userId
 *       （见 {@link #persistRegistry}），hex 只是编码、可还原明文，并非哈希。这是刻意取舍——
 *       {@link #list} 需要在低版本上展示人员标识以支撑预览，改为加盐哈希会直接牺牲该能力；
 *       且设计文档（§3.3/§8.1）认定敏感数据是模板向量，人员标识属索引元数据。若后续要求标识
 *       不可逆，需同步引入“展示名映射表”或放弃 list 展示，不能只改这一处编码。</li>
 * </ul>
 *
 * <h2>并发</h2>
 * 实例方法不再用方法级 {@code synchronized} 修饰符：{@link #forDefaultGallery}/{@link #forScope}
 * 每次都会 {@code new}，同目录多实例并发读改写注册表会丢更新。改为按<b>规范化 scopeDir 路径</b>
 * 共享静态锁（{@link FileStoreUtils#dirLockFor}），使同目录的所有实例串行化 add/remove/readTemplate/list/get。
 *
 * <h2>注册表格式（A8 起升级为 FREG2）</h2>
 * 首行魔数标识版本：新版 {@code FREG2}＝「首行 {@code FREG2} / 第二行 {@code COUNT N} / N 行六字段 /
 * 末行 {@code END N}」，旧版 {@code FREG1}＝「首行 {@code FREG1} / 若干六字段行」（无条目数与结束标记）。
 * 空表真实内容为 {@code FREG2\nCOUNT 0\nEND 0\n}。首行既非 FREG1 也非 FREG2 时判 {@link Status#CORRUPTED}。
 *
 * <h2>注册表容错</h2>
 * 单行条目损坏时<b>跳过并隔离该行</b>，保留其余可解析条目，{@link ListResult#skippedLineCount}
 * 报告跳过数；只有文件头（魔数）损坏才判定为 {@link Status#CORRUPTED}，而<b>整体不可读判
 * {@link Status#IO_ERROR}</b>（两者都不可被当作空库）。
 * <b>但「可解析」不等于「完整」</b>：新版 FREG2 且 COUNT/END 一致、数据行数严格为 N、无重复
 * userId、END 后无额外记录时才判 {@link ListResult#isEnumerationComplete()}；被截断/缺尾/有坏行的
 * FREG2 一律<b>不完整</b>（可解析子集仅供查看，绝不被「跳过坏行」提升为完整）。旧版 FREG1
 * <b>主文件</b>无坏行时刻意判<b>完整</b>（兼容其原有正常使用，下次写入即升级为 FREG2），
 * 但旧版 FREG1 <b>副本</b>（{@code .tmp}/{@code .bak}）一律<b>不完整</b>、不自动提升、不参与判重。
 *
 * <p><b>自愈（如实边界，勿夸大）</b>：{@link #persistRegistry} 只序列化本次传进去的 {@code entries}
 * （被跳过的坏行不在其中），故一次成功的 {@link #remove} 会重写注册表（升级为 FREG2）、顺带清除
 * 已跳过的损坏行。但自愈的前提是**能走到那次写**：
 * <ul>
 *   <li>A8 起 {@link #add} <b>自身</b>就以枚举完整性为前置（{@code !complete} 即拒），故<b>直接 add
 *       也已不再是自愈路径</b>——它只会返回错误，不再「顺带清除坏行」；</li>
 *   <li>注册 UI 路径下 {@link EnrollmentSession#fromRepository} 亦以
 *       {@link ListResult#isEnumerationComplete()} 为判重门，不完整时对**每一帧**样本返回
 *       {@link EnrollmentSession.Reason#REPO_UNAVAILABLE} ⇒ 会话永远集不满 8 样本 ⇒ 经 UI 的
 *       {@code add} 不可达。即**自愈实际只剩 {@code remove} 一条路**。</li>
 *   <li>{@code remove} 的 UI 入口按 {@code list().entries} 列条目：若注册表**只剩被跳过的坏行**
 *       （{@code entries} 为空），列表里没有任何可删条目 ⇒ 经 {@code remove} 无法自愈；该死角现由
 *       {@link #quarantineCorruptRegistry()} 提供应用内恢复出口（详见该方法 javadoc）。</li>
 *   <li>{@link ListResult#message}（「注册表有 N 行损坏，已跳过」）与
 *       {@link ListResult#skippedLineCount} 是**可观测通道**，但状态码仍为 {@link Status#OK}
 *       ⇒ 只展示 {@code !isOk()} 文案的消费方看不到损坏信息。</li>
 * </ul>
 * ⚠️ 该可用性收敛的两半**状态不同**，不应再一并称为「未修」：`add` 自愈被判重门切断这半**仍成立**
 * （存在坏行时 {@link EnrollmentSession#fromRepository} 对每一帧返回
 * {@link EnrollmentSession.Reason#REPO_UNAVAILABLE}）；「只剩坏行 ⇒ UI 无出路」这半**已解**——
 * {@link #quarantineCorruptRegistry()} 给出应用内恢复出口。⚠️ 但仍须区分「**有出口**」与
 * 「**自动修复**」：该出口只**改名旁置**（不还原任何损坏条目）、新建的空表不含任何已注册人员，
 * 原文件仅供手工取回。
 *
 * <p><b>崩溃恢复（P1-3）</b>：{@link #persistRegistry} 采用
 * 「tmp 落盘+fsync → dst→bak → tmp→dst → 清理 bak」阶段替换。主文件**缺失或不可解析**
 * （头损坏 / 整体不可读）时，{@link #loadRegistry} 会按「先未消费的 {@code .tmp}、后 {@code .bak}」
 * 取首个可解析的副产品作为注册表：主文件**不存在**时把它改名就位，主文件**存在**（即便损坏）
 * 时只读、不覆盖，避免把唯一索引伪装成空库、并在后续写入时被先行删除，导致人员索引永久丢失。
 * 因此「头损坏 + 健康副本并存」也会先自愈，{@code quarantineCorruptRegistry} 不再把健康的副本
 * 随损坏主文件一并旁置、清空活性库。
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

    /**
     * 注册表**新版**首行魔数（A8）：第二行为 {@code COUNT N}，其后 N 行六字段记录，末行 {@code END N}。
     * 首行既非 {@code FREG1} 也非 {@code FREG2} 时判定为 CORRUPTED。
     */
    private static final String REGISTRY_MAGIC_V2 = "FREG2";

    /**
     * 注册表**旧版**首行魔数：仅有魔数 + 若干六字段行、**无条目数与结束标记**，故无法证明其自身完整。
     * 仅作**主文件读取兼容**；作为**副本**（{@code .tmp}/{@code .bak}）时不得自动用作判重依据。
     */
    private static final String REGISTRY_MAGIC_V1 = "FREG1";

    /** FREG2 第二行前缀：{@code COUNT <N>}，声明数据行条数。 */
    private static final String COUNT_PREFIX = "COUNT";

    /** FREG2 末行前缀：{@code END <N>}，与 COUNT 相同，作为结束标记与尾数校验。 */
    private static final String END_PREFIX = "END";

    /**
     * 旁置文件后缀：把不可用的注册表工件改名保留为 {@code <原名>.corrupt.<毫秒时间戳>}，
     * 命名习惯与 {@link FeatureTemplateEncryptedStore} 的损坏隔离保持一致（那里也是
     * {@code <名>.corrupt.<ts>}）。**只改名、绝不删除**。
     */
    private static final String CORRUPT_SUFFIX = ".corrupt.";

    /** 合法条目字段数：hex(userId) mv sv templateFileSize templateFileSha256Hex createdAtMs
     *  （第 4/5 列存的是加密模板**文件**全量值，见 {@link Entry} 字段注释）。 */
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
        /**
         * 加密模板<b>文件</b>的字节数 —— 即整个 envelope（魔数+版本+IV+AAD+密文+tag）的长度，
         * <b>不是</b>纯密文（ct+tag）段的长度，两者相差 64B 头。
         *
         * <p>历史改名记录：原名 {@code ciphertextSize}（名实不符，本字段并非纯密文长度），
         * 已改为现名。注册表是<b>位置式六列文本、列名不落盘</b> ⇒ 改字段名不影响已写盘格式，
         * <b>无需迁移</b>。
         */
        public final long templateFileSize;
        /**
         * 加密模板<b>文件</b>（envelope 全量）的 SHA-256（小写十六进制），用于完整性来源核对；
         * 既不是模板明文摘要，也不是仅密文段的摘要。
         *
         * <p>历史改名记录：原名 {@code ciphertextSha256Hex}（名实不符），已改为现名。实际计算与
         * 比对的对象是整个 envelope，比纯密文多 64B 头
         * （{@code MAGIC4 + VERSION1 + ivLen1 + iv12 + aadLen2 + aad40 + ctLen4}）。
         * 列名不落盘，改名不影响已写盘格式（同 {@link #templateFileSize}）。
         */
        @NonNull public final String templateFileSha256Hex;
        public final long createdAtMs;

        Entry(@NonNull String userId, int modelVersion, int templateSchemaVersion,
              long templateFileSize, @NonNull String templateFileSha256Hex, long createdAtMs) {
            this.userId = userId;
            this.modelVersion = modelVersion;
            this.templateSchemaVersion = templateSchemaVersion;
            this.templateFileSize = templateFileSize;
            this.templateFileSha256Hex = templateFileSha256Hex;
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
         * <p>⚠️ <b>仅当 {@link #status} == {@link Status#OK} 时该判据才有意义；
         * {@code status != OK} 时其值<b>未定义</b>。例如「主文件缺失 + 备份存在但备份自身
         * 解析失败」这条路径会把备份的解析结果原样返回（{@code loadRegistry}），此时
         * {@code status} 为 {@code CORRUPTED}/{@code IO_ERROR}，而本字段仍是构造默认值。
         * 消费方必须先用 {@link #isOk()} 门禁，不得直接读本字段。
         *
         * <p><b>破坏性对账必须以此为前提</b>：用非权威集合做差集删除会误删仍注册者的派生数据
         * （显示名孤儿清理即其一）。
         */
        public boolean authoritative = true;

        /**
         * 注册表**格式版本**：2 = 新版（{@code FREG2} + {@code COUNT/END}，完整性可证），
         * 1 = 旧版（{@code FREG1}，仅读取兼容），0 = 无表 / 头损坏 / 不可读（版本未知）。
         *
         * <p>仅 {@link #status} == {@link Status#OK} 时有意义。
         */
        public int formatVersion = 0;

        /**
         * 枚举**完整性**标记（见 {@link #isEnumerationComplete()}）。由仓库层按格式与结构校验置位；
         * {@code status != OK} 时恒为 false。
         */
        public boolean complete = true;

        /**
         * 条目集是否**可证明完整**（枚举未缺行、未被截断），即「能被安全用作判重依据」。
         *
         * <p><b>判据（A8 起）</b>：
         * <ul>
         *   <li>**新版 FREG2 主文件**结构通过严格校验（{@code COUNT N} 与 {@code END N} 一致、
         *       实际数据行数严格为 N、每条合法且 userId 不重复、END 后无额外记录）且无跳过坏行时为 true；</li>
         *   <li>**旧版 FREG1 主文件**在无跳过坏行时亦为 true —— 它没有条目数/尾标记，行边界截断无法被
         *       察觉，故此处置 true 是刻意**兼容 FREG1 主文件的原有正常使用**（读取兼容 ⇒ 允许升级，
         *       下次写入即升级为 FREG2），与 FREG2 的强校验口径不同；</li>
         *   <li>**FREG1 副本**（{@code .tmp}/{@code .bak}）一律不置 true —— 副本须经人工恢复、
         *       不自动提升、不参与判重。</li>
         * </ul>
         *
         * <p>⚠️ <b>语义边界（如实登记，勿夸大）</b>：本判据只证**结构性自洽**（枚举未缺行、未被截断），
         * <b>不构成内容真实性证明</b> —— FREG2 无校验和，{@code COUNT}/{@code END} 均为**自声明**值，
         * 手写 {@code COUNT 2} + 2 行 + {@code END 2} 即可全过（磁盘上仍可能另有未列入的模板）；
         * FREG1 主文件更无尾标记。故 true 只表示「文件内自洽、可安全用作判重前提」，
         * <b>不等价于</b>「已等于已注册全集」。
         *
         * <p>⚠️ 与 {@link #authoritative} 分工不同、**不可互相替代**：
         * <ul>
         *   <li>{@code authoritative} 门禁**破坏性对账**（差集删除），无注册表文件时为 false
         *       （刻意兼容「全新安装」与「索引丢失」两种情况）；</li>
         *   <li>本判据服务**非破坏性判重**：判重漏一个在册者会让同一张脸被重复注册。全新安装
         *       （无表无副产物）时本判据为 true（否则全新安装永远无法注册）。</li>
         * </ul>
         *
         * <p><b>接线现状</b>：{@link EnrollmentSession#fromRepository} 的判重门以本方法为判据
         * （生产接线、非纯工具方法）；仓库侧 {@link #add} 亦以完整性为前置。
         */
        public boolean isEnumerationComplete() {
            return status == Status.OK && complete;
        }

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
     * 删除行为接缝（**仅测试注入用**）：默认走真实文件系统；测试可经 {@link #setFileOpsForTesting}
     * 注入替身以在纯 JVM 下构造「删除失败 / 删除抛 {@code SecurityException}」场景。
     *
     * <p><b>实例字段而非进程级静态</b>（子代理 4 P4）：本字段只属于本实例，注入**不会**影响其它
     * 实例或其它测试类，跨实例泄漏在结构上不可能发生。
     */
    @NonNull
    private FileStoreUtils.FileOps fileOps = FileStoreUtils.DEFAULT_FILE_OPS;

    /**
     * 平台密钥提供者接缝（**仅测试注入用**）：默认 {@link FeatureTemplateEncryptedStore#DEFAULT_KEY_PROVIDER}
     * （生产走 AndroidKeyStore）；测试可经 {@link #setKeyProviderForTesting} 注入 JCE 内存密钥替身，
     * 以在纯 JVM 下跑真实 AES-GCM。
     *
     * <p><b>实例字段而非进程级静态</b>（对 {@code FeatureTemplateEncryptedStore} 静态 provider 的扩面
     * 改造，与 {@link #fileOps} 同构）：本字段只属于本实例，注入**不会**影响其它实例或其它测试类，
     * 跨实例泄漏在结构上不可能发生。本类的 3 处加密调用（{@link #isEnrollmentEnabled}、
     * {@link #add}、{@link #readTemplate}）都以它为首个形参显式传给存储层。
     */
    @NonNull
    private FeatureTemplateEncryptedStore.PlatformKeyProvider keyProvider =
            FeatureTemplateEncryptedStore.DEFAULT_KEY_PROVIDER;

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
        return FeatureTemplateEncryptedStore.isPlatformSupported(keyProvider);
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
            // A8：直接 add 也必须以**枚举完整性**为前置 —— 若注册表被截断 / 缺尾 / 有跳过坏行，
            // 「库中是否已有此人」的判断就不可靠（漏一个在册者会让同一张脸被重复注册）。
            // 此处与 EnrollmentSession 的判重门同口径，不能只封会话入口。
            if (!snap.complete) {
                return new Result(Status.CORRUPTED, null,
                        "注册表枚举不完整（" + snap.message + "），已拒绝新增以免重复注册；"
                                + "请删除任一在册人员以重写注册表，或经「旁置损坏注册表」恢复");
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
                    keyProvider, ctx, tpl, userId, template, modelVersion, templateSchemaVersion);
            if (wr.code != FeatureTemplateEncryptedStore.Code.OK) {
                return new Result(mapStoreCode(wr.code), null, describe(wr.code));
            }

            // 写入成功后回读**整个加密模板文件**（envelope 全量），计算其长度与摘要；
            // 注册表只存该文件摘要，不存模板明文（字段名 ciphertext* 为历史遗留，见 Entry 注释）。
            Entry entry;
            byte[] raw = readFileQuietly(tpl);
            if (raw == null || raw.length == 0) {
                // 回滚 A（回读失败）：本分支在纯 JVM 下**不可构造**，故不加读接缝 ——
                // FileStoreUtils.FileOps 只有 delete/exists、**无 read**；写入（write）与回读
                // （readFileQuietly 裸 FileInputStream）之间也没有任何可注入点，而 tpl.exists()
                // 用的是裸 File.exists()。为唯一消费者给全仓共享接口加方法，成本大于收益。
                boolean rolledBack = FileStoreUtils.deleteWithBackup(tpl, fileOps);
                return new Result(Status.IO_ERROR, null, rolledBack
                        ? "写入后无法回读模板文件"
                        : "写入后无法回读模板文件，且模板回滚失败（可能残留孤儿模板）");
            }
            entry = new Entry(userId, modelVersion, templateSchemaVersion, raw.length,
                    FeatureTemplateEncryptedStore.sha256Hex(raw), System.currentTimeMillis());

            List<Entry> next = new ArrayList<>(snap.entries);
            next.add(entry);
            Status persisted = persistRegistry(next);
            if (persisted != Status.OK) {
                // 注册表写失败 -> 回滚模板，保持模板文件与注册表一致。
                // 回滚必须检查返回值：deleteWithBackup 返回「主文件是否确实已不存在」，
                // 失败即留下**孤儿模板** —— 该 userId 之后每次 add 都会撞上上面的 tpl.exists()
                // 恒返 DUPLICATE_USER，而 remove 先查注册表、无条目即 NOT_FOUND ⇒ 删不掉，
                // UI 无自愈路径。故失败时文案必须如实，不得谎报「已回滚」。
                // 注：本分支不 catch SecurityException（与 remove 不同），保持既有语义未变。
                boolean rolledBack = FileStoreUtils.deleteWithBackup(tpl, fileOps);
                return new Result(persisted, null, rolledBack
                        ? "注册表写入失败，已回滚模板"
                        : "注册表写入失败，且模板回滚失败（可能残留孤儿模板，需重装或手动清理）");
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
            result.formatVersion = snap.formatVersion;
            result.complete = snap.complete;
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
                    deleted = FileStoreUtils.deleteWithBackup(tpl, fileOps);
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
     * 注册表「整份不可枚举」时的**非破坏性**恢复：把当前注册表工件改名旁置，再新建一张空的可用注册表。
     *
     * <h2>为什么需要它</h2>
     * 注册表的自愈只有一条路：一次成功的 {@link #add}/{@link #remove} 会用 {@link #persistRegistry}
     * 重写注册表、顺带清除损坏行。但经 UI 的 {@code add} 被 {@link EnrollmentSession} 的判重门
     * （{@link ListResult#isEnumerationComplete()}）切断，{@code remove} 又需要至少一条可删条目。
     * 当注册表**整份损坏 / 全部行不合法**（{@code list().entries} 为空）时，UI 里没有任何可删条目
     * ⇒ 用户无法自愈。本方法为该死角提供一条**不删除任何注册表工件**的出路（原文件只改名旁置、字节完整
     * 保留；外部审查 [P2]「H：注册表全损坏时界面无恢复入口」）。
     *
     * <h2>绝不丢数据（设计约束）</h2>
     * <ul>
     *   <li><b>只改名、绝不删除</b>：把每个存在的工件移到
     *       {@code <原名>.corrupt.<当前毫秒>}（沿用 {@link FeatureTemplateEncryptedStore} 的损坏隔离
     *       命名习惯），原字节完整保留，结果 {@link Result#message} 回传它们的**绝对路径**供 UI 原样展示，
     *       用户可事后手工取回；</li>
     *   <li><b>一并旁置 {@code .bak}/{@code .tmp}</b>：否则 {@link #persistRegistry} 的阶段 1「恢复基线」
     *       会在主文件缺失时把这些残留文件改名回主文件、随后被空表覆盖 ⇒ 等于**销毁**它们；</li>
     *   <li><b>不枚举、不重建、不伪造条目</b>：损坏内容里读不出任何可靠信息，故新表就是一张干净空表
     *       （仅魔数行）。模板文件仍在磁盘，但其名是 userId 的 SHA-256、无注册表即无法反推，只能由用户
     *       在取回原注册表后手工恢复 —— 本方法**不猜测**重建任何条目；</li>
     *   <li>任何一步失败都**回滚已完成的改名**并返回错误，保持原状。</li>
     * </ul>
     *
     * <p><b>「不丢注册表工件」的精确边界（勿把「不删除任何注册表工件」当成「本方法一次 delete 都不发生」）</b>：
     * 本方法自身只**改名旁置**、不删除任何注册表工件（{@code dst} / {@code .bak} / {@code .tmp}）。但建空表所经的
     * {@link #persistRegistry} 在其**失败**支会 {@code tmp.delete()} 它**自己**刚写的 scratch 临时文件
     * （{@code tmpFileFor(dst)}）——删的是本次新建的临时文件、不是被旁置存留的注册表工件，与本方法「不丢注册表
     * 工件」的承诺不冲突。
     *
     * <h2>前置条件（不满足即拒绝，绝不误旁置）</h2>
     * 仅当注册表**没有任何可用条目**时允许：{@code status != OK}（头损坏 / 整体不可读），或
     * {@code entries} 为空且 {@code skippedLineCount > 0}（全部数据行损坏）。存在任一可解析条目时返回
     * {@link Status#INVALID_ARGUMENT} —— 那意味着走既有的 {@code remove} 自愈即可，绝不在此时把仍可用的
     * 人移出活性路径（用户会误以为人被删了）。
     *
     * <h2>与低版本语义自洽</h2>
     * 与 {@link #remove} 同为写操作，故同样纳入 {@link #isEnrollmentEnabled()} 门禁：API &lt; 23 返回
     * {@link Status#UNSUPPORTED_PLATFORM}，保持「只读预览」自洽。
     *
     * <h2>包内可见（package-private）</h2>
     * 本方法是**有能力面**的写操作（移动注册表文件），收窄为包内可见是为把这项能力限制在
     * {@code com.example.rk3288_opencv} 包内 —— 调用方 {@code MainActivity} 与单元测试本就同包，
     * 收窄不改变调用点；破坏性保护也不再只依赖「调用方会先弹二次确认」这一**约定**，而由编译器兜住。
     *
     * <p><b>如实边界 1（回滚分支的自动化覆盖 —— **条件覆盖**）</b>：「旁置成功、但新建空表（{@link #persistRegistry}）
     * 失败 ⇒ 回滚已完成的改名」这一支由 {@code RegistryQuarantineRecoveryTest} 以**悬空符号链接**构造覆盖（把
     * {@link #tmpFileFor} 的目标预置为悬空链接，令空表写入必然失败且失败点**晚于**旁置）。⚠️ 该覆盖**依赖符号
     * 链接支持**（Windows 需开发者模式 / 建链权限；受限 CI 亦不可）——不支持的环境下该用例以 {@code Assume}
     * **跳过**，此时该分支**无自动化判别锚**，并非无条件「已钉住」。仍**无**自动化判别锚的还有：**旁置本身失败**
     * （{@code renameQuietly} 返回 false）那支、「**回滚本身也失败**」（回滚 {@code renameTo} 亦失败，「回滚未完全
     * 成功」那一支文案）、回滚失败分支**文案**的分歧（谎报「已还原原注册表」时现无断言可拦）与**逆序回滚顺序**（现
     * 用例只有一个被旁置文件，顺序无差别）—— 均无法跨平台稳定构造，按代码走查保证语义。
     *
     * <p><b>如实边界 2（已知代价：「持续不可读」与「已损坏」不显式区分）</b>：前置条件把 {@code status != OK}
     * 一律视为「无可用条目」，故主文件**持续**不可读（非损坏，如权限 / ACL 拒绝）时也会被旁置 —— 结果是
     * **改名保留、不丢数据**，但**清空活性库**（新建空表）。之所以**不**引入「持续 vs 瞬时」的显式区分：
     * <ul>
     *   <li><b>瞬时故障已被现有双重检查挡下</b>：UI 侧先 {@code list()}（锁内）判定不可读才弹恢复入口，用户
     *       确认后进入本方法 —— 本方法**自身**在同一 {@code dirLock()} 内**重新** {@code loadRegistry()} 复核
     *       {@code noUsableEntries}；两次判定之间隔着用户确认（秒级），瞬时故障在此期间恢复时第二次判据即为
     *       false ⇒ 返回 {@link Status#INVALID_ARGUMENT}、**一个文件都不动**。该复核是本方法**内在**的（任何
     *       调用方都受益），不是依赖调用方自觉；</li>
     *   <li><b>「两次一致判定」并不能区分二者</b>：瞬时故障与持续故障在紧邻的两次读取里都表现为「不可读」，
     *       要真正区分必须引入**时间延迟**（如隔 N 毫秒复读）—— 给一个文件操作加上定时等待，复杂度与不确定性
     *       都显著上升，收益却仅覆盖「恰好在本方法执行瞬间仍不可读、随后又自行恢复」这一狭窄窗口；</li>
     *   <li><b>即便判定为「持续不可读」也没有更好的出路</b>：注册表读不出来时应用本就无法枚举人员，拒绝旁置只会
     *       让用户停在无恢复入口的状态；旁置（改名保留 + 空表 + 回传原文件路径）给出了一条可行的降级出路。</li>
     * </ul>
     * 故本项**登记为已知代价**，不引入「加固」。
     *
     * @return {@code OK} 时已旁置原文件并新建空表，{@link Result#message} 含旁置文件绝对路径（绝不包含
     *         模板/向量内容）；失败时状态码与既有错误通道一致，且文案**如实**反映回滚是否成功。
     */
    @NonNull
    Result quarantineCorruptRegistry() {
        synchronized (dirLock()) {
            if (!isEnrollmentEnabled()) {
                return new Result(Status.UNSUPPORTED_PLATFORM, null, unsupportedReason());
            }
            RegistrySnapshot snap = loadRegistry();
            // 无可用条目：整份不可读（头损坏 / IO），或条目为空且**枚举不完整**
            //（全部行被跳过、或 FREG2 被截断/缺尾而无任何可解析条目）。
            // ⚠️「全新安装（无表无副产物）」的 complete 为 true，故不会被误判为损坏。
            boolean noUsableEntries = snap.status != Status.OK
                    || (snap.entries.isEmpty() && !snap.complete);
            if (!noUsableEntries) {
                return new Result(Status.INVALID_ARGUMENT, null,
                        "注册表当前可用，未做任何改动；如需清除个别损坏行，请在人员列表删除任意一位人员");
            }

            File dst = registryFile();
            File parent = dst.getParentFile();
            long ts = System.currentTimeMillis();
            // 主文件 + 写入器备份 + 未消费的临时文件：三者都必须旁置，理由见方法 javadoc。
            File[] sources = new File[]{dst, backupFileFor(dst), tmpFileFor(dst)};
            List<File> movedFrom = new ArrayList<>();
            List<File> movedTo = new ArrayList<>();
            for (File f : sources) {
                if (!f.exists()) {
                    continue;
                }
                // 目标名必须取「当前不存在」的：POSIX/Android 的 rename(2) 会**静默覆盖**同名目标，
                // 不取唯一名会销毁此前的旁置证据（Windows 上 renameTo 到已存在目标会失败，故只在
                // POSIX 上危险）。同名时依次追加 `-<序号>`。
                File aside = uniqueAsideFile(parent, f.getName(), ts);
                if (!FileStoreUtils.renameQuietly(f, aside)) {
                    // 回滚已完成的改名，保持原状后如实报错。回滚失败必须如实说明实际状态，
                    // 不得谎报「未做任何改动」。
                    List<File> failed = rollbackMoves(movedFrom, movedTo);
                    return failed.isEmpty()
                            ? new Result(Status.IO_ERROR, null, "损坏注册表旁置失败，未做任何改动")
                            : new Result(Status.IO_ERROR, null,
                                    "损坏注册表旁置失败，且回滚未完全成功；以下文件仍在旁置位置"
                                            + "（原件未删除，可手工取回）：" + joinPaths(failed));
                }
                movedFrom.add(f);
                movedTo.add(aside);
            }

            // 建一张干净的空表。三部曲都已移开 ⇒ persistRegistry 阶段 1 不会再把它们改名回主文件后被覆盖。
            Status created = persistRegistry(new ArrayList<Entry>());
            if (created != Status.OK) {
                // 新建失败：把旁置的文件逐项移回原位。回滚失败必须如实说明实际状态，
                // 不得谎报「已还原原注册表」。
                List<File> failed = rollbackMoves(movedFrom, movedTo);
                return failed.isEmpty()
                        ? new Result(created, null, "新建空注册表失败，已还原原注册表")
                        : new Result(created, null,
                                "新建空注册表失败，且回滚未完全成功；以下文件仍在旁置位置"
                                        + "（原件未删除，可手工取回）：" + joinPaths(failed));
            }

            String message = movedTo.isEmpty()
                    ? "已新建空注册表（无可旁置的原文件）"
                    : "损坏注册表已旁置（原件未删除，可手工取回）：" + joinPaths(movedTo);
            return new Result(Status.OK, null, message);
        }
    }

    /**
     * 把已旁置的文件逐一移回原位，返回**未能还原**的文件（空列表 = 完全还原）。
     * 不因单项失败而中断（继续尝试其余项），以便如实报告实际残留状态。
     */
    @NonNull
    private static List<File> rollbackMoves(@NonNull List<File> movedFrom, @NonNull List<File> movedTo) {
        List<File> failed = new ArrayList<>();
        for (int i = movedFrom.size() - 1; i >= 0; i--) {
            if (!FileStoreUtils.renameQuietly(movedTo.get(i), movedFrom.get(i))) {
                failed.add(movedTo.get(i));
            }
        }
        return failed;
    }

    /** 用 {@code ；} 连接若干文件的**绝对路径**（供结果文案；只含路径，绝不含模板/向量内容）。 */
    @NonNull
    private static String joinPaths(@NonNull List<File> files) {
        StringBuilder sb = new StringBuilder();
        for (File f : files) {
            if (sb.length() > 0) {
                sb.append("；");
            }
            sb.append(f.getAbsolutePath());
        }
        return sb.toString();
    }

    /**
     * 为一旁置候选选一个**当前不存在**的目标名：{@code <原文件名>.corrupt.<ts>}，若已被占用则依次
     * 追加 {@code -<序号>}。POSIX/Android 的 {@code rename(2)} 会**静默覆盖**同名目标 ⇒ 不取唯一名
     * 会销毁此前的旁置证据；Windows 上 {@code renameTo} 到已存在目标会失败（失败安全）。
     */
    @NonNull
    @androidx.annotation.VisibleForTesting
    static File uniqueAsideFile(@Nullable File parent, @NonNull String baseName, long ts) {
        File candidate = asideCandidate(parent, baseName, Long.toString(ts));
        for (int seq = 1; candidate.exists(); seq++) {
            candidate = asideCandidate(parent, baseName, ts + "-" + seq);
        }
        return candidate;
    }

    @NonNull
    private static File asideCandidate(@Nullable File parent, @NonNull String baseName,
                                       @NonNull String tsToken) {
        String name = baseName + CORRUPT_SUFFIX + tsToken;
        return (parent == null) ? new File(name) : new File(parent, name);
    }

    // ---------------- 孤儿模板旁置（A15） ----------------

    /**
     * 孤儿模板列表结果。{@code fileNames} 永不为 null；出错时为空列表。
     *
     * <p>{@link #fileNames} 只含**文件名**（{@code u_<64hex>.bin}）—— 不含目录、不可反推 userId，
     * 也不代表任何具体人员，UI 据此展示数量与摘要即可。
     */
    public static final class OrphanListResult {
        @NonNull public final Status status;
        @NonNull public final List<String> fileNames;
        /** 是否允许旁置：仅「完整 FREG2 主表 + 平台支持」时为 true；否则只能查看。 */
        public final boolean quarantineAllowed;
        @NonNull public final String message;

        OrphanListResult(@NonNull Status status, @NonNull List<String> fileNames,
                         boolean quarantineAllowed, @NonNull String message) {
            this.status = status;
            this.fileNames = fileNames;
            this.quarantineAllowed = quarantineAllowed;
            this.message = message;
        }

        public boolean isOk() {
            return status == Status.OK;
        }
    }

    /**
     * 列出当前图库中的**孤儿模板**：模板文件名（{@code u_<sha256(userId)>.bin}）在注册表中没有任何
     * 条目引用 —— 通常是**注册表缺失/损坏**造成（模板仍在磁盘而索引丢了），故只旁置、不删除。
     *
     * <h2>只读语义与门禁</h2>
     * 本方法**只读**：仅扫描 gallery 目录的**直属普通文件**（不含子目录，含旁置目录
     * {@code .orphan-recovery/}）。缺主表 / 旧版 FREG1 / 不完整 FREG2 时
     * {@link OrphanListResult#quarantineAllowed} 为 false —— 此时枚举不可信，孤儿判定仅供参考，
     * 只可查看、不可旁置。
     *
     * <p><b>不反推人员、不展示明文</b>：文件名是 userId 的 SHA-256（单向），无法反推 userId；
     * 结果只回传文件名。注册表不可读时返回空列表（无法确认谁在册，不臆测孤儿）。
     */
    @NonNull
    public OrphanListResult listOrphanTemplates() {
        synchronized (dirLock()) {
            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new OrphanListResult(snap.status, new ArrayList<String>(), false,
                        "注册表不可读（" + snap.message + "），无法确认孤儿模板");
            }
            java.util.Set<String> referenced = new java.util.HashSet<>();
            for (Entry e : snap.entries) {
                referenced.add(templateFileFor(e.userId).getName());
            }
            List<String> orphans = new ArrayList<>();
            File[] children = scopeDir.listFiles();
            if (children != null) {
                for (File f : children) {
                    // 只处理**直属普通文件**：子目录（含旁置目录）不计；非模板命名不计。
                    if (f.isFile()
                            && FeatureTemplateEncryptedStore.isTemplateFileName(f.getName())
                            && !referenced.contains(f.getName())) {
                        orphans.add(f.getName());
                    }
                }
            }
            java.util.Collections.sort(orphans);

            boolean platformOk = isEnrollmentEnabled();
            boolean regProvable = snap.formatVersion == 2 && snap.complete;
            boolean allowed = platformOk && regProvable;
            String msg;
            if (!regProvable) {
                msg = "注册表非完整 FREG2（版本 " + snap.formatVersion + "、条目 " + snap.entries.size()
                        + "）：仅可查看孤儿模板，暂不可旁置";
            } else if (!platformOk) {
                msg = "当前系统不支持加密存储（API < 23）：仅可查看孤儿模板，暂不可旁置";
            } else {
                msg = "";
            }
            return new OrphanListResult(Status.OK, orphans, allowed, msg);
        }
    }

    /**
     * 把**单个**孤儿模板旁置到同图库的 {@code .orphan-recovery/} 目录（**只改名、不删除**，可手工取回）。
     *
     * <h2>前置门禁（不满足即拒绝，绝不误旁置）</h2>
     * <ul>
     *   <li>平台支持（API &ge; 23）；</li>
     *   <li>{@code fileName} 必须是**模板文件名**（{@code u_<64hex>.bin}）—— 不开放任意/绝对路径删除；</li>
     *   <li>注册表必须是**完整 FREG2 主表**（与 {@link ListResult#isEnumerationComplete()} 同口径），
     *       否则无法判定谁是孤儿；</li>
     *   <li>在**与注册共用的目录锁**内**重读**注册表与引用集合：若该文件已被引用、或已不存在/不再是
     *       模板文件（列表后被改动），一律拒绝并要求刷新；</li>
     *   <li>旁置目标取**当前不存在**的唯一名，绝不覆盖既有恢复文件。</li>
     * </ul>
     *
     * <p><b>数据安全</b>：只 {@code rename} 到 {@code .orphan-recovery/}，**不永久删除**；失败时原文件保留。
     * 旁置成功后原名释放，可用同一 userId 重新注册（副本仍在恢复目录中）。
     */
    @NonNull
    public Result quarantineOrphanTemplate(@NonNull String fileName) {
        synchronized (dirLock()) {
            if (!isEnrollmentEnabled()) {
                return new Result(Status.UNSUPPORTED_PLATFORM, null, unsupportedReason());
            }
            if (!FeatureTemplateEncryptedStore.isTemplateFileName(fileName)) {
                return new Result(Status.INVALID_ARGUMENT, null, "文件名不是合法模板文件名，已拒绝");
            }
            RegistrySnapshot snap = loadRegistry();
            if (snap.status != Status.OK) {
                return new Result(snap.status, null, "注册表不可读，无法确认该文件是否仍为孤儿");
            }
            if (!(snap.formatVersion == 2 && snap.complete)) {
                return new Result(Status.CORRUPTED, null,
                        "注册表非完整 FREG2，拒绝旁置以免误移在册人员的模板；请刷新后重试");
            }
            // 锁内重读引用集合：确认仍无该条目引用。
            java.util.Set<String> referenced = new java.util.HashSet<>();
            for (Entry e : snap.entries) {
                referenced.add(templateFileFor(e.userId).getName());
            }
            if (referenced.contains(fileName)) {
                return new Result(Status.INVALID_ARGUMENT, null,
                        "该文件已被注册表引用，拒绝旁置；请刷新列表");
            }
            File src = new File(scopeDir, fileName);
            if (!src.isFile()) {
                return new Result(Status.NOT_FOUND, null, "模板文件不存在或已变化，请刷新列表");
            }
            File recoveryDir = new File(scopeDir, FeatureTemplateEncryptedStore.ORPHAN_RECOVERY_DIR_NAME);
            if (!recoveryDir.exists() && !recoveryDir.mkdirs()) {
                return new Result(Status.IO_ERROR, null, "无法创建恢复目录，原文件保留");
            }
            File target = uniqueOrphanTarget(recoveryDir, fileName);
            if (!FileStoreUtils.renameQuietly(src, target)) {
                return new Result(Status.ATOMIC_RENAME_FAILED, null, "旁置失败，原文件保留");
            }
            return new Result(Status.OK, null, "已把孤儿模板旁置到 " + target.getAbsolutePath());
        }
    }

    /** 在恢复目录内为 {@code fileName} 选一个**当前不存在**的目标名（不覆盖既有恢复文件）。 */
    @NonNull
    @androidx.annotation.VisibleForTesting
    static File uniqueOrphanTarget(@NonNull File recoveryDir, @NonNull String fileName) {
        File candidate = new File(recoveryDir, fileName);
        for (int seq = 1; candidate.exists(); seq++) {
            candidate = new File(recoveryDir, fileName + "-" + seq);
        }
        return candidate;
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
                    keyProvider, ctx, templateFileFor(userId), userId, e.modelVersion,
                    e.templateSchemaVersion, true,
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
         * <p>同 {@link ListResult#authoritative}：**仅 {@code status == OK} 时该判据才有意义**，
         * {@code status != OK} 时其值未定义（「备份存在但备份自身解析失败」路径原样返回，不置 false）。
         *
         * <p>用途：显示名孤儿清理等**破坏性**对账必须以此为前提——用非权威集合做差集删除会误删。
         */
        boolean authoritative = true;

        /**
         * 注册表**格式版本**（2 = FREG2 可证完整；1 = FREG1 旧版；0 = 无表 / 损坏 / 恢复的副本）。
         *
         * <p>同 {@link ListResult#formatVersion}：仅 {@code status == OK} 时才有意义。
         */
        int formatVersion = 0;

        /**
         * 枚举**完整性**（见 {@link ListResult#isEnumerationComplete()}）：
         * 「新版 FREG2 主文件且结构严格通过校验」「无坏行的旧版 FREG1 **主文件**」（刻意兼容其原有正常
         * 使用、允许升级）或「全新安装（无表无副产物）」为 true；被截断/损坏的 FREG2、**任何**从副本
         * 读取/恢复来的内容（含健康 FREG1 副本）一律 false。
         */
        boolean complete = false;

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
            // ★★ 2026-09-28 修（外部审查 [P1]「备份恢复后可能放行重复人脸注册」+ 独立复核确认可达）：
            //   先试**未消费的 `.tmp`**，再退回 `.bak`。
            //   崩溃窗口「模板已落盘（add）→ 注册表 dst→bak 已完成 → tmp→dst 未完成」下，**最新**的
            //   条目集在 **`.tmp`** 里，而 `.bak` 是**缺该行**的旧集。原实现只认 `.bak` ⇒ 恢复出缺行集
            //   而判重门放行 ⇒ 同一张脸可用**另一个 userId** 再注册一次（add() 只按 userId 查模板
            //   文件存在性，换 ID 即绕过）。
            //   ★ A8 起：**仅完整 FREG2** 副本可被自动采用并（主文件缺失时）提升为主文件；
            //     旧版 FREG1 或截断/损坏的 FREG2 副本只能**只读查看**、不提升、不参与判重。
            RegistrySnapshot recovered = recoverFromSidecar(dst, "缺失");
            if (recovered != null) {
                return recovered;
            }
            // 走到这里：没有**可自动采用的完整 FREG2** 副本。
            // ★ A8：区分「真正首次安装（无表无副产物）」与「存在不可用副本」——后者绝不能伪装成
            //   首次空库，否则会以「库为空」放行注册（旧版/截断副本里的在册者会被重复注册）。
            File tmp = tmpFileFor(dst);
            File unusable = firstRegularFile(tmp, bak);
            if (unusable != null) {
                RegistrySnapshot parsed = parseRegistryFile(unusable);
                if (parsed.status == Status.OK) {
                    RegistrySnapshot copy = new RegistrySnapshot(Status.OK, parsed.entries,
                            parsed.skippedLineCount,
                            parsed.message.isEmpty()
                                    ? "注册表主文件缺失，仅剩不可用副本；仅供查看、不参与判重"
                                    : "注册表主文件缺失，仅剩不可用副本（" + parsed.message + "）；仅供查看、不参与判重");
                    copy.authoritative = false;
                    copy.formatVersion = parsed.formatVersion;
                    copy.complete = false;
                    return copy;
                }
                // 不可解析的副本（CORRUPTED / IO_ERROR）：如实上报。⚠️ 本路径与既有契约
                // 「status != OK 时 authoritative 未定义」一致，故**不覆写**该字段（保持构造默认）。
                return parsed;
            }
            // 真正首次无表无副产物的空库：合法，且判重门放行（否则全新安装永远无法注册）。
            // 注意：不视为权威（无法区分「全新安装」与「索引丢失」），但不影响判重完整性。
            RegistrySnapshot none = new RegistrySnapshot(Status.OK, new ArrayList<Entry>(), 0, "");
            none.authoritative = false;
            none.complete = true;
            return none;
        }

        RegistrySnapshot snap = parseRegistryFile(dst);
        if (snap.status != Status.OK) {
            // ★ 2026-09-29 修（头损坏 + 健康 .bak/.tmp 并存时错失自愈）：主文件**不可解析**
            //   （头损坏 CORRUPTED，或整体不可读 IO_ERROR）时，若仍有**可解析**的 `.tmp`/`.bak`，
            //   优先用它自愈，而不是把健康的副产品随主文件一起报废。
            //   A8 收窄：自愈**提升**只对**完整 FREG2** 副本开放；旧版/不完整副本仅只读查看，
            //   绝不 rename 覆盖主文件字节（POSIX 的 rename(2) 会静默覆盖同名目标，覆盖即销毁原件）。
            String mainState = (snap.status == Status.CORRUPTED) ? "头损坏" : "不可读";
            RegistrySnapshot recovered = recoverFromSidecar(dst, mainState);
            if (recovered != null) {
                return recovered;
            }
        }
        return snap;
    }

    /**
     * 主文件不可用（缺失 / 不可解析）时读取副产品：分两轮 ——
     * <ol>
     *   <li><b>采用</b>：按「先 `.tmp`、后 `.bak`」取**首个完整 FREG2** 副本，返回采用出的快照
     *       （{@code authoritative=false}、{@code complete=true}）；仅当主文件**不存在**时把它
     *       {@code renameQuietly} 就位（{@code canPromote}）。</li>
     *   <li><b>只读查看</b>：无完整 FREG2 副本时，取首个**可解析**副本供查看
     *       （{@code authoritative=false}、{@code complete=false}，**不提升、不判重**）。</li>
     * </ol>
     *
     * <p><b>顺序依据</b>：写者不变式「写 tmp + fsync → dst→bak → tmp→dst」下，残留的 `.tmp` 严格
     * 新于 `.bak`。旧版 FREG1 或截断/损坏的 FREG2 副本可能缺行，故**绝不**被「采用」；主文件
     * **存在**（哪怕是损坏 / 不可读的字节）时一律**只读**副本、绝不 rename 覆盖 —— POSIX 的
     * {@code rename(2)} 会静默覆盖同名目标，覆盖即销毁主文件原字节，违背「不可解析的工件仍应保留、
     * 改名不删」的数据安全约束。
     *
     * @param mainState 主文件当前状态的**如实**措辞（「缺失」/「头损坏」/「不可读」），仅用于结果文案
     * @return 采用或只读查看得的快照；无任何可解析的普通文件副本时返回 {@code null}（由调用方各自
     *         决定如何如实上报）
     */
    @Nullable
    private RegistrySnapshot recoverFromSidecar(@NonNull File dst, @NonNull String mainState) {
        boolean canPromote = !dst.exists();
        File tmp = tmpFileFor(dst);
        File bak = backupFileFor(dst);

        // 第一轮：**仅完整 FREG2** 副本可被自动采用，并在主文件缺失时提升为主文件。
        // 旧版 FREG1 或截断/损坏的 FREG2 副本一律不采用 —— 它们可能缺行，不能作为判重依据。
        // 顺序「先 .tmp、后 .bak」：写者不变式「写 tmp + fsync → dst→bak → tmp→dst」下，
        // 残留的 .tmp 严格新于 .bak。
        RegistrySnapshot adopted = adoptCompleteFreg2(tmp, dst, canPromote, "未消费的 .tmp", mainState);
        if (adopted != null) {
            return adopted;
        }
        adopted = adoptCompleteFreg2(bak, dst, canPromote, "备份", mainState);
        if (adopted != null) {
            return adopted;
        }

        // 第二轮：无可采用的完整 FREG2 副本时，退化为**只读查看**首个可解析副本。
        // 不提升、不判重（complete/authoritative 均置 false）—— 「读取兼容」不等于「已证明完整」。
        RegistrySnapshot view = viewOnlySidecar(tmp, mainState, "未消费的 .tmp");
        if (view != null) {
            return view;
        }
        return viewOnlySidecar(bak, mainState, "备份");
    }

    /**
     * 尝试把某个副本作为**完整 FREG2** 采用：解析通过且 {@code formatVersion==2 && complete} 时，
     * 在主文件缺失（{@code canPromote}）时把它改名提升为主文件，返回快照；否则返回 {@code null}。
     */
    @Nullable
    private RegistrySnapshot adoptCompleteFreg2(@NonNull File sidecar, @NonNull File dst,
                                                boolean canPromote, @NonNull String label,
                                                @NonNull String mainState) {
        if (!sidecar.isFile()) {
            return null;
        }
        RegistrySnapshot parsed = parseRegistryFile(sidecar);
        if (parsed.status != Status.OK || parsed.formatVersion != 2 || !parsed.complete) {
            return null;
        }
        boolean promoted = canPromote && FileStoreUtils.renameQuietly(sidecar, dst);
        RegistrySnapshot recovered = new RegistrySnapshot(Status.OK, parsed.entries,
                parsed.skippedLineCount,
                promoted ? ("已从" + label + "恢复注册表（含最新条目）")
                        : ("注册表主文件" + mainState + "，已从" + label + "读取（含最新条目）"));
        recovered.authoritative = false;
        recovered.formatVersion = parsed.formatVersion;
        recovered.complete = true;
        return recovered;
    }

    /** 把副本作为**只读查看**内容返回（不提升、不判重）；副本不可解析为 OK 时返回 {@code null}。 */
    @Nullable
    private RegistrySnapshot viewOnlySidecar(@NonNull File sidecar, @NonNull String mainState,
                                             @NonNull String label) {
        if (!sidecar.isFile()) {
            return null;
        }
        RegistrySnapshot parsed = parseRegistryFile(sidecar);
        if (parsed.status != Status.OK) {
            return null;
        }
        RegistrySnapshot view = new RegistrySnapshot(Status.OK, parsed.entries,
                parsed.skippedLineCount,
                "注册表主文件" + mainState + "，已从" + label + "读取（仅供查看，不参与判重）");
        view.authoritative = false;
        view.formatVersion = parsed.formatVersion;
        view.complete = false;
        return view;
    }

    /** 返回两个候选中的第一个**普通文件**（排除目录 / 悬空链接），无则返回 {@code null}。 */
    @Nullable
    private static File firstRegularFile(@NonNull File a, @NonNull File b) {
        if (a.isFile()) {
            return a;
        }
        return b.isFile() ? b : null;
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
        if (lines.isEmpty()) {
            return new RegistrySnapshot(Status.CORRUPTED, new ArrayList<Entry>(), 0, "注册表头损坏");
        }
        String magic = lines.get(0).trim();
        if (REGISTRY_MAGIC_V2.equals(magic)) {
            return parseRegistryV2(lines);
        }
        if (REGISTRY_MAGIC_V1.equals(magic)) {
            return parseRegistryV1Legacy(lines);
        }
        return new RegistrySnapshot(Status.CORRUPTED, new ArrayList<Entry>(), 0, "注册表头损坏");
    }

    /**
     * 旧版 FREG1：仅魔数 + 若干六字段行，**无条目数/结束标记**。
     *
     * <p>读取兼容：单行损坏跳过并隔离，保留其余可解析条目。因格式本身无法证明「未缺行」，
     * {@code complete} 只取「无跳过坏行」这一**必要条件**；行边界截断无法被察觉，属已接受并登记的
     * 风险（见 {@link ListResult#isEnumerationComplete()}）。作为**副本**使用时，调用方会另行把
     * {@code complete} 覆写为 false（副本永不参与判重）。
     */
    @NonNull
    private static RegistrySnapshot parseRegistryV1Legacy(@NonNull List<String> lines) {
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
        snap.formatVersion = 1;
        snap.complete = (skipped == 0);
        return snap;
    }

    /**
     * 新版 FREG2：<code>FREG2</code> / <code>COUNT N</code> / N 行六字段 / <code>END N</code>。
     *
     * <p><b>完整性校验</b>：COUNT 与 END 均为**非负整数、无溢出、数值相同**；实际数据行数严格为 N；
     * 每条合法且 userId 不重复；END 后无额外记录。任一不满足 ⇒ {@code complete=false}，
     * 但**可解析条目仍返回**供查看 —— 绝不被「跳过坏行」提升为完整。
     */
    @NonNull
    private static RegistrySnapshot parseRegistryV2(@NonNull List<String> lines) {
        List<Entry> out = new ArrayList<>();
        int skipped = 0;
        Long declared = (lines.size() > 1) ? parseCountField(lines.get(1).trim(), COUNT_PREFIX) : null;

        if (declared == null) {
            // 缺 COUNT 行 / COUNT 非法（负数 / 溢出 / 非数字）：无法确定数据边界，仅尽力取可解析行供查看。
            for (int i = 1; i < lines.size(); i++) {
                String line = lines.get(i).trim();
                if (line.isEmpty()) {
                    continue;
                }
                Entry parsed = parseEntry(line);
                if (parsed == null) {
                    skipped++;
                    continue;
                }
                out.add(parsed);
            }
            RegistrySnapshot snap = new RegistrySnapshot(Status.OK, out, skipped,
                    "注册表 COUNT 行缺失或非法");
            snap.authoritative = false;
            snap.formatVersion = 2;
            snap.complete = false;
            return snap;
        }

        long n = declared;
        // 溢出保护：条目数超出 int 范围一律视为不完整（不按该值分配 / 迭代）。
        int nInt = (n <= Integer.MAX_VALUE) ? (int) n : -1;
        final int dataStart = 2;
        boolean dup = false;
        java.util.HashSet<String> seenIds = new java.util.HashSet<>();
        if (nInt >= 0) {
            for (int i = 0; i < nInt && dataStart + i < lines.size(); i++) {
                String line = lines.get(dataStart + i).trim();
                if (line.isEmpty()) {
                    skipped++;
                    continue;
                }
                Entry parsed = parseEntry(line);
                if (parsed == null) {
                    skipped++;
                    continue;
                }
                if (!seenIds.add(parsed.userId)) {
                    dup = true;
                    skipped++;   // 重复 userId：计入跳过，保证 complete 必为 false
                    continue;
                }
                out.add(parsed);
            }
        }

        // 期望结构：2 行头 + N 行数据 + 1 行 END。
        boolean sizeOk = nInt >= 0 && lines.size() == (2L + nInt + 1L);
        boolean endOk = false;
        // 先用 long 计算头尾偏移，避免 COUNT 接近 int 上限时溢出为负索引。
        long endIndex = (long) dataStart + nInt;
        if (nInt >= 0 && endIndex < lines.size()) {
            Long endN = parseCountField(lines.get((int) endIndex).trim(), END_PREFIX);
            endOk = (endN != null && endN == n);
        }
        // END 之后的额外非空行计入 skipped（可见性）。
        if (nInt >= 0) {
            for (long i = endIndex + 1; i < lines.size(); i++) {
                if (!lines.get((int) i).trim().isEmpty()) {
                    skipped++;
                }
            }
        }

        boolean complete = nInt >= 0 && sizeOk && endOk && (skipped == 0) && !dup;
        String msg = complete ? "" : describeV2Incomplete(sizeOk, endOk, dup, skipped);
        RegistrySnapshot snap = new RegistrySnapshot(Status.OK, out, skipped, msg);
        snap.authoritative = complete;
        snap.formatVersion = 2;
        snap.complete = complete;
        return snap;
    }

    /** 解析 {@code COUNT N} / {@code END N} 行；前缀不符 / 负数 / 溢出 / 非数字时返回 null。 */
    @Nullable
    private static Long parseCountField(@NonNull String line, @NonNull String prefix) {
        String[] t = line.split("\\s+");
        if (t.length != 2 || !prefix.equals(t[0])) {
            return null;
        }
        try {
            long v = Long.parseLong(t[1]);
            return (v < 0) ? null : v;
        } catch (Exception ex) {
            // 含数值溢出（Long.parseLong 抛 NumberFormatException）与非数字。
            return null;
        }
    }

    @NonNull
    private static String describeV2Incomplete(boolean sizeOk, boolean endOk, boolean dup, int skipped) {
        StringBuilder sb = new StringBuilder("注册表枚举不完整");
        List<String> reasons = new ArrayList<>();
        if (!sizeOk) {
            reasons.add("数据行数与 COUNT 不符或缺少 END/存在额外记录");
        }
        if (!endOk) {
            reasons.add("END 标记缺失或与 COUNT 不一致");
        }
        if (dup) {
            reasons.add("存在重复 userId");
        }
        if (skipped > 0) {
            reasons.add("有 " + skipped + " 行无法解析");
        }
        if (!reasons.isEmpty()) {
            sb.append("：").append(String.join("、", reasons));
        }
        return sb.toString();
    }

    // 静默重命名（崩溃恢复用）已收敛到 FileStoreUtils.renameQuietly（任务 A4c），本类不再自带实现。

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
     * 注册表**未消费的临时文件**（与主文件同目录）。
     *
     * <p>★ 2026-09-28 抽出（原为 {@code persistRegistry} 内的内联构造）：崩溃恢复也要用它
     * —— 见 {@link #loadRegistry()} 的「优先恢复 .tmp」一段。两处必须用**同一命名**，
     * 故收敛到这里，避免日后只改一处导致恢复静默失效。
     */
    @NonNull
    private static File tmpFileFor(@NonNull File registryFile) {
        return new File(registryFile.getParentFile(), registryFile.getName() + ".tmp");
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
        // A8：仅**完整 FREG2** 备份可被提升为基线；旧版 FREG1 或截断/损坏的备份**不提升、也不消费**
        //（如实报错并保留原件），避免把不可判重的副本当作可回滚基线后又被后续写入覆盖。
        if (!dst.exists() && bak.isFile()) {
            RegistrySnapshot bakSnap = parseRegistryFile(bak);
            if (!(bakSnap.status == Status.OK && bakSnap.formatVersion == 2 && bakSnap.complete)) {
                return Status.IO_ERROR;
            }
            if (!FileStoreUtils.renameQuietly(bak, dst)) {
                return Status.IO_ERROR;
            }
        }

        File tmp = tmpFileFor(dst);
        int count = entries.size();
        StringBuilder sb = new StringBuilder();
        sb.append(REGISTRY_MAGIC_V2).append('\n');
        sb.append(COUNT_PREFIX).append(' ').append(count).append('\n');
        for (Entry e : entries) {
            // userId 以可逆 hex 编码落盘（非哈希）：list 预览需要展示标识，见类级 javadoc 取舍说明。
            sb.append(toHex(e.userId.getBytes(StandardCharsets.UTF_8))).append(' ')
                    .append(e.modelVersion).append(' ')
                    .append(e.templateSchemaVersion).append(' ')
                    .append(e.templateFileSize).append(' ')
                    .append(e.templateFileSha256Hex).append(' ')
                    .append(e.createdAtMs).append('\n');
        }
        // 末行结束标记：与 COUNT 相同的条目数，作为完整性尾校验。
        sb.append(END_PREFIX).append(' ').append(count).append('\n');
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
