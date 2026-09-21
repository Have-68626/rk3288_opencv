package com.example.rk3288_opencv;

import android.content.Context;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * FaceModelManager —— Android 模型管理器（任务 14）。
 *
 * <p>P1-6 路线下 <b>APK 不内置任何模型权重</b>：首启无可用模型，由用户通过 SAF 选择一个
 * 包含 {@code manifest.json} 的 ZIP 包导入。本类负责该导入流程的全部安全校验、原子启用，
 * 以及当前 profile 的切换；任何失败都<b>保留原配置</b>，首启失败则保持「未导入」态。
 *
 * <h2>ZIP 导入流程（顺序即安全边界）</h2>
 * <ol>
 *   <li><b>解压前</b>逐条校验 entry 名：拒绝绝对路径/盘符/{@code ..}/反斜杠（Zip Slip），
 *       并用 canonical 路径做包含性兜底；文件数上限 {@value #MAX_FILE_COUNT}；
 *       解压累计字节上限 <b>256 MiB</b>（{@value #MAX_TOTAL_BYTES} 字节，边写边判、不先落盘全量）。</li>
 *   <li>解压到同盘临时目录 {@code .tmp_<nano>}；streaming 写出，超限立即中止并删除临时目录。</li>
 *   <li>在临时目录内唯一定位 {@code manifest.json}，其所在目录即 manifest 基目录。</li>
 *   <li>用 {@link FaceModelManifest#parse} 做 schemaVersion==1 / ABI / 路径 / sha256 格式校验
 *       （与 native 契约一致），并要求 {@code licenseAccepted==true}。</li>
 *   <li>对 manifest 引用的 param/bin 逐个校验：存在、非 Git LFS pointer、SHA-256 与引脚一致。</li>
 *   <li><b>native 校验/暂存</b>：把 manifest 原文交给 {@link NativeRuntime#loadProfile} 做格式/ABI
 *       校验（<b>只校验暂存，不等于加载生效</b>），非 validated 立即失败，此时旧目录与激活指针完全未被触碰。</li>
 *   <li><b>原子启用目录</b>：把基目录 rename 到 {@code <storeDir>/<profileId>}（旧目录先 rename 为 .bak），
 *       <b>暂不</b>写激活指针。全程同盘 rename，保证原子性。</li>
 *   <li><b>激活并落定</b>：目录稳定后调 {@link NativeRuntime#activateRuntime} 绑定实时管线；仅当 native
 *       返回 {@code ok:true} 才原子落定激活指针 {@code active_profile}。激活失败时<b>同名覆盖</b>回滚目录到
 *       旧版本（{@code .bak_<id>} → {@code <id>}，与 {@link #activate} 路径语义一致）、<b>首次导入</b>保留
 *       新目录但不切指针（旧配置继续生效），均返回 {@link Status#RUNTIME_ACTIVATE_FAILED}，绝不静默报成功；
 *       指针落定失败则回滚目录到导入前状态。</li>
 * </ol>
 *
 * <h2>切换 profile</h2>
 * {@link #activate} 对已导入目录重新执行第 4–6 步校验，再原子改写激活指针；失败时指针不变，
 * 即保留原配置。
 *
 * <h2>存储布局</h2>
 * <pre>
 * files/face_models/
 *   active_profile            当前激活的 profileId；本版本起为 `profileId=<id>` 两行格式，
 *                             第二行可选 `txnId=<32hex>`（本次事务的提交代次）。旧格式
 *                             （纯 profileId 一行）仍可读，视为「无提交证据」。
 *   active_profile.tmp/.bak   指针的原子写临时/备份
 *   &lt;profileId&gt;/manifest.json
 *   &lt;profileId&gt;/scrfd.param ...（模型文件）
 *   .tmp_&lt;nano&gt;/              解压临时目录（下次导入时清扫；动态名）
 *   .staging/                  临时/暂存文件目录（**名字恒定**，不含 profileId）：
 *     .staging/txn/&lt;profileId&gt;      事务记录落盘前的临时文件（原 .txn_&lt;id&gt;.tmp）
 *     .staging/commit/&lt;profileId&gt;   提交见证落盘前的临时文件（原 .commit_&lt;id&gt;.tmp）
 *                              下次导入时**整体清空**——里面只可能是临时文件，故清扫是纯正语义。
 *                              （名字不含 profileId ⇒ 结构上不可能与任何 profile 的正式产物同名。）
 *   .bak_&lt;profileId&gt;/         目录替换时移开的旧配置备份
 *   .txn_&lt;profileId&gt;          目录替换**事务记录**（profileId/hadOld/prevActive[/txnId]/state，任务 A）
 *   .commit_&lt;profileId&gt;       **提交见证**（profileId/txnId）：本次替换已提交的持久证据。
 *                             按 profileId 固定命名、**不随激活指针移动**——指针里的代次会被下一次
 *                             导入/切换覆盖，见证不会（2026-09-18 修复「静默回滚」缺口）。
 *   .rollback_&lt;profileId&gt;     旧版回滚意图标记（已非主路径，仅保留兼容与恢复）
 * </pre>
 *
 * <p>本类不写任何模型内容到日志；错误消息只含状态码语义与相对文件名。
 */
public final class FaceModelManager {

    /** SAF 导入 ZIP 的文件数上限（一个 profile 仅需 manifest + 4 个模型文件，留足余量）。 */
    public static final int MAX_FILE_COUNT = 64;

    /** 解压总大小上限：256 MiB。 */
    public static final long MAX_TOTAL_BYTES = 256L * 1024L * 1024L;

    /** manifest.json 体量上限（防止把超大文件当 manifest 读入内存）。 */
    public static final long MAX_MANIFEST_BYTES = 1L * 1024L * 1024L;

    /** manifest 文件名（ZIP 内唯一）。 */
    public static final String MANIFEST_FILE_NAME = "manifest.json";

    /** 当前激活 profile 的指针文件名。 */
    public static final String ACTIVE_POINTER_FILE = "active_profile";

    /**
     * 回滚意图标记前缀（第五轮审查 P1）：仅当「目录回滚时新目录删不掉」才写入 {@code .rollback_<id>}。
     *
     * <p>标记存在的语义是：{@code <profileId>} 目录是**激活失败的新包**、原模型仍滞留在
     * {@code .bak_<id>}，必须在启动/状态查询时先删新目录、再把备份还原就位。
     */
    private static final String ROLLBACK_MARKER_PREFIX = ".rollback_";

    /**
     * 提交见证文件名前缀：{@code <storeDir>/.commit_<profileId>}（点号前缀 → 被 {@code listProfiles}
     * 忽略）。内容 {@code profileId=} + {@code txnId=}，与事务记录同格式。
     *
     * <p>包级可见：单元测试用它构造/断言见证现场（与 {@link #ACTIVE_POINTER_FILE} 同例）。
     */
    static final String COMMIT_WITNESS_PREFIX = ".commit_";

    /**
     * 暂存目录名：{@code <storeDir>/.staging}（点号前缀 → 被 {@code listProfiles} 忽略）。
     *
     * <p>所有**由 profileId 拼成的临时文件**都放进这里（{@code .staging/txn/<profileId>}、
     * {@code .staging/commit/<profileId>}）。子目录名与文件名都不含额外的点号后缀，而 {@code .staging}
     * 以点号开头（不可能等于任何合法 profileId 目录），故**结构上**不可能与任何 profile 的正式产物
     * （{@code <profileId>/}、{@code .txn_<id>}、{@code .commit_<id>}）同名——这正是旧方案
     * （顶层 {@code .txn_<id>.tmp}）的漏洞：{@code isSafeDirName} 放行点号，profileId 恰为
     * {@code foo.tmp} 时，它的活记录 {@code .txn_foo.tmp} 与「profileId 为 foo 的记录临时文件」逐字同名。
     *
     * <p>包级可见：单元测试用它断言「暂存目录无残留」（与 {@link #COMMIT_WITNESS_PREFIX} 同例）。
     */
    static final String STAGING_DIR = ".staging";

    /** 暂存目录内的事务记录临时文件子目录：{@code .staging/txn/<profileId>}。 */
    private static final String STAGING_TXN_DIR = "txn";

    /** 暂存目录内的提交见证临时文件子目录：{@code .staging/commit/<profileId>}。 */
    private static final String STAGING_COMMIT_DIR = "commit";

    /** 默认存储目录名（相对 {@code files/}）。 */
    private static final String DEFAULT_STORE_DIR = "face_models";

    /** gallery（已注册人员模板）目录名，与 {@code FeatureTemplateEncryptedStore.defaultDir} 一致。 */
    private static final String GALLERY_DIR_NAME = "face_templates";

    /** Git LFS pointer 首行特征（模型未拉取）。 */
    private static final String LFS_MARKER = "version https://git-lfs.github.com/spec/v1";

    // 目录锁与静默改名已收敛到 FileStoreUtils（任务 A4c）；本类的 dirLock() 走其共享锁表。

    /** native 运行时依赖；抽成接口以便纯 JVM 单测注入假实现（真实实现会加载 so）。 */
    public interface NativeRuntime {
        /**
         * 见 {@link NativeFaceBridge#loadProfile(String, String)}：仅做 manifest 格式/ABI 校验并暂存，
         * 返回 {@code validated:true} 表示校验通过；<b>不代表</b>推理管线已加载生效。
         */
        @NonNull String loadProfile(@NonNull String manifestJson, @NonNull String currentAbi);

        /** 见 {@link NativeFaceBridge#getRuntimeStatus()}。 */
        @NonNull String getRuntimeStatus();

        /**
         * 见 {@link NativeFaceBridge#activateRuntime(String, String, String, String)}：把已校验的
         * manifest 与模型基目录绑定到原生实时管线，使识别真正生效（否则 Android 端恒 missing_model）。
         *
         * <p>默认实现返回失败 JSON：纯 JVM 单测的假实现无需感知实时管线绑定（绑定为运行时增强，
         * 失败不阻断导入；真实实现见 {@link #DEFAULT_RUNTIME}）。
         */
        @NonNull
        default String activateRuntime(@NonNull String manifestJson, @NonNull String baseDir,
                                       @NonNull String currentAbi, @NonNull String galleryDir) {
            return "{\"ok\":false,\"error\":{\"code\":\"FACE_NOT_IMPLEMENTED\","
                    + "\"message\":\"activateRuntime 未被该实现支持\"}}";
        }
    }

    /** 默认实现：惰性引用 {@link NativeFaceBridge}（构造本管理器不会触发 native 库加载）。 */
    private static final NativeRuntime DEFAULT_RUNTIME = new NativeRuntime() {
        @Override
        @NonNull
        public String loadProfile(@NonNull String manifestJson, @NonNull String currentAbi) {
            return NativeFaceBridge.loadProfile(manifestJson, currentAbi);
        }

        @Override
        @NonNull
        public String getRuntimeStatus() {
            return NativeFaceBridge.getRuntimeStatus();
        }

        @Override
        @NonNull
        public String activateRuntime(@NonNull String manifestJson, @NonNull String baseDir,
                                      @NonNull String currentAbi, @NonNull String galleryDir) {
            return NativeFaceBridge.activateRuntime(manifestJson, baseDir, currentAbi, galleryDir);
        }
    };

    /** 管理操作结果码。 */
    public enum Status {
        OK,
        /** 未导入任何模型（首启默认态，需要引导 SAF 导入）。 */
        NOT_IMPORTED,
        INVALID_ARGUMENT,
        /** ZIP entry 为绝对路径/含 {@code ..}/反斜杠，或 canonical 路径逃出临时目录。 */
        ZIP_SLIP,
        TOO_MANY_FILES,
        TOO_LARGE,
        MISSING_MANIFEST,
        /** manifest 非法（JSON 坏、schema 非 1、路径不安全、sha256 格式错、字段缺失等）。 */
        MANIFEST_INVALID,
        /** 用户未确认许可。 */
        LICENSE_NOT_ACCEPTED,
        /** manifest 引脚 SHA-256 与实际文件不符。 */
        SHA256_MISMATCH,
        /** manifest 引用的模型文件缺失或为 LFS pointer。 */
        MISSING_MODEL_FILE,
        ABI_MISMATCH,
        /** native manifest 校验/暂存（loadProfile）失败；不代表加载生效判定。 */
        NATIVE_CHECK_FAILED,
        /** 实时管线激活失败（native reload 未通过）：激活指针保留旧值，旧配置继续生效。 */
        RUNTIME_ACTIVATE_FAILED,
        IO_ERROR,
        ATOMIC_RENAME_FAILED,
        /** 切换目标 profile 未导入。 */
        PROFILE_NOT_FOUND
    }

    /** 单次操作结果。 */
    public static final class Result {
        @NonNull public final Status status;
        @NonNull public final FaceRuntimeStatus runtimeStatus;
        /** 中文消息；只含状态语义，不含模型内容。 */
        @NonNull public final String message;

        Result(@NonNull Status status, @NonNull FaceRuntimeStatus runtimeStatus, @NonNull String message) {
            this.status = status;
            this.runtimeStatus = runtimeStatus;
            this.message = message;
        }

        public boolean isOk() {
            return status == Status.OK;
        }
    }

    /** 可注入限额（仅单测用于以小限额验证上限逻辑，生产用默认值）。 */
    static final class Limits {
        final int maxFileCount;
        final long maxTotalBytes;
        final long maxManifestBytes;

        Limits(int maxFileCount, long maxTotalBytes, long maxManifestBytes) {
            this.maxFileCount = maxFileCount;
            this.maxTotalBytes = maxTotalBytes;
            this.maxManifestBytes = maxManifestBytes;
        }

        static Limits defaults() {
            return new Limits(MAX_FILE_COUNT, MAX_TOTAL_BYTES, MAX_MANIFEST_BYTES);
        }
    }

    /** 内部受控失败：携带对外状态码与中文消息。 */
    private static final class ImportFailure extends Exception {
        final Status status;

        ImportFailure(@NonNull Status status, @NonNull String message) {
            super(message);
            this.status = status;
        }
    }

    @NonNull private final File storeDir;
    @NonNull private final NativeRuntime runtime;
    @NonNull private final Limits limits;

    /**
     * #3（第三波审查）状态快照缓存：<b>仅</b>由后台线程经 {@link #status()} 写入，UI 线程经
     * {@link #statusSnapshot()} 无锁读取。<b>不</b>与 {@link #dirLock()} 关联——否则 UI 线程会在
     * 导入/切换持锁（其间含 native 自检与基准推理）时被整段阻塞而 ANR。volatile 保证跨线程可见性。
     */
    @Nullable private volatile FaceRuntimeStatus statusCache;

    public FaceModelManager(@NonNull File storeDir, @NonNull NativeRuntime runtime) {
        this(storeDir, runtime, Limits.defaults());
    }

    @VisibleForTesting
    FaceModelManager(@NonNull File storeDir, @NonNull NativeRuntime runtime, @NonNull Limits limits) {
        this.storeDir = storeDir;
        this.runtime = runtime;
        this.limits = limits;
    }

    /** 默认实例：存储目录 {@code files/face_models}，native 走 {@link NativeFaceBridge}。 */
    @NonNull
    public static FaceModelManager forDefault(@NonNull Context ctx) {
        return new FaceModelManager(new File(ctx.getFilesDir(), DEFAULT_STORE_DIR), DEFAULT_RUNTIME);
    }

    @NonNull
    public File storeDir() {
        return storeDir;
    }

    /** 某个 profileId 的落盘目录（不保证存在）。 */
    @NonNull
    public File profileDir(@NonNull String profileId) {
        return new File(storeDir, profileId);
    }

    /** 当前激活 profileId；未激活返回 null。 */
    @Nullable
    public String activeProfileId() {
        synchronized (dirLock()) {
            return readActive();
        }
    }

    /** 已导入的 profileId 列表（含未激活者），按字典序。 */
    @NonNull
    public List<String> listProfiles() {
        synchronized (dirLock()) {
            List<String> out = new ArrayList<>();
            File[] children = storeDir.listFiles();
            if (children == null) {
                return out;
            }
            for (File c : children) {
                String name = c.getName();
                if (!c.isDirectory() || name.startsWith(".")) {
                    continue;
                }
                if (FaceModelManifest.isSafeDirName(name)
                        && new File(c, MANIFEST_FILE_NAME).isFile()) {
                    out.add(name);
                }
            }
            Collections.sort(out);
            return out;
        }
    }

    /**
     * SAF 导入入口：从 ZIP 流导入一个 profile 并原子启用。
     *
     * <p>{@code in} 由本方法负责关闭（ZipInputStream 会关闭底层流）。
     *
     * @param currentAbi       当前设备 ABI（如 {@code arm64-v8a}），必须出现在 manifest 的
     *                          supportedAbis 中，也用于 native 自检
     * @param licenseAccepted  用户是否已确认模型许可；为 false 直接拒绝导入
     */
    @NonNull
    public Result importZip(@NonNull InputStream in, @NonNull String currentAbi,
                            boolean licenseAccepted) {
        synchronized (dirLock()) {
            if (in == null) {
                return fail(Status.INVALID_ARGUMENT, "ZIP 输入流为空");
            }
            if (currentAbi == null || currentAbi.trim().isEmpty()) {
                return fail(Status.INVALID_ARGUMENT, "currentAbi 为空");
            }
            if (!licenseAccepted) {
                return fail(Status.LICENSE_NOT_ACCEPTED, "请先确认模型许可后再导入");
            }
            if (!ensureStoreDir()) {
                return fail(Status.IO_ERROR, "无法创建模型存储目录");
            }
            // 任务 A4：先扫掉上次残留的解压临时目录（本方法持 dirLock，不会误删本次的）。
            sweepStaleTempDirs();

            File tmpDir = new File(storeDir, ".tmp_" + System.nanoTime());
            if (!tmpDir.mkdirs()) {
                return fail(Status.IO_ERROR, "无法创建临时目录");
            }
            try {
                // ①-② 安全解压（Zip Slip / 文件数 / 总大小）。
                extractZip(in, tmpDir);
                // ③ 唯一定位 manifest，确定基目录。
                File manifestFile = locateSingleManifest(tmpDir);
                String baseDir = manifestFile.getParentFile().getCanonicalPath();
                File base = new File(baseDir);
                String manifestJson = readTextFile(manifestFile, limits.maxManifestBytes);

                // ④ manifest 契约校验 + 许可确认（许可已在入口确认）。
                FaceModelManifest.Profile profile = validateManifest(manifestJson, currentAbi);

                // ⑤ 四个模型文件存在性 + LFS + sha256 引脚校验。
                verifyModelFiles(base, profile);

                // ⑥ native 校验/暂存：只判 manifest 格式/ABI，失败时磁盘/指针未被改动。
                nativeValidateProfile(manifestJson, currentAbi);

                // Codex 要求 5：存在未了结的替换现场时**禁止继续覆盖导入**（先尽力恢复一次）。
                String blocked = unresolvedSwapReason(profile.profileId);
                if (blocked != null) {
                    return fail(Status.ATOMIC_RENAME_FAILED, blocked);
                }

                // 记录旧激活指针：激活/落定失败时保留旧配置。
                // ⚠️ **必须在上面那次恢复之后读**（与 activate() 的既有顺序一致）：恢复可能改写指针
                //（旧格式「补做指针还原」这条路径），提前读会把**陈旧值**写进事务记录的 prevActive、
                //并交给 bestEffortReactivate，使回滚目标偏差一次。（A2 审计 M-1）
                String previousActive = readActive();

                // ⑦ 目录原子替换（rename 基目录到 <profileId>），但**先不写激活指针**：
                //    只有实时管线激活成功才落定指针（#2）。
                ProfileSwap swap = swapInProfileDirs(base, profile.profileId, previousActive);

                // ⑧ 激活实时管线：目录已稳定，用最终 profileId 目录绑定（临时目录随后被清理）。
                //    native 侧候选管线先 reload、成功才提交，失败时旧管线原样保留。
                ActivateOutcome ao = nativeActivateRuntime(manifestJson, swap.target, currentAbi);
                if (!ao.accepted()) {
                    // 激活失败（native 候选管线未提交，旧管线/旧配置原样保留）：
                    // - #1（第三波审查）同名覆盖：旧目录此刻位于 swap.bak（.bak_<id>），必须回滚就位，
                    //   否则删掉旧目录、而 active_profile 仍指同名 <profileId>（其内容已被新包覆盖）
                    //   -> 旧模型永久丢失；重启后 readActive 只判目录存在性 -> 谎报 ready 再激活失败。
                    //   回滚后与 activate 路径（不删文件、保留原配置）语义一致。
                    // - 首次导入：新目录是唯一副本，保留之（不切指针 -> 仍为未导入引导态）。
                    if (swap.hadOld) {
                        // #1（第三波审查）：回滚结果必须显式检查，绝不静默当作「已回滚成功」。
                        // 回滚失败时旧目录滞留 .bak_<id>，而 active_profile 仍指向同名 <profileId>
                        // ——磁盘可能残留失败模型，重启后可能加载错误模型。
                        // 此处返回 ATOMIC_RENAME_FAILED（与 persistActive 失败路径、swapInProfileDirs
                        // 失败路径的失败码/文案语义一致），并保留 .bak_<id>，交由随后的
                        // buildStatus()->recoverProfileDirIfNeeded() 在状态查询/启动时把它还原就位。
                        // 第五轮审查 P1：两种失败现场都必须可被启动恢复覆盖——target 已删除（缺目标）
                        // 与 target 删不掉（回滚时已写入 .rollback_<id> 意图标记），故「将由启动恢复」
                        // 现在是可兑现的承诺，而不再是空头支票。
                        RollbackOutcome ro = rollbackProfileSwap(swap);
                        if (ro != RollbackOutcome.RESTORED) {
                            FaceRuntimeStatus st = buildStatus(nativeStatusOrNull());
                            // 文案按**回滚返回值**（而非磁盘反推）与落盘后状态生成：
                            // · 备份已被消费 → 回滚其实已就位（buildStatus 的 recover 可能刚就地完成）；
                            // · FAILED_RECORDED → 已留意图标记，启动恢复可收尾；
                            // · FAILED_UNRECORDED → 已尝试回滚但未成功、且未能记录意图，绝不承诺自动恢复。
                            // 注意：文案直接呈现给用户，不得混入 Markdown 强调符号。
                            String detail;
                            if (!swap.bak.exists()) {
                                detail = "，旧配置已回滚就位";
                            } else if (ro == RollbackOutcome.FAILED_RECORDED) {
                                detail = "且旧配置回滚失败（旧目录滞留于 " + swap.bak.getName() + "，将由启动恢复）";
                            } else {
                                detail = "且旧配置回滚失败，且未能记录恢复意图（旧目录滞留于 "
                                        + swap.bak.getName() + "）——请排除磁盘/权限问题后重试导入";
                            }
                            return new Result(Status.ATOMIC_RENAME_FAILED, st,
                                    "模型已导入，但实时管线激活失败：" + ao.reason + "，" + detail);
                        }
                    } else {
                        // 首次导入无旧配置：新目录保留（不切指针 → 仍为未导入引导态），事务就地了结——
                        // 否则残留的未提交记录会让 buildStatus 误报「事务未完成」。
                        deleteRecursivelyQuietly(swap.bak);
                        finishTxn(profile.profileId);
                    }
                    // #1-Low-2（第三波审查）：按是否有旧同名配置区分失败文案，避免首次导入也误报
                    // 「已保留原配置」（此时并无旧配置）。
                    String tail = swap.hadOld
                            ? "已回滚到原模型"
                            : "已保留新导入的模型目录但未激活";
                    return new Result(Status.RUNTIME_ACTIVATE_FAILED, buildStatus(nativeStatusOrNull()),
                            "模型已导入，但实时管线激活失败：" + ao.reason + "，" + tail);
                }
                // ⑧.5 提交见证：把「本次替换已提交」这条事实先落到一个**不随激活指针移动**的位置。
                //   为什么必须先于 ⑨：⑨ 写入的指针会被下一次导入/切换覆盖，一旦被覆盖，「本次已提交」
                //   就再无任何盘上证据——此后触碰本 profile 只能判「未提交」，把一次成功的替换**静默
                //   回滚**（第三代理 2026-09-18 实证复现）。见证按 profileId 固定命名，与指针无关。
                //   写不进去 ⇒ 与「in_progress 记录写不进去就绝不动手」同一条原则：**绝不越过提交点**。
                if (!writeCommitWitness(swap.profileId, swap.txnId)) {
                    RollbackOutcome ro = rollbackProfileSwap(swap);
                    if (ro != RollbackOutcome.RESTORED) {
                        // 与下方 ⑨ 分支同口径：区分「已留意图标记，启动可收尾」与「未能记录意图」。
                        // 不承诺自动恢复，也不谎称已回滚。注意文案直接呈现给用户，不得含 Markdown 强调符号。
                        String tail = (ro == RollbackOutcome.FAILED_RECORDED)
                                ? "，将由启动恢复"
                                : "，且未能记录恢复意图（请排除磁盘/权限问题后重试导入）";
                        return fail(Status.ATOMIC_RENAME_FAILED,
                                "无法记录提交见证，且旧配置回滚失败（旧目录滞留于 "
                                        + swap.bak.getName() + tail + "）");
                    }
                    bestEffortReactivate(previousActive, currentAbi);
                    return fail(Status.ATOMIC_RENAME_FAILED, swap.hadOld
                            ? "无法记录提交见证，已回滚配置"
                            : "无法记录提交见证；已保留新导入的模型目录但未激活（可重新导入）");
                }

                // ⑨ 生效后才原子落定激活指针；落定失败则回滚目录到导入前状态。
                // 传入本次事务代次：它与 ⑧.5 的见证共同构成提交证据，恢复据此判定「已提交」。
                if (!persistActive(profile.profileId, swap.txnId)) {
                    // 见证已落盘 ⇒ 按本协议**不得回滚**：回滚会让「盘上证据说已提交」与「现场已回滚」
                    // 自相矛盾，下一次恢复会据此把已回滚的现场再翻成「保留新目录」。故先撤销见证，
                    // 撤销成功才回滚；撤销不掉就保留新目录、如实报「未激活」（可重新导入）。
                    if (!clearCommitWitness(swap.profileId)) {
                        return fail(Status.ATOMIC_RENAME_FAILED,
                                "激活指针写入失败，且提交见证未能撤销——已保留新导入的模型目录且未激活（可重新导入）");
                    }
                    RollbackOutcome ro = rollbackProfileSwap(swap);
                    if (ro != RollbackOutcome.RESTORED) {
                        // 与 importZip 分支同口径：区分「已留意图标记，启动可收尾」与「未能记录意图」。
                        // 不承诺自动恢复，也不谎称已回滚。注意文案直接呈现给用户，不得含 Markdown 强调符号。
                        String tail = (ro == RollbackOutcome.FAILED_RECORDED)
                                ? "，将由启动恢复"
                                : "，且未能记录恢复意图（请排除磁盘/权限问题后重试）";
                        return fail(Status.ATOMIC_RENAME_FAILED,
                                "激活指针写入失败，且旧配置回滚失败（旧目录滞留于 "
                                        + swap.bak.getName() + tail + "）");
                    }
                    bestEffortReactivate(previousActive, currentAbi);
                    return fail(Status.ATOMIC_RENAME_FAILED, swap.hadOld
                            ? "激活指针写入失败，已回滚配置"
                            : "激活指针写入失败；已保留新导入的模型目录但未激活（可重新导入）");
                }
                // 事务提交（Codex 要求 3）：指针已落定 → 先把记录改写为 committed，再清备份，最后清记录。
                // 改写失败时**继续**而**不回滚**：此刻激活确已成功（native 提交 + 指针落定都在前），
                // 因「记录写失败」而回滚一个可用配置更糟。
                // **约束 1（2026-09-17 外部审查 P1 #1）**：上述「记录残留也会收敛到保留新目录」不再
                // 依赖「备份确实被清掉」这一前提。旧实现依赖它，一旦**备份也删不掉**，现场就退化成
                // 「in_progress + target + bak」，恢复按「未提交」把**一次成功的导入静默回滚**（而 native
                // 仍在跑新模型 → 磁盘/指针/native 三方不一致）。现在提交证据是 ⑧.5 的见证 + 指针里的
                // txnId：记录即便停在 in_progress，恢复也能据「见证/指针代次 == 事务代次」判定为已提交，
                // 只重试清理备份。**两处记录保留时见证也必须保留**（清记录一律走 finishTxn），否则证据
                // 会随指针移动再次丢失——那正是 2026-09-18 补上见证的原因。
                if (!writeTxn(new DirTxn(swap.profileId, swap.hadOld, previousActive, swap.txnId, true))) {
                    // 刻意**无操作**，也不据此回滚（理由见上）。显式写出分支而非忽略返回值，是为了让
                    // 「此处考虑过改写失败」在代码里可见。备份清理与记录清除照常执行：若备份也删不掉，
                    // 记录会留在 in_progress，但恢复仍不会回滚（提交证据已就位）。
                }
                deleteRecursivelyQuietly(swap.bak);
                if (!swap.bak.exists()) {
                    finishTxn(swap.profileId);
                }

                return new Result(Status.OK, buildStatus(nativeStatusOrNull()), "");
            } catch (ImportFailure f) {
                return fail(f.status, f.getMessage());
            } catch (Exception | StackOverflowError ex) {
                return fail(Status.IO_ERROR, "导入过程发生未预期错误");
            } finally {
                // 成功时 baseDir 已 rename 走，tmpDir 仅剩无用文件；失败时清理全部临时内容。
                deleteRecursivelyQuietly(tmpDir);
            }
        }
    }

    /**
     * 切换当前 profile：对已导入目录重新做 manifest/文件/native 校验，先绑定实时管线，
     * 仅当激活成功才原子改写激活指针；任一步失败都保持原激活配置不变。
     */
    @NonNull
    public Result activate(@NonNull String profileId, @NonNull String currentAbi,
                           boolean licenseAccepted) {
        synchronized (dirLock()) {
            if (isReservedProfileId(profileId)) {
                return fail(Status.INVALID_ARGUMENT, "非法 profileId");
            }
            if (currentAbi == null || currentAbi.trim().isEmpty()) {
                return fail(Status.INVALID_ARGUMENT, "currentAbi 为空");
            }
            if (!licenseAccepted) {
                return fail(Status.LICENSE_NOT_ACCEPTED, "请先确认模型许可后再切换");
            }
            File dir = profileDir(profileId);
            File manifestFile = new File(dir, MANIFEST_FILE_NAME);
            // #3 崩溃恢复：目录替换窗口崩溃后（target 缺失 + .bak_<id> 存在）先恢复就位。
            recoverProfileDirIfNeeded(profileId);
            // Codex 要求 5：未了结的替换现场（未提交事务 / 歧义并存）时禁止继续操作。
            String blocked = unresolvedSwapReason(profileId);
            if (blocked != null) {
                return fail(Status.ATOMIC_RENAME_FAILED, blocked);
            }
            if (!dir.isDirectory() || !manifestFile.isFile()) {
                return fail(Status.PROFILE_NOT_FOUND, "该配置尚未导入：" + profileId);
            }
            try {
                String manifestJson = readTextFile(manifestFile, limits.maxManifestBytes);
                FaceModelManifest.Profile profile = validateManifest(manifestJson, currentAbi);
                verifyModelFiles(dir, profile);
                nativeValidateProfile(manifestJson, currentAbi);

                String previousActive = readActive();

                // #2：先激活实时管线（native 候选管线先 reload、成功才提交 -> 失败时旧管线保留）；
                // 未成功前绝不动激活指针，因此旧配置完整保留。
                ActivateOutcome ao = nativeActivateRuntime(manifestJson, dir, currentAbi);
                if (!ao.accepted()) {
                    return new Result(Status.RUNTIME_ACTIVATE_FAILED, buildStatus(nativeStatusOrNull()),
                            "实时管线激活失败：" + ao.reason + "，已保留原配置");
                }
                // 生效后才落定指针；落定失败则 best-effort 把运行态回滚到旧 profile。
                // 约束 2-2：始终以**实参 profileId** 为目标，绝不改用 readActive() —— 上一次
                // recoverProfileDirIfNeeded 可能刚把指针改成别的配置，若改用它就会把用户明确选择的
                // 目标偷换成恢复结果。代次用**新生成的**（本次切换没有事务记录）：这样其它 profileId
                // 的残留事务在恢复时必然看到「指针代次 != 自己的代次」→ 判未提交 → 只回滚目录、
                // 不动指针，不会把用户刚选的配置翻回去。
                if (!persistActive(profile.profileId, newTxnId())) {
                    bestEffortReactivate(previousActive, currentAbi);
                    return fail(Status.ATOMIC_RENAME_FAILED, "激活指针写入失败，已保留原配置");
                }
                // 第五轮审查 A3' δ5：用户已显式激活该 profile 且成功——上一次「回滚未完成」的意图
                // 已被本次激活取代。不清标记的话，buildStatus 会报「回滚未完成」ERROR，与本次
                // OK 结果自相矛盾。上方 recoverProfileDirIfNeeded 在 target 可删时已就地完成回滚
                // 并清标记；此处覆盖的是「它没能清掉」（target 仍被占用等）的剩余情形。
                clearRollbackMarker(profile.profileId);
                return new Result(Status.OK, buildStatus(nativeStatusOrNull()), "");
            } catch (ImportFailure f) {
                return fail(f.status, f.getMessage());
            } catch (Exception | StackOverflowError ex) {
                return fail(Status.IO_ERROR, "切换过程发生未预期错误");
            }
        }
    }

    /** {@link #recoverForStartup()} 的结果。 */
    public static final class StartupRecovery {
        /** 恢复**之后**重新读到的激活 profileId；null = 未导入或恢复未完成（见 {@link #blockedReason}）。 */
        @Nullable public final String activeProfileId;
        /** 非 null 表示恢复未完成，调用方**不得**继续自动激活。 */
        @Nullable public final String blockedReason;

        StartupRecovery(@Nullable String activeProfileId, @Nullable String blockedReason) {
            this.activeProfileId = activeProfileId;
            this.blockedReason = blockedReason;
        }
    }

    /**
     * 启动自动激活闸门：**先完成目录事务恢复，再重新读取激活指针**，并对重读到的目标再确认一次
     * 无未决事务（约束 2-1）。
     *
     * <p>为什么需要这个入口：启动流程原先「先读指针拿目标、再调 {@link #activate}」，而恢复发生在
     * {@code activate} 内部——若恢复把指针改回旧配置，调用方手里仍是恢复前的值，它会把这个旧值
     * 重新激活并写回，**抵消掉恢复**（外部审查 P1 #2）。
     *
     * <p><b>用户主动切换不得使用本方法</b>：它重读指针，会把用户明确选择的目标替换为恢复结果
     * （约束 2-2）。显式切换请直接调 {@code activate(用户选择, ...)}。
     *
     * @return {@link StartupRecovery}；{@code blockedReason} 非 null 时调用方**必须停止**自动激活
     *         并如实报错——不猜测哪个配置该生效。
     */
    @NonNull
    public StartupRecovery recoverForStartup() {
        synchronized (dirLock()) {
            ActivePointer p0 = readActivePointer();
            if (p0 == null) {
                return new StartupRecovery(null, null);   // 未导入
            }
            recoverProfileDirIfNeeded(p0.profileId);
            // 恢复可能改写了指针（旧格式回滚会补做指针还原）→ **必须重读**，并对重读到的目标做
            // 未决事务检查（约束 2-1「先完成恢复，再重新读取」+ 约束 2-3）。
            //
            // 只在这里检查**一次**即可，前面再加一次同样的检查是冗余分支：恢复改写指针的唯一途径
            // 是旧格式的「补做指针还原」，而它一旦成功就清记录。所以「p0 的记录仍在」必然意味着
            // 指针没被改写（p1 == p0），本次检查必然命中。该冗余分支无法被任何用例区分（实测：
            // 删掉它全部用例仍绿），故刻意只保留一处判据。
            ActivePointer p1 = readActivePointer();
            if (p1 == null) {
                return new StartupRecovery(null, "恢复后激活指针缺失，无法确定应激活的配置");
            }
            String reason = unresolvedSwapReason(p1.profileId);
            if (reason != null) {
                return new StartupRecovery(null, reason);
            }
            return new StartupRecovery(p1.profileId, null);
        }
    }

    /**
     * 人工了结一个**歧义现场**（新旧配置并存、无法判定哪份应生效）：保留用户选定的那一份、
     * 删除另一份，并清除未决事务记录。
     *
     * <p>为什么必须有它（约束 1「证据不足时保留现场并报错，不猜测、不删除任一副本」）：只报错会把
     * 该 profileId **永久卡死**——{@code unresolvedSwapReason} 会拒绝后续所有导入。本方法把决定权
     * 交回用户，且**只在用户显式确认后才删除副本**。
     *
     * <p>前置：备份 {@code .bak_<id>} 必须存在——否则返回 {@code INVALID_ARGUMENT}，防止本方法被
     * 当成通用的删除入口误用。**当前目录缺失**时只允许「还原为备份」（此刻备份是唯一完整副本，
     * 而 {@code buildStatus} 会同时报歧义与目录缺失，若不允许了结就形成「报错却点了没用」的死角，
     * 见 A1 审计 N3）。
     *
     * <p>刻意**不**改写激活指针：目录内容换了，但「哪个 profile 处于激活位」没变，指针无需改动；
     * 且未决记录会在同一个临界区内被清掉，故不会留下「指针代次与某条记录相等」的假证据。
     *
     * @param keepNew true = 保留当前目录（删备份）；false = 还原为备份（删当前目录后把备份就位）
     */
    @NonNull
    public Result resolveAmbiguousSwap(@NonNull String profileId, boolean keepNew) {
        synchronized (dirLock()) {
            if (isReservedProfileId(profileId)) {
                return fail(Status.INVALID_ARGUMENT, "非法 profileId");
            }
            File target = profileDir(profileId);
            File bak = bakDirOf(profileId);
            if (!bak.exists()) {
                return fail(Status.INVALID_ARGUMENT, "该配置不存在「新旧并存」现场，无需了结");
            }
            if (!target.exists() && keepNew) {
                // 目标目录已缺失（外部删除 / 磁盘异常）：「保留当前目录」无从谈起——备份是唯一
                // 完整副本，只能还原它。显式拒绝并指出可用选项，避免用户「点了没反应」。
                return fail(Status.INVALID_ARGUMENT, "当前目录已缺失，只能选择「还原为备份」");
            }
            if (keepNew) {
                if (!fileOps.deleteRecursively(bak)) {
                    return fail(Status.ATOMIC_RENAME_FAILED,
                            "无法删除备份目录 " + bak.getName() + "，已保留现场");
                }
            } else {
                if (target.exists() && !fileOps.deleteRecursively(target)) {
                    return fail(Status.ATOMIC_RENAME_FAILED, "无法删除当前目录，已保留现场");
                }
                if (!fileOps.rename(bak, target)) {
                    return fail(Status.ATOMIC_RENAME_FAILED,
                            "备份目录无法还原就位（" + bak.getName() + "），已保留现场");
                }
            }
            // 了结即现场消失：未决事务记录、**提交见证**与**旧版回滚意图标记**都要清掉——否则
            // buildStatus 会因残留标记继续报「上次模型回滚未完成」ERROR（A1 审计 N7）；见证残留则
            // 会把「本次已提交」的假证据留给下一次同名事务。
            finishTxn(profileId);
            clearRollbackMarker(profileId);
            return new Result(Status.OK, buildStatus(nativeStatusOrNull()), "");
        }
    }

    /**
     * 当前运行时状态快照（含 native {@code getRuntimeStatus} 叠加）。
     * 首启无模型返回 {@link FaceRuntimeStatus.State#NOT_IMPORTED} 及引导文案。
     *
     * <p>本方法持 {@link #dirLock()} 并做文件 IO 与 native 调用，<b>只允许后台线程调用</b>；
     * UI 线程请改用 {@link #statusSnapshot()}（无锁读缓存），以免被导入/切换的持锁段阻塞。
     */
    @NonNull
    public FaceRuntimeStatus status() {
        FaceRuntimeStatus s;
        synchronized (dirLock()) {
            s = buildStatus(nativeStatusOrNull());
        }
        // #3：发布快照供 UI 线程无锁读取（volatile 写保证可见性）。仅后台线程到达此处。
        statusCache = s;
        return s;
    }

    /**
     * #3（第三波审查）：纯读状态快照，<b>不加目录锁、不做任何文件 IO / native 调用</b>，
     * 可在 UI 线程安全调用而不阻塞。
     *
     * <p>后台刷新线程（如 MainActivity 的 FaceModelStatus 线程）调用 {@link #status()} 会把结果写入
     * volatile 快照；本方法仅读该缓存，因此最多落后一个刷新周期。首帧（后台尚未产出快照）返回
     * 「加载中/未就绪」占位，绝不阻塞、绝不臆测为就绪。
     */
    @NonNull
    public FaceRuntimeStatus statusSnapshot() {
        FaceRuntimeStatus s = statusCache;
        return s != null ? s : loadingPlaceholder();
    }

    /**
     * 首帧占位：后台尚未产出状态快照时返回的「加载中/未就绪」态，<b>不</b>触发任何 IO/加锁。
     *
     * <p>#3-Low-1（第三波审查）：改用 {@link FaceRuntimeStatus.State#LOADING} 专用态（而非复用
     * {@code NOT_IMPORTED}），使 UI 渲染路径能区分「加载中」与「真未导入」，避免冷启动最多 2s
     * 误显「模型来源=未导入」。占位消息为「正在加载」，绝不臆测为未导入结论。
     */
    @NonNull
    private static FaceRuntimeStatus loadingPlaceholder() {
        return FaceRuntimeStatus.loading();
    }

    /** 校验用辅助：目录锁（与 AndroidEnrollmentRepository 同策略，便于单测断言共享）。 */
    @VisibleForTesting
    @NonNull
    static Object dirLockForTesting(@NonNull File storeDir) {
        return FileStoreUtils.dirLockFor(storeDir);
    }

    // ---------------- 校验 ----------------

    @NonNull
    private FaceModelManifest.Profile validateManifest(@NonNull String manifestJson,
                                                       @NonNull String currentAbi)
            throws ImportFailure {
        FaceModelManifest.ParseResult pr = FaceModelManifest.parse(manifestJson, currentAbi);
        if (!pr.isOk()) {
            // 仅将 ABI 不匹配单独暴露；其余按 manifest 非法处理。
            if (pr.status == FaceModelManifest.Status.ABI_MISMATCH) {
                throw new ImportFailure(Status.ABI_MISMATCH, pr.message);
            }
            throw new ImportFailure(Status.MANIFEST_INVALID, pr.message);
        }
        if (!FaceModelManifest.isSafeDirName(pr.profile.profileId)
                || isReservedProfileId(pr.profile.profileId)) {
            throw new ImportFailure(Status.MANIFEST_INVALID, "profileId 不能作为目录名");
        }
        return pr.profile;
    }

    /**
     * 保留目录名判定：profileId 会作为 {@code storeDir} 下的子目录名，必须避开
     * 激活指针文件（{@code active_profile}）与所有以 {@code .} 开头的内部过渡目录
     * （{@code .tmp_*} / {@code .bak_*}），否则会与指针/临时目录互相覆盖。
     */
    private static boolean isReservedProfileId(@NonNull String id) {
        return !FaceModelManifest.isSafeDirName(id)
                || id.startsWith(".")
                || ACTIVE_POINTER_FILE.equals(id);
    }

    private void verifyModelFiles(@NonNull File baseDir, @NonNull FaceModelManifest.Profile profile)
            throws ImportFailure {
        for (FaceModelManifest.Entry e : profile.allEntries()) {
            File f = new File(baseDir, e.relativePath);
            // 二次防御：manifest 路径已过 isSafeRelativePath，这里再用 canonical 兜底包含性。
            if (!isContained(baseDir, f)) {
                throw new ImportFailure(Status.MANIFEST_INVALID,
                        "模型路径逃出基目录：" + e.relativePath);
            }
            if (!f.isFile()) {
                throw new ImportFailure(Status.MISSING_MODEL_FILE, "缺失模型文件：" + e.relativePath);
            }
            if (isLfsPointer(f)) {
                throw new ImportFailure(Status.MISSING_MODEL_FILE,
                        "模型文件是 Git LFS pointer（未拉取）：" + e.relativePath);
            }
            if (!e.sha256.isEmpty()) {
                String actual = sha256Hex(f);
                if (actual == null || !actual.equalsIgnoreCase(e.sha256)) {
                    throw new ImportFailure(Status.SHA256_MISMATCH, "SHA-256 不匹配：" + e.relativePath);
                }
            }
        }
    }

    /**
     * native manifest 校验/暂存门禁（原 nativeSelfCheck，P1-2 澄清语义）：
     * {@link NativeRuntime#loadProfile} <b>只做格式/ABI 校验并暂存</b>，返回 {@code validated:true}。
     * 因此本方法只判「已校验」，<b>绝不</b>把它当作「已加载生效」——后者由
     * {@link #nativeActivateRuntime} 的 {@code effective}/{@code reason}（经 {@code getRuntimeStatus} 暴露）
     * 决定，并映射到 {@link FaceRuntimeStatus#nativeProfileActive}。
     */
    private void nativeValidateProfile(@NonNull String manifestJson, @NonNull String currentAbi)
            throws ImportFailure {
        String resp;
        try {
            resp = runtime.loadProfile(manifestJson, currentAbi);
        } catch (Throwable t) {
            // native 库缺失/加载失败也按校验失败处理，绝不把坏配置设为当前。
            throw new ImportFailure(Status.NATIVE_CHECK_FAILED, "native 模型校验不可用");
        }
        if (!isNativeValidated(resp)) {
            throw new ImportFailure(Status.NATIVE_CHECK_FAILED, "native 模型校验/暂存未通过");
        }
    }

    /**
     * 绑定实时管线并返回结果：把 manifest 与<b>稳定的</b>模型基目录交给 native
     * {@code activateRuntime}，使识别真正生效（否则 Android 端恒 {@code missing_model}，仅预览不识别）。
     *
     * <p>调用时机：目录原子就位之后、激活指针落定<b>之前</b>（#2）。native 侧采用「候选管线先
     * reload、成功才提交替换」，故本方法返回 {@code ok:false} 时旧管线与旧配置均原样保留，调用方
     * 必须据此保留/回滚指针，绝不静默报成功。
     *
     * <p>{@code ok:true} 表示 native 已接受激活；Engine 尚未创建时会返回 {@code pending:true}
     * （{@code effective:false}），属正常的「稍后补激活」，不算失败。
     *
     * @return native 激活结果（ok/effective/reason）；native 不可用或返回非法时 ok=false。
     */
    @NonNull
    private ActivateOutcome nativeActivateRuntime(@NonNull String manifestJson, @NonNull File baseDir,
                                                  @NonNull String currentAbi) {
        String resp;
        try {
            resp = runtime.activateRuntime(manifestJson, baseDir.getAbsolutePath(), currentAbi,
                    galleryDirPath());
        } catch (Throwable t) {
            // native 库缺失/不可用：按激活失败处理（调用方保留旧配置），绝不静默成功。
            return new ActivateOutcome(false, false, "native_unavailable");
        }
        return parseActivateOutcome(resp);
    }

    /** native {@code activateRuntime} 结果的状态视图（仅状态元数据，无模型内容）。 */
    @VisibleForTesting
    static final class ActivateOutcome {
        final boolean ok;
        final boolean effective;
        @NonNull final String reason;

        ActivateOutcome(boolean ok, boolean effective, @NonNull String reason) {
            this.ok = ok;
            this.effective = effective;
            this.reason = reason;
        }

        /**
         * 是否接受为激活成功：{@code ok:true} 且（已生效 或 属正常的 {@code pending}
         * 延后激活——Engine 尚未创建时由 nativeInit 补激活）。仅 {@code ok} 为真、
         * 又非 pending 的「未生效」不得视为成功，避免谎报已激活。
         */
        boolean accepted() {
            return ok && (effective || "pending".equals(reason));
        }
    }

    /** 解析 activateRuntime 返回 JSON 的 {@code ok}/{@code effective}/{@code reason}；解析失败即失败。 */
    @NonNull
    private static ActivateOutcome parseActivateOutcome(@Nullable String resp) {
        if (resp == null || resp.isEmpty()) {
            return new ActivateOutcome(false, false, "empty_response");
        }
        try {
            Map<String, Object> root = FaceModelManifest.asObject(
                    FaceModelManifest.parseJsonValue(resp));
            if (root == null) {
                return new ActivateOutcome(false, false, "bad_response");
            }
            Boolean ok = FaceModelManifest.asBoolean(root.get("ok"));
            Boolean effective = FaceModelManifest.asBoolean(root.get("effective"));
            String reason = FaceModelManifest.asString(root.get("reason"));
            if (reason == null || reason.isEmpty()) {
                reason = "unknown";
            }
            return new ActivateOutcome(ok != null && ok, effective != null && effective, reason);
        } catch (Exception ex) {
            return new ActivateOutcome(false, false, "bad_response");
        }
    }

    /**
     * best-effort：把实时管线重新绑定到旧激活 profile（仅当旧目录仍完整）。
     * 用于「新 profile 已生效但激活指针落定失败」时，避免内存运行态与磁盘指针不一致。
     * 失败静默：调用方已在上报指针写入失败，此处仅尽量恢复。
     */
    private void bestEffortReactivate(@Nullable String previousProfileId, @NonNull String currentAbi) {
        if (previousProfileId == null) {
            return;
        }
        File dir = profileDir(previousProfileId);
        File mf = new File(dir, MANIFEST_FILE_NAME);
        if (!dir.isDirectory() || !mf.isFile()) {
            return;
        }
        try {
            String oldManifest = readTextFile(mf, limits.maxManifestBytes);
            runtime.activateRuntime(oldManifest, dir.getAbsolutePath(), currentAbi, galleryDirPath());
        } catch (Throwable ignored) {
            // 回滚为尽力而为；失败不影响已上报的失败状态。
        }
    }

    /** gallery 目录绝对路径（仅作契约位传入；当前图库来自内存注册，native 侧不读该目录）。 */
    @NonNull
    private String galleryDirPath() {
        File parent = storeDir.getParentFile();
        File dir = new File(parent != null ? parent : storeDir, GALLERY_DIR_NAME);
        return dir.getAbsolutePath();
    }

    /** 解析 native 返回 JSON 的 {@code ok} 字段；解析失败视为失败。 */
    private static boolean isNativeOk(@Nullable String resp) {
        if (resp == null || resp.isEmpty()) {
            return false;
        }
        try {
            Map<String, Object> root = FaceModelManifest.asObject(FaceModelManifest.parseJsonValue(resp));
            if (root == null) {
                return false;
            }
            Boolean ok = FaceModelManifest.asBoolean(root.get("ok"));
            return ok != null && ok;
        } catch (Exception ex) {
            return false;
        }
    }

    /**
     * 判定 {@code loadProfile} 的响应是否为「已校验/暂存」：必须 {@code ok==true} 且
     * {@code validated==true}。仅 {@code ok} 为真不足以确认新版契约已生效，故一并严格要求，
     * 防止旧实现（只回 ok、不校验暂存）被误判为通过。
     */
    private static boolean isNativeValidated(@Nullable String resp) {
        if (!isNativeOk(resp)) {
            return false;
        }
        try {
            Map<String, Object> root = FaceModelManifest.asObject(FaceModelManifest.parseJsonValue(resp));
            if (root == null) {
                return false;
            }
            Boolean validated = FaceModelManifest.asBoolean(root.get("validated"));
            return validated != null && validated;
        } catch (Exception ex) {
            return false;
        }
    }

    // ---------------- 解压 ----------------

    private void extractZip(@NonNull InputStream in, @NonNull File tmpDir) throws ImportFailure {
        int fileCount = 0;
        long total = 0;
        byte[] buf = new byte[64 * 1024];
        try (ZipInputStream zis = new ZipInputStream(in)) {
            ZipEntry entry;
            while ((entry = zis.getNextEntry()) != null) {
                String name = entry.getName();
                // Zip Slip：ZIP 规范用 '/'，出现 '\' 视为路径穿越企图；绝对路径/盘符/.. 一律拒绝。
                // 目录条目同样必须先过路径校验再 continue，避免名为 "../evil/" 的目录条目绕过校验。
                if (name == null || name.isEmpty() || name.indexOf('\\') >= 0
                        || !FaceModelManifest.isSafeRelativePath(name)) {
                    throw new ImportFailure(Status.ZIP_SLIP, "ZIP 含非法路径条目");
                }
                if (entry.isDirectory()) {
                    continue;
                }
                fileCount++;
                if (fileCount > limits.maxFileCount) {
                    throw new ImportFailure(Status.TOO_MANY_FILES,
                            "ZIP 文件数超过上限 " + limits.maxFileCount);
                }
                File dst = new File(tmpDir, name);
                // 包含性兜底：即使上面的语义校验被绕过，canonical 路径也必须在临时目录内。
                if (!isContained(tmpDir, dst)) {
                    throw new ImportFailure(Status.ZIP_SLIP, "ZIP 条目逃出临时目录");
                }
                File parent = dst.getParentFile();
                if (parent != null && !parent.exists() && !parent.mkdirs()) {
                    throw new ImportFailure(Status.IO_ERROR, "无法创建解压目录");
                }
                long entryBytes = 0;
                try (FileOutputStream fos = new FileOutputStream(dst, false)) {
                    int n;
                    while ((n = zis.read(buf)) > 0) {
                        entryBytes += n;
                        total += n;
                        if (total > limits.maxTotalBytes) {
                            throw new ImportFailure(Status.TOO_LARGE,
                                    "解压总大小超过上限 " + limits.maxTotalBytes + " 字节");
                        }
                        fos.write(buf, 0, n);
                    }
                }
                if (MANIFEST_FILE_NAME.equals(dst.getName()) && entryBytes > limits.maxManifestBytes) {
                    throw new ImportFailure(Status.TOO_LARGE, "manifest.json 过大");
                }
            }
        } catch (IOException ex) {
            throw new ImportFailure(Status.IO_ERROR, "ZIP 解压失败");
        }
    }

    /** 在临时目录内唯一定位 manifest.json；0 个 → MISSING_MANIFEST，多个 → MANIFEST_INVALID。 */
    @NonNull
    private File locateSingleManifest(@NonNull File root) throws ImportFailure {
        List<File> found = new ArrayList<>();
        collectManifests(root, found);
        if (found.isEmpty()) {
            throw new ImportFailure(Status.MISSING_MANIFEST, "ZIP 内未找到 " + MANIFEST_FILE_NAME);
        }
        if (found.size() > 1) {
            throw new ImportFailure(Status.MANIFEST_INVALID, "ZIP 内存在多个 " + MANIFEST_FILE_NAME);
        }
        return found.get(0);
    }

    private static void collectManifests(@NonNull File dir, @NonNull List<File> out) {
        File[] children = dir.listFiles();
        if (children == null) {
            return;
        }
        for (File c : children) {
            if (c.isDirectory()) {
                collectManifests(c, out);
            } else if (MANIFEST_FILE_NAME.equals(c.getName())) {
                out.add(c);
            }
        }
    }

    // ---------------- 原子启用 ----------------

    /**
     * 目录原子替换：把已验证的 {@code base} 目录 rename 为 {@code <storeDir>/<profileId>}。
     *
     * <p>本方法<b>不</b>写激活指针（#2：只有实时管线激活成功才落定指针）；旧目录先 rename 为
     * {@code .bak_<profileId>} 并保留在句柄中，供指针落定成功时清理、失败时回滚。
     *
     * <p>#3 崩溃恢复：若检测到上次替换在「target→bak 之后、base→target 之前」崩溃
     * （target 缺失但 {@code .bak_<id>} 存在），先把备份恢复回 target；恢复失败即中止，
     * 绝不删除唯一完整副本。
     */
    @NonNull
    private ProfileSwap swapInProfileDirs(@NonNull File base, @NonNull String profileId,
                                          @Nullable String prevActive)
            throws ImportFailure {
        File target = profileDir(profileId);
        File bak = bakDirOf(profileId);
        // 先了结上一次未完成的回滚（第五轮审查 A3' γ5）：若残留 .rollback_<id> 意图标记，下面的
        // deleteRecursivelyQuietly(bak) 会先删掉**旧模型备份**，而 target 仍是那个激活失败的新包
        // ——旧模型就此永久丢失。此调用同时覆盖崩溃窗口（target 缺失 + bak 存在）现场。
        recoverProfileDirIfNeeded(profileId);
        if (!target.exists() && bak.exists()) {
            // 崩溃恢复：旧目录是唯一完整副本，恢复失败不能继续（否则下一步会删掉它）。
            if (!fileOps.rename(bak, target)) {
                throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                        "检测到上次替换残留（" + bak.getName() + "），恢复失败，已中止以保留旧配置");
            }
        }
        deleteRecursivelyQuietly(bak);
        if (bak.exists()) {
            // **写事务记录之前**必须保证 bak 已不存在（A1-P2②/A2-Med2）：否则下面的 target→bak 会因
            // 目标已存在而失败，而那条已写入的 in_progress 记录会让恢复把「游离的陈旧 bak」误判为
            // 「本次移开的旧目录」→ **静默回滚掉一次已提交的更新**。守住这条不变量后，
            // 「in_progress + bak 在」恒等于「本次替换已把旧目录移开」，状态推导才成立。
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                    "上次替换的备份无法清理（" + bak.getName() + "），已中止以保留现有配置");
        }
        boolean hadOld = target.exists();

        // 事务记录（Codex 要求 1）：**破坏性改名前**可靠落 in_progress；写不进去就绝不动手，
        // 旧配置保持原位。记录里的 hadOld + 原激活指针供恢复时「退旧」使用，txnId 则是提交证据
        // ——它在提交点被盖进激活指针，使恢复能区分「未提交」与「已提交但 committing 改写失败」。
        String txnId = newTxnId();
        if (!writeTxn(new DirTxn(profileId, hadOld, prevActive, txnId, false))) {
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                    "无法记录目录替换事务，已中止以保留旧配置（未做任何破坏性改动）");
        }

        if (hadOld && !fileOps.rename(target, bak)) {
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED, "旧配置目录无法移开");
        }
        boolean moved = fileOps.rename(base, target);
        if (!moved) {
            // 回滚失败必须显式上报，绝不静默：否则旧目录仅以 .bak_<id> 滞留、指针悬空。
            boolean restored = !hadOld || fileOps.rename(bak, target);
            if (!restored) {
                throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                        "临时目录无法原子重命名，且旧配置恢复失败（旧目录滞留于 " + bak.getName() + "）");
            }
            // 已就地还原到替换前状态 → 事务了结（否则残留记录会让 buildStatus 误报「事务未完成」）。
            finishTxn(profileId);
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED, "临时目录无法原子重命名");
        }
        // 替换已成功落定：目录语义完全由本 swap 承担，此前遗留的回滚意图标记随之失效，必须清除，
        // 否则 buildStatus 会因残留标记把本次新配置误判为「回滚未完成」而报 ERROR。
        clearRollbackMarker(profileId);
        return new ProfileSwap(target, bak, hadOld, profileId, txnId);
    }

    /** 目录替换句柄：记录旧目录备份，供激活指针落定成功时清理、失败时回滚。 */
    private static final class ProfileSwap {
        final File target;
        final File bak;
        final boolean hadOld;
        /** 本次替换的 profileId——回滚写入意图标记时需要（target/bak 名称已被改写，不能反推）。 */
        final String profileId;
        /**
         * 本次替换的**提交代次**：与 in_progress 记录里写的是同一个值，提交点由
         * {@code persistActive(profileId, txnId)} 盖进激活指针。回滚/清理路径不需要它，
         * 但 importZip 的两个后续步骤（落定指针、改写 committed 记录）都必须复用它——
         * 重新生成一个会让「指针代次 == 记录代次」这条提交证据失效。
         */
        final String txnId;

        ProfileSwap(File target, File bak, boolean hadOld, String profileId, String txnId) {
            this.target = target;
            this.bak = bak;
            this.hadOld = hadOld;
            this.profileId = profileId;
            this.txnId = txnId;
        }
    }

    /** 回滚结局：区分「已还原」「失败但已记录可恢复意图」「失败且无法自动恢复」。 */
    private enum RollbackOutcome { RESTORED, FAILED_RECORDED, FAILED_UNRECORDED }

    /**
     * 目录回滚：删除激活失败的新目录，并把旧目录（{@code .bak_<id>}）还原就位。
     *
     * <p><b>意图标记是「尽力而为」，不是「前置条件」</b>（第六轮双代理审计 A1-P2 / A2-High 纠正）：
     * 早先的实现「标记写不成功就不动手」，结果是**更差**——枚举「标记写失败」前提下的组合：
     * <ul>
     *   <li>删除成功 + 改名成功：照旧做能把旧模型**真正还原**（本实现）；停手则 target 仍是那个
     *       激活失败的新包，且无标记 → 状态报 ready(新包)、旧模型滞留 .bak（**更差**）。</li>
     *   <li>删除成功 + 改名失败：照旧做会留下「target 缺失」→ 崩溃窗口恢复能还原旧模型（**更差**）。</li>
     *   <li>删除失败：两者结局相同（target + bak 并存）。</li>
     * </ul>
     * 即「停手」把可自动还原的两种组合一并牺牲，却并未换来任何改善；而它想避免的
     * 「target 在 + bak 在 + 无标记」现场，恰恰是「什么都不做」的必然结果。
     * 故标记只用于**失败后能否由启动恢复兜底**，不得成为放弃还原的理由。
     *
     * <p><b>返回值区分三种结局</b>，调用方据此选文案（不得再从磁盘反推——标记路径可能被占用成
     * 目录等，{@code marker.exists()} 与「是否已记录意图」并不等价）。
     */
    @NonNull
    private RollbackOutcome rollbackProfileSwap(@NonNull ProfileSwap swap) {
        if (!swap.hadOld) {
            // 首次导入：新目录是**用户唯一的新包**，不得删除——与 importZip 的激活失败分支同口径
            //（那里也是保留目录、不切指针）。此前这里无条件删除，会在「native 激活已成功、仅指针
            //  写失败」时丢掉用户唯一的新包，且调用方文案谎称「已回滚配置」（根本没有可回滚的配置）。
            finishTxn(swap.profileId);   // 无旧可还原、指针未改：事务无事可做
            return RollbackOutcome.RESTORED;
        }
        // 尽力记录回滚意图：它能覆盖「删除过程中崩溃」等窄窗口，但记录失败不应阻止还原。
        final boolean recorded = writeRollbackMarker(swap);
        deleteRecursivelyQuietly(swap.target);
        if (!swap.target.exists() && fileOps.rename(swap.bak, swap.target)) {
            clearRollbackMarker(swap.profileId);  // 回滚完整完成，意图标记使命结束
            finishTxn(swap.profileId);             // 事务亦然：现场已回到替换前
            return RollbackOutcome.RESTORED;
        }
        return recorded ? RollbackOutcome.FAILED_RECORDED : RollbackOutcome.FAILED_UNRECORDED;
    }

    /** 回滚意图标记文件路径：{@code <storeDir>/.rollback_<profileId>}。 */
    @NonNull
    private File rollbackMarkerOf(@NonNull String profileId) {
        return new File(storeDir, ROLLBACK_MARKER_PREFIX + profileId);
    }

    /** 旧目录备份路径：{@code <storeDir>/.bak_<profileId>}（回滚是否真正完成以它是否被消费为准）。 */
    @NonNull
    private File bakDirOf(@NonNull String profileId) {
        return new File(storeDir, ".bak_" + profileId);
    }

    /**
     * 写入回滚意图标记（存在性即语义；内容仅供诊断）。
     *
     * <p><b>返回值必须被检查</b>：恢复逻辑只能靠该标记识别「target 在 + bak 在」这一回滚失败现场，
     * 因此标记写不出来时**绝不可继续做破坏性删除**（见 {@link #rollbackProfileSwap}）。
     *
     * @return 标记是否确实落盘
     */
    private boolean writeRollbackMarker(@NonNull ProfileSwap swap) {
        try (FileOutputStream fos = new FileOutputStream(rollbackMarkerOf(swap.profileId), false)) {
            fos.write(("target=" + swap.target.getName() + "\n").getBytes(StandardCharsets.UTF_8));
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Exception ignored) {
            }
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }

    /** 清除回滚意图标记（无可清除时静默）。 */
    private void clearRollbackMarker(@NonNull String profileId) {
        //noinspection ResultOfMethodCallIgnored
        rollbackMarkerOf(profileId).delete();
    }

    // ── 目录替换事务记录（任务 A，2026-09-16 决策）────────────────────────────────
    // 为什么需要它：`.rollback_<id>` 只有「存在即回滚」语义，**无法区分**「替换进行中」与
    // 「已成功提交但尚未清理备份」。故引入带 **state** 的记录：任何破坏性改名前先落
    // `in_progress`，指针落定后再改写为 `committed`。恢复据此判定「退旧」还是「留新」，
    // 不再依赖目录存在性猜测（那正是「把从未激活成功的新包报成 READY」的根因）。
    //
    // **2026-09-17 外部审查后的加强（约束 1）**：仅有 state 是不够的——**写 state 本身可能失败**，
    // 于是「未提交」与「已提交但 committed 改写失败」会留下同一份 in_progress 记录，恢复无法区分
    // （同名覆盖时连激活指针都前后相同）。故引入**事务代次 txnId**：随机 nonce，写在记录里、并由
    // persistActive 在**唯一提交点**盖进激活指针。判定式：`指针.txnId == txn.txnId ⇒ 已提交`。
    //
    // **2026-09-18 第三代理实证后的补强**：把代次只放在指针里仍不够——**指针会被下一次导入/切换
    // 覆盖**，证据随之被摧毁（见下方「提交见证」一节）。现在多一份**不随指针移动**的见证
    // `.commit_<profileId>`，且判定式放宽为「见证代次 == 事务代次 **或** 指针代次 == 事务代次」。

    /** 事务记录文件名前缀：{@code <storeDir>/.txn_<profileId>}（点号前缀 → 被 listProfiles 忽略）。 */
    private static final String TXN_PREFIX = ".txn_";
    private static final String TXN_STATE_IN_PROGRESS = "in_progress";
    private static final String TXN_STATE_COMMITTED = "committed";

    /**
     * 生成**事务代次**：本次目录替换唯一，写在事务记录里、并在提交点由 {@link #persistActive}
     * 盖进激活指针。
     *
     * <p>为什么必须是「每次都不同的 nonce」而不是「当前激活的 profileId」：同名覆盖时
     * profileId 在事务前后**完全相同**，用它作提交证据无法区分「尚未提交」与「已提交但记录
     * 改写失败」——会把一个失败的新包误判为提交成功。（UUID 自 API 1 起可用；去横线后是
     * 32 位 hex，便于用「长度 + 字符集」做完整性校验。）
     */
    @NonNull
    private static String newTxnId() {
        return java.util.UUID.randomUUID().toString().replace("-", "");
    }

    /**
     * 代次格式校验：不合法的代次一律按「**无证据**」处理，绝不把损坏内容当成可比较的 id
     * ——否则脏内容可能碰巧相等而伪造出一次「已提交」判定。
     */
    private static boolean isSafeTxnId(@Nullable String s) {
        if (s == null || s.length() != 32) return false;
        for (int i = 0; i < 32; i++) {
            char c = s.charAt(i);
            boolean hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            if (!hex) return false;
        }
        return true;
    }

    /** 目录替换事务记录（Codex 要求 2：目标 profile / 是否有旧目录 / 原激活指针 / 提交状态）。 */
    private static final class DirTxn {
        @NonNull final String profileId;
        final boolean hadOld;
        @Nullable final String prevActive;
        /**
         * 本次替换的提交代次；{@code null} = **旧格式记录**（本版本之前写入的），按旧语义恢复。
         *
         * <p>代次行**必须同样出现在 in_progress 记录里**：`writeTxn(committed)` 改写失败而备份
         * 又删不掉时，磁盘上留下的恰好是 in_progress 形态，代次是判定它「其实已提交」的唯一依据。
         */
        @Nullable final String txnId;
        final boolean committed;

        DirTxn(@NonNull String profileId, boolean hadOld, @Nullable String prevActive,
               @Nullable String txnId, boolean committed) {
            this.profileId = profileId;
            this.hadOld = hadOld;
            this.prevActive = prevActive;
            this.txnId = txnId;
            this.committed = committed;
        }

        @NonNull
        byte[] encode() {
            StringBuilder sb = new StringBuilder();
            sb.append("profileId=").append(profileId).append('\n');
            sb.append("hadOld=").append(hadOld).append('\n');
            sb.append("prevActive=").append(prevActive == null ? "" : prevActive).append('\n');
            // txnId == null（旧格式构造）时**不写该行**，产出与旧版本逐字节相同的记录，
            // 供测试构造「旧版本遗留现场」并验证旧语义兼容。
            if (txnId != null) {
                sb.append("txnId=").append(txnId).append('\n');
            }
            sb.append("state=").append(committed ? TXN_STATE_COMMITTED : TXN_STATE_IN_PROGRESS)
                    .append('\n');
            return sb.toString().getBytes(StandardCharsets.UTF_8);
        }
    }

    /** 事务记录路径。 */
    @NonNull
    private File txnFileOf(@NonNull String profileId) {
        return new File(storeDir, TXN_PREFIX + profileId);
    }

    /** 暂存根目录 {@code <storeDir>/.staging}（不保证存在）。 */
    @NonNull
    private File stagingRoot() {
        return new File(storeDir, STAGING_DIR);
    }

    /** 事务记录落盘前的临时文件：{@code <storeDir>/.staging/txn/<profileId>}。 */
    @NonNull
    private File stagingTxnFileOf(@NonNull String profileId) {
        return new File(new File(stagingRoot(), STAGING_TXN_DIR), profileId);
    }

    /** 提交见证落盘前的临时文件：{@code <storeDir>/.staging/commit/<profileId>}。 */
    @NonNull
    private File stagingCommitFileOf(@NonNull String profileId) {
        return new File(new File(stagingRoot(), STAGING_COMMIT_DIR), profileId);
    }

    /**
     * 确保 {@code f} 的父目录存在（写暂存文件前必调）。
     *
     * <p><b>必须在 {@code dirLock} 内调用</b>：{@code writeTxn} / {@code writeCommitWitness} 的调用点
     * 都在 {@code importZip} 的持锁段内（{@code swapInProfileDirs} 与 {@code importZip} 主体），
     * 故此处裸 {@code mkdirs} 不会与并发导入竞争。
     *
     * @return 父目录是否可用；失败时调用方按「写失败」处理（绝不继续）
     */
    private static boolean ensureParentDir(@NonNull File f) {
        File parent = f.getParentFile();
        return parent == null || parent.isDirectory() || parent.mkdirs();
    }

    /**
     * 读事务记录。
     *
     * <p>缺 {@code txnId} 行（或代次不合法）→ {@link DirTxn#txnId} 为 null，即**旧格式记录**：
     * 由本版本之前的代码写入，按 {@code recoverTxnLegacy} 的旧语义恢复。本版本写入的记录
     * 一定带代次，故「新记录 + 指针无代次」只可能是「提交点从未成功落定」。
     *
     * @return 记录；无文件或内容不可解析（含 profileId 不匹配）时返回 null——
     *         调用方必须把 null 当作「**无记录**」而非「无事务」，后者需按歧义现场处理。
     */
    @Nullable
    private DirTxn readTxn(@NonNull String profileId) {
        File f = txnFileOf(profileId);
        if (!f.isFile()) return null;
        String text;
        try {
            // 必须**完整**读回：profileId 没有长度上限（isSafeDirName 只拒绝空/./../分隔符），
            // 而记录里 profileId 与 prevActive 各出现一次、且 txnId 行排在 state 行之前。
            // 截断会先吃掉 txnId 行 → 记录被降级为「旧格式」→ 提交证据丢失 → 一次已提交的替换
            // 被误判为未提交而回滚。失败的代价是单向的（丢证据 → 回滚），故读全比省一次分配重要。
            text = readTextFile(f, 4096);
        } catch (Exception e) {
            return null;
        }
        String id = null;
        String prev = "";
        String txnId = null;
        boolean hadOld = false;
        boolean committed = false;
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if ("profileId".equals(k)) id = v;
            else if ("hadOld".equals(k)) hadOld = "true".equals(v);
            else if ("prevActive".equals(k)) prev = v;
            // 非法/损坏的代次降级为 null（= 无提交证据），**不**当成可比较的 id：
            // 脏内容一旦参与相等比较，可能伪造出一次「已提交」判定。
            else if ("txnId".equals(k)) txnId = isSafeTxnId(v) ? v : null;
            else if ("state".equals(k)) committed = TXN_STATE_COMMITTED.equals(v);
        }
        if (id == null || !id.equals(profileId)) return null;
        return new DirTxn(id, hadOld, prev.isEmpty() ? null : prev, txnId, committed);
    }

    /**
     * 原子落事务记录（临时文件 + fsync + 改名）。
     *
     * <p><b>返回值必须被检查</b>：破坏性改名前写不进去就**绝不能动手**（Codex 要求 1），
     * 否则会留下「改了一半、且无任何记录可依」的现场。
     */
    private boolean writeTxn(@NonNull DirTxn txn) {
        // 临时文件放进**名字恒定**的暂存子目录 `.staging/txn/<profileId>`（文件名即 profileId）：
        // 旧的顶层 `.txn_<id>.tmp` 与「profileId 恰为 <id>.tmp 的**正式记录**」逐字同名，此处的
        // 截断写会把它**覆盖销毁**——只修清扫救不了这条独立破坏路径。改名目标 `.txn_<id>` 仍在
        // storeDir 之下，故 moveReplace 依旧是同盘 rename（原子、覆盖语义见 FileOps.moveReplace）。
        File tmp = stagingTxnFileOf(txn.profileId);
        if (!ensureParentDir(tmp)) {
            return false;
        }
        if (!fileOps.writeSync(tmp, txn.encode())) {
            // 保留原有自删策略（仅 moveReplace 失败时删）：writeSync 失败可能留下半截文件，但它
            // 位于 `.staging/` 下——下次导入整体清空该目录即回收，不构成同名歧义，也不漏检。
            return false;
        }
        if (!fileOps.moveReplace(tmp, txnFileOf(txn.profileId))) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return false;
        }
        return true;
    }

    /**
     * 清除事务记录（无可清除时静默）。
     *
     * <p>⚠️ <b>不要直接调用本方法</b>：事务了结请走 {@link #finishTxn(String)}。只清记录而把
     * 见证留在盘上，会留下「证据在、记录不在」的中间态——虽不会碰巧匹配（代次是每次不同的
     * nonce），但它是无谓的垃圾，也是后人误读的来源。
     */
    private void clearTxn(@NonNull String profileId) {
        //noinspection ResultOfMethodCallIgnored
        txnFileOf(profileId).delete();
    }

    // ── 提交见证（2026-09-18 修复：提交证据随指针移动而丢失）─────────────────────────
    // 缺口：`txnId` 原先只存在**激活指针**里，而指针会被下一次导入/切换覆盖。于是「本次替换
    // 已提交」这条事实会随指针移动而**彻底消失**；此后触碰该 profile 只能判「未提交」→ 把一次
    // 成功的替换**静默回滚**，而 native 仍在跑新模型（磁盘/指针/native 三方不一致）。
    //
    // 为什么不按「指针不在本 profile ⇒ 保留现场」简单修：cross-profile 的**崩溃**回滚（合法，
    // 已有用例 inProgressTxnRollsBackToOldModelAndPointer 覆盖）观察到的指针形态与它**完全相同**
    // ——两种历史用「仅指针」这一种见证不可区分，所以才需要一份**不随指针移动**的见证。
    //
    // 为什么见证必须先于提交点写入：证据的持久性若依赖后续某次写入成功，那次写入失败就会把
    // 我们打回原形。见证写在提交动作最前沿，指针怎么移动、后续改写是否失败，都与它无关。

    /** 提交见证路径：{@code <storeDir>/.commit_<profileId>}。 */
    @NonNull
    private File commitWitnessOf(@NonNull String profileId) {
        return new File(storeDir, COMMIT_WITNESS_PREFIX + profileId);
    }

    /**
     * 原子落提交见证（临时文件 + writeSync/fsync + 改名），与 {@link #writeTxn} 同构。
     *
     * <p><b>返回值必须被检查</b>：见证落不下就**绝不越过提交点**——与「{@code in_progress} 记录
     * 写不进去就绝不动手」同一条原则。落不下却继续提交，等于把本次事务交回给一份注定会被指针
     * 移动摧毁的证据，正是本方法要消灭的那个缺口。
     */
    private boolean writeCommitWitness(@NonNull String profileId, @NonNull String txnId) {
        // 与 writeTxn 同构：临时文件放进 `.staging/commit/<profileId>`（文件名即 profileId），
        // 不再用顶层 `.commit_<id>.tmp`——那个名字与「profileId 恰为 <id>.tmp 的**活见证**」同名。
        // 迁入暂存目录后，即使留下半截文件也可被 sweepStaleTempDirs 安全清扫（见其 javadoc）。
        File tmp = stagingCommitFileOf(profileId);
        StringBuilder sb = new StringBuilder();
        sb.append("profileId=").append(profileId).append('\n');
        sb.append("txnId=").append(txnId).append('\n');
        if (!ensureParentDir(tmp)) {
            return false;
        }
        if (!fileOps.writeSync(tmp, sb.toString().getBytes(StandardCharsets.UTF_8))) {
            // 必须删临时文件：DEFAULT_FILE_OPS.writeSync 先建/截断文件再写，写到一半失败（如磁盘满）
            // 会留下半截文件。（迁入 `.staging/` 后它已可被清扫，但此处仍显式删除，正常路径零残留。）
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return false;
        }
        if (!fileOps.moveReplace(tmp, commitWitnessOf(profileId))) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return false;
        }
        return true;
    }

    /**
     * 读提交见证里的代次；见证缺失 / 不可读 / profileId 不符 / 代次不合法 → {@code null}
     * （= **无证据**，而不是「未提交」以外的任何含义）。
     *
     * <p>损坏内容一律降级为「无证据」，绝不参与相等比较——否则脏内容碰巧相等就会伪造出一次
     * 「已提交」，把**未提交**的新目录留在激活位（理由与 {@link #isSafeTxnId} 同）。
     */
    @Nullable
    private String readCommitWitness(@NonNull String profileId) {
        File f = commitWitnessOf(profileId);
        if (!f.isFile()) return null;
        String text;
        try {
            // 必须**完整**读回，不能只读一个定长缓冲：profileId 没有长度上限（isSafeDirName 只拒绝
            // 空/./../分隔符），截断会让 txnId 行丢失 → 见证被判「无证据」→ 在「指针已移走」的现场
            // 把一次**成功**的导入静默回滚，正是本机制要消灭的失败模式。
            // 上界取 4096：见证文件名是 `.commit_<id>`，受文件系统 255 字节的**文件名**上界约束，
            // 故 profileId ≤ 247 字节、内容 ≤ 约 300 字节——4096 在实践中不可达，只是防御。
            text = readTextFile(f, 4096);
        } catch (Exception e) {
            return null;
        }
        String id = null;
        String txnId = null;
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if ("profileId".equals(k)) id = v;
            else if ("txnId".equals(k)) txnId = isSafeTxnId(v) ? v : null;
        }
        return (id != null && id.equals(profileId)) ? txnId : null;
    }

    /**
     * 撤销提交见证。
     *
     * <p>用于「见证已落盘、提交点最终却没能落定」的收尾（{@code persistActive} 失败）：此时若不
     * 撤销，盘上会留下一条「本次事务已提交」的证据，而下一次恢复会据此把**已经回滚**的现场再翻成
     * 「保留新目录」——证据与现场自相矛盾。
     *
     * @return 见证是否**确实已不在盘上**：调用方据此决定能否回滚（见 {@code importZip}）
     */
    private boolean clearCommitWitness(@NonNull String profileId) {
        File f = commitWitnessOf(profileId);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
        return !f.exists();
    }

    /**
     * 了结一条事务：**记录与见证一并清除**。
     *
     * <p>所有「事务已无事可做」的路径都必须走这里（而不是 {@link #clearTxn}），使「见证在 ⟺ 记录在」
     * 成为一条不变量。反过来，**记录被保留时见证也必须保留**——那正是下次恢复判「已提交」的依据。
     */
    private void finishTxn(@NonNull String profileId) {
        clearTxn(profileId);
        clearCommitWitness(profileId);
    }

    /**
     * 原子写激活指针：tmp + bak 替换（与注册表落盘同策略）。
     *
     * <p><b>本方法是唯一允许写「带提交代次的」激活指针的方法</b>（两个调用点：{@code importZip}
     * 的提交点、{@code activate} 的显式切换）。任何**新增**的写入点都必须传入本次事务的
     * {@code txnId}——指针里的代次是与事务记录比对、判定「提交点是否已落定」的证据之一；
     * 漏盖代次会让一次已提交的事务在恢复时被误判为未提交而被回滚。
     *
     * <p>**刻意不盖代次**的例外只有一处：{@link #restoreActivePointer}——它不属于任何事务（旧格式
     * 恢复路径），伪造一个代次会让「指针代次 == 事务代次」这条判据失真。
     *
     * <p>另有 {@link #readActivePointer} 在崩溃恢复时把 {@code active_profile.bak} rename 回主指针：
     * 那只是把**先前由本方法或 {@code restoreActivePointer} 写下的内容**就位，不制造新代次，
     * 故不在上述限制之内。除此之外不得再有绕过本方法写 {@code active_profile} 的地方。
     *
     * <p>⚠️ 但**不能只靠它**：指针会被下一次导入/切换覆盖，证据随之丢失。{@code importZip} 必须在
     * 调用本方法**之前**先落 {@code .commit_<id>} 见证（见 {@link #writeCommitWitness}）——那份
     * 证据不随指针移动，是判定「已提交」的**主**证据，本方法盖进指针的代次只是第二份。
     *
     * @param txnId 本次事务的提交代次；{@code null} 表示这是一次**没有所属事务**的写入
     *              （当前无此调用点，保留参数形态以备将来），写出的指针将被视为「无提交证据」。
     */
    private boolean persistActive(@NonNull String profileId, @Nullable String txnId) {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File tmp = new File(storeDir, ACTIVE_POINTER_FILE + ".tmp");
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
        // 阶段 1：#3 崩溃恢复——上次写入在「dst→bak」之后、「tmp→dst」完成前退出时主指针缺失、
        // 备份尚在；先把备份恢复为可回滚基线，否则下面会先删备份再写，指针在崩溃窗口中丢失。
        // 恢复失败则宁可不写（绝不删除唯一副本）。
        if (!dst.exists() && bak.exists() && !fileOps.rename(bak, dst)) {
            return false;
        }
        // 内容写入走 fileOps.writeSync（写 + flush + fsync，与事务记录同一条接缝）：
        // ① 让**提交点写入失败**成为可注入、可测的失败点（否则该分支在纯 JVM 下无法构造，
        //    而「提交写失败」恰是本次修复要覆盖的组合之一）；
        // ② 消除「事务记录走接缝、指针自己走裸流」的策略分裂。
        // 语义与原先的裸 FileOutputStream + getFD().sync() 等价（DEFAULT_FILE_OPS.writeSync 同语义）。
        if (!fileOps.writeSync(tmp, encodeActivePointer(profileId, txnId))) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return false;
        }
        if (bak.exists()) {
            bak.delete();
        }
        if (dst.exists() && !fileOps.rename(dst, bak)) {
            tmp.delete();
            return false;
        }
        if (!fileOps.rename(tmp, dst)) {
            if (bak.exists()) {
                fileOps.rename(bak, dst);
            }
            tmp.delete();
            return false;
        }
        if (bak.exists()) {
            bak.delete();
        }
        return true;
    }

    /**
     * 激活指针的内容（profileId + 可选的提交代次）。
     *
     * <p>{@link #txnId} 为 null 有两种来源：解析出的是**旧格式**指针（裸 profileId，本版本之前
     * 写入），或代次不合法而被降级为「无证据」。**后者不得参与相等比较**——脏内容一旦相等，
     * 会伪造出一次「已提交」判定。
     */
    private static final class ActivePointer {
        @NonNull final String profileId;
        @Nullable final String txnId;

        ActivePointer(@NonNull String profileId, @Nullable String txnId) {
            this.profileId = profileId;
            this.txnId = txnId;
        }
    }

    /** 指针文件内容：两行同处一个文件 → 一次 rename 原子落定，profileId 与代次不可能分离。 */
    @NonNull
    private static byte[] encodeActivePointer(@NonNull String profileId, @Nullable String txnId) {
        StringBuilder sb = new StringBuilder();
        sb.append("profileId=").append(profileId).append('\n');
        if (txnId != null) {
            sb.append("txnId=").append(txnId).append('\n');
        }
        return sb.toString().getBytes(StandardCharsets.UTF_8);
    }

    /**
     * 读取当前激活指针；#3 崩溃恢复：主指针缺失/不可读时回退 {@code active_profile.bak}
     * 并把备份 rename 回主指针（复用 {@link AndroidEnrollmentRepository} 的 .bak 恢复模式）。
     * 主指针与备份都不可用时返回 null（未导入）。
     */
    @Nullable
    private ActivePointer readActivePointer() {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
        if (!dst.isFile()) {
            if (bak.isFile()) {
                // 崩溃恢复：主指针缺失但备份是唯一完整副本，先解析备份再 rename 回主。
                ActivePointer p = parseActivePointer(bak);
                if (p != null) {
                    fileOps.rename(bak, dst);
                    return p;
                }
            }
            return null;
        }
        ActivePointer p = parseActivePointer(dst);
        if (p == null && bak.isFile()) {
            // 主指针存在但不可读/非法：从备份恢复，避免退化为「未导入」丢失激活。
            ActivePointer fromBak = parseActivePointer(bak);
            if (fromBak != null) {
                fileOps.rename(bak, dst);
                return fromBak;
            }
        }
        return p;
    }

    /**
     * 当前激活的 profileId；语义与调用方契约与升级前**完全一致**
     * （{@code activeProfileId()} 等全部调用点不需要改动）。需要提交代次时用
     * {@link #readActivePointer()}。
     */
    @Nullable
    private String readActive() {
        ActivePointer p = readActivePointer();
        return p == null ? null : p.profileId;
    }

    /**
     * 解析指针文件为 {@link ActivePointer}；缺失/不可读/非法（profileId 不是安全目录名）返回 null。
     *
     * <p>两种格式都接受：
     * <ul>
     *   <li>本版本：含 {@code profileId=<id>} 行（+ 可选 {@code txnId=<32hex>} 行）——按 key=value 解析；</li>
     *   <li>旧版本：整串即裸 {@code <id>}——保持可读，{@code txnId} 解析为 null（无提交证据）。</li>
     * </ul>
     */
    @Nullable
    private static ActivePointer parseActivePointer(@NonNull File f) {
        String text;
        try {
            text = readTextFile(f, 4096);
        } catch (Exception ex) {
            return null;
        }
        String trimmed = text.trim();
        if (!trimmed.contains("profileId=")) {
            // 旧格式：整串即 profileId。**不**因为有换行就当损坏——旧实现的 trim() 语义照旧。
            //
            // ⚠️ **刻意不「收紧」成「含换行即视为损坏」**（A2 审计 Low-4 提过这个方向）：
            // 把整串交给 isSafeDirName 会让这类**损坏显式暴露**——后续按该 profileId 找目录必然
            // 找不到，buildStatus 遂报「激活配置目录缺失」ERROR（**不触发任何删除**）。若在这里
            // 拒绝，损坏文件会退化成「未导入」这一**静默**状态，反而更难发现。收紧是**更糟**的行为。
            return FaceModelManifest.isSafeDirName(trimmed)
                    ? new ActivePointer(trimmed, null)
                    : null;
        }
        String id = null;
        String txnId = null;
        for (String line : trimmed.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if ("profileId".equals(k)) id = v;
            else if ("txnId".equals(k)) txnId = isSafeTxnId(v) ? v : null;
        }
        if (id == null || !FaceModelManifest.isSafeDirName(id)) return null;
        return new ActivePointer(id, txnId);
    }

    // ── 文件操作接缝（任务 A，2026-09-16 决策；子代理 4 P4 改为实例级）────────────────
    // 用途：目录替换的事务协议有大量**破坏性**步骤（改名 / 删除 / 落事务记录），其失败分支在纯 JVM
    // 下难以构造（本类是 final、原方法均为 private static，「非空目录占位」只覆盖得了少数情形）。
    // 故把这三类操作收敛为可注入接口。注入点已是**实例级**（见下方 fileOps 字段），
    // 与 FileStoreUtils 的实例级改造同构；默认实现即原有行为，**生产路径零变化**。
    //
    // ⚠️ 本接口**不是** {@code @VisibleForTesting}（A4 与 FileStoreUtils.FileOps 对齐注解哲学）：
    // 注入点改为实例级后，本类以 private 字段持有本类型（默认 {@link #DEFAULT_FILE_OPS}），故本类型
    // 属**生产内部类型**，并非测试专用；仅**注入方法** {@link #setFileOpsForTesting(FileOps)}
    // （以及 FileStoreUtils 侧的对应方法）才是测试专用。此前把注解放到接口上会误导读者。

    interface FileOps {
        /** 改名；语义同 {@link File#renameTo}（含吞 SecurityException）。**目标已存在时失败**。 */
        boolean rename(@NonNull File from, @NonNull File to);

        /**
         * 替换式移动：**目标已存在时覆盖**。
         *
         * <p>为什么必须有它：{@code File.renameTo} 在 Windows 上遇已存在目标**必然失败**（POSIX 则覆盖）
         * ——事务记录从 {@code in_progress} 改写为 {@code committed} 正是「覆盖既有文件」，
         * 若走 renameTo 则在 Windows 上**恒失败**，记录会停在 in_progress，进而在备份清理也失败时
         * 导致恢复把一次**成功**的导入误判为未提交而回滚。
         */
        boolean moveReplace(@NonNull File from, @NonNull File to);

        /** 递归删除；返回是否**确实已不存在**。 */
        boolean deleteRecursively(@NonNull File f);

        /** 写入 + flush + fsync（事务记录等需落盘保证的小文件）；返回是否成功。 */
        boolean writeSync(@NonNull File f, @NonNull byte[] data);
    }

    private static final FileOps DEFAULT_FILE_OPS = new FileOps() {
        @Override
        public boolean rename(@NonNull File from, @NonNull File to) {
            return FileStoreUtils.renameQuietly(from, to);
        }

        @Override
        public boolean moveReplace(@NonNull File from, @NonNull File to) {
            // **不得**用 java.nio.file：Files / File#toPath / StandardCopyOption 均需 **API 26**，
            // 而本项目 minSdk=21（lint 的 NewApi 已据此报错）。故用「先删目标再改名」实现覆盖语义。
            // 已知窗口：两步之间崩溃会短暂「无记录」——此时 buildStatus 会把现场判为**歧义并如实报错**
            //（不销毁数据），远优于「提交改写恒失败 → 恢复误回滚一次成功的导入」。
            //noinspection ResultOfMethodCallIgnored
            to.delete();
            return from.renameTo(to);
        }

        @Override
        public boolean deleteRecursively(@NonNull File f) {
            return FaceModelManager.deleteRecursively(f);
        }

        @Override
        public boolean writeSync(@NonNull File f, @NonNull byte[] data) {
            try (FileOutputStream fos = new FileOutputStream(f, false)) {
                fos.write(data);
                fos.flush();
                try {
                    fos.getFD().sync();
                } catch (Exception ignored) {
                }
                return true;
            } catch (Exception e) {
                return false;
            }
        }
    };

    /**
     * 破坏性文件操作（删除 / 改名 / 写盘）的**实例级**注入点（子代理 4 P4，与 {@code FileStoreUtils}
     * 同构的扩面改造）。
     *
     * <p>此前本字段与注入方法都是**进程级静态**（{@code private static volatile FileOps} +
     * 静态 {@code setFileOpsForTesting}）。而 {@code :app:testDebugUnitTest} 的全部测试类在
     * **同一 JVM 内串行**执行，任一类注入后忘记复位就会污染其后所有用例。现改为**实例字段**：
     * 每处破坏性调用读的都是**本实例**的 {@code fileOps}，经实例方法
     * {@link #setFileOpsForTesting(FileOps)} 注入 ⇒ 跨实例泄漏在结构上不可能发生。
     *
     * <p><b>默认即原有行为，生产路径零变化</b>：实例构造时默认 {@link #DEFAULT_FILE_OPS}。
     *
     * <p><b>为何非 {@code final}（安全发布前提，如实固化）</b>：字段须可经
     * {@link #setFileOpsForTesting(FileOps)} 在**构造后**改写（测试注入），故不能标 {@code final}；
     * 即它**非 volatile、非 final**。理论上若本实例被**不安全发布**，并发读者可能见到 {@code null}。
     * 当前仓库**安全**：本类实例在生产中仅经 {@code MainActivity.modelManager()} 的 {@code volatile}
     * 字段 + 双检锁发布（见 {@code MainActivity.faceModelManager}），volatile 写建立了 happens-before
     * ⇒ 构造期对该字段的赋值先于其它线程的读取而可见。**改动发布路径（去掉 volatile / 改为常规共享）
     * 前须复核本前提**。
     */
    private FileOps fileOps = DEFAULT_FILE_OPS;

    /** 仅供测试注入；传 null 恢复默认（真实文件系统）实现。 */
    @androidx.annotation.VisibleForTesting
    void setFileOpsForTesting(@Nullable FileOps ops) {
        fileOps = (ops != null) ? ops : DEFAULT_FILE_OPS;
    }

    /**
     * 目录恢复（启动/状态查询前调用），覆盖两种现场：
     *
     * <ol>
     *   <li><b>回滚未完成</b>（存在 {@code .rollback_<id>} 标记，第五轮审查 P1）：{@code <profileId>}
     *       目录是**激活失败的新包**，原模型在 {@code .bak_<id>}。先删掉新目录，备份才有位置还原；
     *       target 删不掉则**保持现场不动**（既不丢旧模型，也不谎报就绪）。</li>
     *   <li><b>崩溃窗口</b>（#3）：激活目录缺失但存在 {@code .bak_<id>}——上次替换在 target→bak 之后、
     *       base→target 之前崩溃，旧目录是唯一完整副本，直接还原。</li>
     * </ol>
     *
     * <p>恢复为 best-effort：失败不在此处报错，仍由 {@link #buildStatus} 依据真实目录如实判定。
     *
     * <p><b>rename 返回值的契约分界（勿擅自「统一」）</b>：本类中 {@code fileOps.rename(...)}
     * 的返回值<b>只在「决策点」检查</b>——即其成败会改变对外结果或用户文案时，代表是
     * {@code rollbackProfileSwap} 里决定 {@code RollbackOutcome} 的那一处（有专门用例钉住）。
     * <b>恢复/回填路径刻意不检查</b>，共四处：{@code persistActive} 内（tmp→dst 改名失败后的
     * 尽力回填）、{@code readActivePointer} 两处（活动指针崩溃恢复）、{@code recoverProfileDirIfNeeded}
     * 的崩溃窗口还原。这四处的 rename 结果都被丢弃，但后果需<b>分级</b>，不可一概而论：
     * <ul>
     *   <li>{@code persistActive} 那处失败时该方法本就返回 false，调用方据此报
     *       {@code ATOMIC_RENAME_FAILED} ⇒ <b>已用户可见</b>，无需再查。</li>
     *   <li>{@code recoverProfileDirIfNeeded} 的崩溃窗口还原失败时目标目录仍缺失，下一次
     *       {@link #buildStatus} 会据「目录缺失」报 ERROR ⇒ <b>已由下一跳如实呈现</b>。</li>
     *   <li>{@code readActivePointer} 两处则<b>可能没有任何用户可见信号</b>。两处改名失败的现场
     *       <b>并不相同</b>，不能简化成单一情形：<b>第一处</b>（主指针 {@code active_profile}
     *       <b>缺失</b>的那一支）改名失败后主指针<b>仍缺失</b>；<b>第二处</b>（主指针<b>存在但
     *       不可解析/非法</b>的那一支）改名失败后主指针<b>仍在，且保留其损坏内容</b>——并非
     *       「缺失」。两处的共同点是：{@code active_profile.bak} 都尚未被消耗、<b>仍可解析</b>，
     *       且两处的 rename 返回值都被丢弃、随后都<b>无条件</b>返回从 {@code .bak} 解析出的
     *       <b>有效</b> id ⇒ 读路径（{@link #readActive()} / {@link #readActivePointer()}）会
     *       回落到 {@code .bak} 并返回<b>有效</b> id ⇒ {@link #buildStatus} 走正常（READY）
     *       分支、不报错。即在这两处，「静默」<b>可能</b>延伸到用户层——不要假定它一定被下一跳
     *       呈现。</li>
     * </ul>
     * <b>若要在此补检查，必须同时给出用户可见的处置路径</b>，否则只是多一个被丢弃的布尔值。
     */
    private void recoverProfileDirIfNeeded(@NonNull String profileId) {
        File target = profileDir(profileId);
        File bak = bakDirOf(profileId);

        // ① 事务记录**优先**：它带 state，能区分「替换进行中」与「已成功提交但尚未清理备份」，
        //    而 .rollback_<id> 只有「存在即回滚」语义（这正是方案①必须升级为事务记录的原因）。
        DirTxn txn = readTxn(profileId);
        if (txn != null) {
            recoverTxn(txn, target, bak);
            return;
        }

        // ② 旧版回滚意图标记（无事务记录的年代）：存在即回滚。
        File marker = rollbackMarkerOf(profileId);
        if (marker.exists()) {
            if (!bak.exists()) {
                // 备份已不存在（无旧模型可还原）：标记失去意义，清除以免状态永久卡在 ERROR。
                //noinspection ResultOfMethodCallIgnored
                marker.delete();
            } else if (!target.exists() || fileOps.deleteRecursively(target)) {
                // 新目录已清空，备份可还原；成功则连标记一并清除（回滚到此完成）。
                if (fileOps.rename(bak, target)) {
                    //noinspection ResultOfMethodCallIgnored
                    marker.delete();
                }
            }
            // 至此 target 仍存在或备份仍未还原：保持现场，交由 buildStatus 如实报错，不做无根据的承诺。
            return;
        }

        // ③ 崩溃窗口（无任何记录）：旧目录是唯一完整副本，直接还原。
        if (!target.exists() && bak.exists()) {
            fileOps.rename(bak, target);
        }
        // ④ 「无记录且 target 与 bak 并存」= **歧义现场**（旧版本遗留）：保留两份数据、
        //    **不猜哪份应生效**（Codex 要求 6）。此处刻意不做任何改动，由 buildStatus 报错提示人工处理。
    }

    /**
     * 按事务记录恢复（Codex 要求 4）。
     *
     * <p><b>提交判定（约束 1）</b>：提交证据有两份——提交点前落的 {@code .commit_<id>} 见证，以及
     * {@link #persistActive} 盖进激活指针的代次。故：
     * <ul>
     *   <li>记录自述 {@code committed}，**或**记录带代次且**见证或指针**的代次与之相等 ⇒ **已提交**
     *       ——保留新目录，只重试清理备份；</li>
     *   <li>其余 ⇒ **未提交** ——回滚目录，且**绝不触碰激活指针**。</li>
     * </ul>
     *
     * <p><b>为什么不能只看指针</b>：指针会被下一次导入/切换覆盖；一旦覆盖，「本次已提交」就再无
     * 盘上证据，而 cross-profile 的**崩溃**回滚（合法）观察到的指针形态与之完全相同——两种历史
     * 用「仅指针」这一种见证不可区分，恢复只能猜，猜错就是静默回滚一次成功的替换。
     *
     * <p><b>为什么回滚路径不再还原指针</b>：{@code persistActive} 是唯一写指针的方法，所以
     * 「未提交」等价于「本事务从未成功改写过指针」——指针根本无需还原。这条不变量一次消掉两处
     * 历史歧义：同名覆盖时指针在事务前后都指向同一个 profileId（无法据它区分提交前后），而
     * 代次每次都不同，据代次可以判定。
     *
     * <p><b>为什么不按 bak 是否存在推断历史</b>：`bak` 被上一次恢复消费掉（rename 回 target）
     * 与「从未创建过 bak」在磁盘上**完全同形**。旧实现据「bak 不在」断言「替换尚未开始」并清
     * 记录，会把「指针还原意图」永久丢弃（P2）。新实现不再用 bak 的存在性做历史推断。
     *
     * <p>任一步失败都**保持现场并保留记录**，由 {@code buildStatus} 报 ERROR 且拒绝继续覆盖导入。
     */
    private void recoverTxn(@NonNull DirTxn txn, @NonNull File target, @NonNull File bak) {
        if (txn.txnId == null) {
            // 旧格式记录（本版本之前写入）：没有代次可比对，走旧语义。
            recoverTxnLegacy(txn, target, bak);
            return;
        }
        if (isCommitted(txn, readActivePointer(), readCommitWitness(txn.profileId))) {
            commitSide(txn, target, bak);
            return;
        }
        rollbackDirOnly(txn, target, bak);
    }

    /**
     * 是否为「**证据不足的旧格式歧义现场**」：旧记录（无代次、未自述提交）+ 同名覆盖
     * （{@code prevActive} 为空或等于本 profileId）+ 备份仍在 + 指针已指向本 profileId。
     *
     * <p>此时「替换未提交」与「已提交但记录改写失败且备份也没删掉」两种历史在磁盘上同形，
     * 而旧记录没有代次可比对 → 只能保留全部副本与记录、报错，交由
     * {@link #resolveAmbiguousSwap} 显式了结（约束 1）。
     *
     * <p>抽成单一判据供 {@link #recoverTxnLegacy} / {@link #unresolvedSwapReason} /
     * {@link #buildStatus} 共用：三处若各写一份，会出现「恢复保留了记录、状态却报别的错」
     * 或「状态给了人工入口、恢复又把副本删了」这类自相矛盾。
     */
    private static boolean isAmbiguousLegacySite(@NonNull DirTxn txn, @Nullable ActivePointer p,
                                                @NonNull File bak) {
        if (txn.txnId != null || txn.committed || !txn.hadOld || !bak.exists()) return false;
        if (p == null || !txn.profileId.equals(p.profileId)) return false;
        return txn.prevActive == null || txn.prevActive.equals(txn.profileId);
    }

    /**
     * 事务是否**在效果上**已提交：记录自述已提交，或提交证据成立。
     *
     * <p><b>证据有两份，任一份成立即可</b>：
     * <ol>
     *   <li>{@code .commit_<profileId>} 见证里的代次 == 事务代次——**独立于激活指针**，是本轮修复
     *       的核心证据：指针会被下一次导入/切换覆盖，见证不会；</li>
     *   <li>激活指针里的代次 == 事务代次——{@code persistActive} 成功的直接痕迹，作为第二份见证。</li>
     * </ol>
     * 两份都用「与本次事务代次相等」判定；代次是每次不同的 nonce，脏内容不可能碰巧相等（不合法的
     * 代次在读入时已被降级为 {@code null}）。
     *
     * <p>抽成单一判据供 {@link #recoverTxn} / {@link #unresolvedSwapReason} / {@link #buildStatus}
     * 共用。三处若各写一份，会出现「恢复认为已提交、状态却报未完成」的自相矛盾。
     *
     * @param witnessTxnId {@code .commit_<profileId>} 里的代次（无见证时为 {@code null}）
     */
    private static boolean isCommitted(@NonNull DirTxn txn, @Nullable ActivePointer p,
                                       @Nullable String witnessTxnId) {
        if (txn.committed) return true;
        if (txn.txnId == null) return false;   // 旧格式记录：没有代次可比对
        return txn.txnId.equals(witnessTxnId) || (p != null && txn.txnId.equals(p.txnId));
    }

    /**
     * 已提交侧：保留新目录，只重试清理备份；**任何情况下不动指针**。
     *
     * <p>⚠️ **target 缺失时直接保留现场、不删备份**：此刻备份是唯一完整副本，删掉它等于把配置
     * 彻底丢失。「已提交」只说明**提交点已落定**，并不蕴含「新目录仍在」——新目录可能被外部删除
     * 或遭磁盘异常。这两件事必须分开判断。（A1 审计 N2）
     *
     * <p>保持现场后 {@code buildStatus} 会因目录缺失报 ERROR，用户可经
     * {@link #resolveAmbiguousSwap} 选「还原为备份」把配置救回来。
     */
    private void commitSide(@NonNull DirTxn txn, @NonNull File target, @NonNull File bak) {
        if (!target.exists()) {
            return;
        }
        if (bak.exists()) {
            fileOps.deleteRecursively(bak);
        }
        // 备份确实清掉才清记录；清不掉则保留记录，下次继续重试清理。
        if (!bak.exists()) {
            finishTxn(txn.profileId);
        }
    }

    /**
     * 未提交侧：回滚目录（删新目录 → 还原备份），**不动指针**。
     *
     * <p>{@code hadOld=false} 或 bak 不存在时无事可做：前者没有旧目录可还原；后者说明替换尚未
     * 越过「旧目录移开」，或被上一次回滚把 bak 消费掉了——两种情形该做的动作都是「清记录」，
     * 因为本事务从未改写过指针（否则会走已提交分支）。
     */
    private void rollbackDirOnly(@NonNull DirTxn txn, @NonNull File target, @NonNull File bak) {
        if (!txn.hadOld) {
            finishTxn(txn.profileId);
            return;
        }
        if (!bak.exists()) {
            finishTxn(txn.profileId);
            return;
        }
        if (target.exists() && !fileOps.deleteRecursively(target)) {
            return;   // 删不掉 → 保持现场，记录保留 → buildStatus 报 ERROR
        }
        if (!fileOps.rename(bak, target)) {
            return;   // 还原失败 → 保持现场，记录保留
        }
        finishTxn(txn.profileId);
    }

    /**
     * 旧格式记录（无提交代次，由本版本之前的代码写入）的恢复：**沿用旧语义**，仅对可判定的
     * 子情形加强（约束 2-4：不整体重写旧版兼容逻辑）。
     *
     * <p><b>四个分支（与代码逐条对应）</b>，记 {@code P} = 指针当前值：
     * <ol>
     *   <li>自述 {@code committed} ⇒ 同 {@link #commitSide}（且 target 缺失时不删备份）；</li>
     *   <li>{@code !hadOld} ⇒ 清记录（没有旧目录可还原，指针也未被本事务写过）；</li>
     *   <li>备份**不在**：
     *     <ul>
     *       <li>{@code P == profileId} 且 {@code prevActive} 是另一配置 ⇒ 判为「回滚已把备份还原成
     *           target、仅指针还原失败」，**补做** {@code restoreActivePointer(prevActive)}（失败则
     *           保留记录待重试）。这是相对旧实现的**加强**：旧实现据「备份不在」直接清记录，会把
     *           这次还原意图永久丢弃（P2）；</li>
     *       <li>其余 ⇒ 清记录（未越过「旧目录移开」，或没有需要还原的指针）。</li>
     *     </ul>
     *   </li>
     *   <li>备份**在**：
     *     <ul>
     *       <li>同名覆盖且 {@code P == profileId} ⇒ **证据不足**（见下），保留现场与记录；</li>
     *       <li>{@code P == profileId} 但 {@code prevActive} 是另一配置 ⇒ 判为**已提交**
     *           （未提交的事务不会写指针），走 {@link #commitSide}；</li>
     *       <li>否则 ⇒ 回滚目录（删 target → 还原备份），且仅当 {@code prevActive} 是另一配置时
     *           才还原指针。</li>
     *     </ul>
     *   </li>
     * </ol>
     *
     * <p>「补做指针还原」的正确性：{@code prevActive} 与 {@code profileId} 不同、而指针**已经是**
     * {@code profileId}——未提交的事务不可能把指针改成 {@code profileId}（旧实现只在提交点写指针，
     * 而提交成功会留下 {@code committed} 记录），故只能是「回滚已就位、指针没还原回去」，补做它
     * 并非猜测。
     *
     * <p><b>唯一保留的歧义（同名覆盖 + 备份在 + 指针已是本 profileId）</b>：此时「未提交」与
     * 「已提交但记录改写失败且备份也没删掉」两历史同形，而旧记录没有代次可判 → **保留全部副本
     * 与记录并报 ERROR，不猜测、不删除任一副本**（约束 1），由 {@link #resolveAmbiguousSwap}
     * 供用户显式了结。
     */
    private void recoverTxnLegacy(@NonNull DirTxn txn, @NonNull File target, @NonNull File bak) {
        if (txn.committed) {
            commitSide(txn, target, bak);
            return;
        }
        if (!txn.hadOld) {
            finishTxn(txn.profileId);
            return;
        }
        ActivePointer p = readActivePointer();
        boolean pointerIsTarget = p != null && txn.profileId.equals(p.profileId);
        boolean distinctPrev = txn.prevActive != null && !txn.prevActive.equals(txn.profileId);

        if (!bak.exists()) {
            // 「替换尚未开始」与「回滚已消费 bak、仅指针还原失败」同形。后者用指针值识别出来
            // （见方法注释的论证），补做那次失败的指针还原；失败则保留记录待下次重试。
            if (pointerIsTarget && distinctPrev) {
                if (!restoreActivePointer(txn.prevActive)) {
                    return;
                }
            }
            finishTxn(txn.profileId);
            return;
        }
        // bak 在。
        if (isAmbiguousLegacySite(txn, p, bak)) {
            // 同名覆盖：两历史同形（未提交 / 已提交但记录改写失败且备份没删掉），证据不足 →
            // 保留现场与记录、报 ERROR，由 resolveAmbiguousSwap 交用户了结。
            return;
        }
        if (pointerIsTarget) {
            // 指针已成了本 profileId，而 prevActive 是**另一个**配置：未提交的事务不会写指针
            // （旧实现只在提交点写指针，且提交成功会留下 committed 记录），故提交点必已落定，
            // 记录停在 in_progress 只因 committed 改写失败 → **保留新目录**，只重试清理备份。
            commitSide(txn, target, bak);
            return;
        }
        // 指针不是本 profileId ⇒ 未提交：回滚目录；仅当 prevActive 是另一个配置时才还原指针。
        if (target.exists() && !fileOps.deleteRecursively(target)) {
            return;
        }
        if (!fileOps.rename(bak, target)) {
            return;
        }
        if (distinctPrev && !restoreActivePointer(txn.prevActive)) {
            return;   // 指针还原失败 → 保留记录待下次重试
        }
        finishTxn(txn.profileId);
    }

    /**
     * 是否存在**未了结**的目录替换现场；有则禁止继续覆盖导入（Codex 要求 5）。
     *
     * <p>先尽力恢复一次：可自愈的现场（如未提交但未真正替换、已提交只差清备份）会被就地了结，
     * 不留阻塞理由；只有**恢复失败**或**歧义并存**才拒绝继续——否则下一次导入会覆盖掉待恢复的数据。
     *
     * @return 阻塞原因；{@code null} 表示可继续
     */
    @Nullable
    private String unresolvedSwapReason(@NonNull String profileId) {
        recoverProfileDirIfNeeded(profileId);

        File target = profileDir(profileId);
        File bak = bakDirOf(profileId);
        DirTxn txn = readTxn(profileId);
        ActivePointer p = readActivePointer();
        // 证据不足的旧格式歧义现场：必须走人工了结，文案要指向它（而不是含糊地说「未完成」）。
        if (txn != null && isAmbiguousLegacySite(txn, p, bak)) {
            return "检测到同名覆盖且无提交证据的歧义现场（新旧配置并存），已保留两份数据；"
                    + "请用「处理未决替换」选择保留当前目录或还原为备份";
        }
        // 用**统一判据**（记录自述 或 提交证据成立）：只看 txn.committed 会把「已提交但记录改写
        // 失败」误判为未完成，从而把一个可用配置永久阻塞。
        if (txn != null && !isCommitted(txn, p, readCommitWitness(profileId))) {
            return "上次目录替换未完成（未提交事务），已保留现场，请先排除磁盘占用再导入";
        }
        if (txn == null && !rollbackMarkerOf(profileId).exists() && target.exists() && bak.exists()) {
            return "检测到新旧配置并存且无事务记录，无法判定哪份应生效，请人工确认后再导入";
        }
        return null;
    }

    /**
     * 把激活指针写回指定值（{@code null} = 清除指针文件）。
     *
     * <p><b>本方法故意不写提交代次</b>：它不属于任何事务，伪造一个代次会让「指针代次 == 事务代次」
     * 这条提交证据失去意义。因此它**只允许被旧格式恢复路径调用**（那条路径的事务本就没有代次）。
     * 新的事务若要写指针，必须走 {@link #persistActive(String, String)} 并传入本次事务的代次。
     *
     * @return 是否确实落盘；失败时调用方**不得**清除事务记录，留待下次重试
     */
    private boolean restoreActivePointer(@Nullable String prevActive) {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        if (prevActive == null) {
            //noinspection ResultOfMethodCallIgnored
            dst.delete();
            return !dst.exists();
        }
        return fileOps.writeSync(dst, prevActive.getBytes(StandardCharsets.UTF_8));
    }

    // ---------------- 状态组装 ----------------

    @NonNull
    private FaceRuntimeStatus buildStatus(@Nullable String nativeJson) {
        int count = listProfiles().size();
        String active = readActive();
        FaceRuntimeStatus base;
        if (active == null) {
            base = FaceRuntimeStatus.notImported(count);
        } else {
            // #3 崩溃恢复 + 第五轮审查 P1：回滚未完成 / 目录替换窗口的残留现场先恢复就位，再判状态。
            // 约束 2-3：恢复**可能改写或清除**激活指针（旧格式恢复会补做指针还原），故恢复后必须
            // **重新读取**——否则会把恢复前的配置状态报给 UI。有界两跳，不引入循环：恢复链实际最多
            // 一跳，但代码**不保证**这一点；两跳后仍变化即按当前值如实判定，绝不空转。
            // （A1 审计 N6：原注释写成「最多一跳」，那是关于实现的断言而非代码保证，已改为诚实表述。）
            for (int pass = 0; pass < 2 && active != null; pass++) {
                recoverProfileDirIfNeeded(active);
                String after = readActive();
                if (after == null) {
                    active = null;
                    break;
                }
                if (after.equals(active)) {
                    break;
                }
                active = after;
            }
        }
        if (active == null) {
            // 未导入；或恢复把指针清掉了（旧格式回滚到「无激活配置」）——两者对外同语义。
            return FaceRuntimeStatus.notImported(count).withNativeStatus(nativeJson);
        }
        File dir = profileDir(active);
        File mf = new File(dir, MANIFEST_FILE_NAME);
        File bak = bakDirOf(active);
        DirTxn pendingTxn = readTxn(active);
        ActivePointer pendingPtr = readActivePointer();
        if (pendingTxn != null && bak.exists() && !dir.isDirectory()) {
            // 记录仍在 + **目标目录缺失** + 备份在：备份是此刻**唯一完整副本**。可能是「已提交但新
            // 目录被外部删除」，也可能是「未提交且 target 被删」——两历史同形，**不猜**。保留两份
            // 数据与记录，并给出人工了结入口：此时唯一可行的选择是「还原为备份」。
            // （A1 审计 N2/N3：修了「commitSide 不许删唯一副本」之后，必须同时给出这条出路，否则
            //   用户会看到 ERROR 却没有任何可执行动作。）
            base = FaceRuntimeStatus.errorAmbiguousSwap(active, count,
                    "目标目录缺失但存在备份（" + bak.getName() + "），已保留现场；"
                            + "请选择「还原为备份」把配置救回", active);
        } else if (pendingTxn != null && isAmbiguousLegacySite(pendingTxn, pendingPtr, bak)) {
            // 证据不足的旧格式歧义现场：恢复了「保留两份数据」，需用户显式了结。带机器可读的
            // ambiguousSwapProfileId，供 UI 在多种 ERROR 原因中准确识别并给出操作入口（约束 1）。
            base = FaceRuntimeStatus.errorAmbiguousSwap(active, count,
                    "检测到同名覆盖且无提交证据的歧义现场（新旧配置并存），已保留两份数据；"
                            + "请选择保留当前目录或还原为备份（" + bak.getName() + "）", active);
        } else if (pendingTxn != null && !isCommitted(pendingTxn, pendingPtr, readCommitWitness(active))) {
            // 未提交的事务仍在 = 恢复未能完成（删不掉新目录 / 还原失败 / 指针写不回）。
            // 既不能报 ready（会让人以为新模型在跑），也不能承诺「已回滚」——如实报错并要求人工介入。
            base = FaceRuntimeStatus.error(active, count,
                    "目录替换事务未完成（未提交），已保留现场；请排除磁盘占用后重试，勿反复导入");
        } else if (rollbackMarkerOf(active).exists() && bak.exists()) {
            // 仅当「意图标记在**且**旧模型仍在备份目录」才是真正卡住的回滚：此时既不能报 ready
            //（会掩盖真实故障、让人以为新模型在跑），也不能承诺「已回滚」——如实报 ERROR 并指出
            // 原模型位置。若备份已被消费，说明还原其实已经完成（仅标记删除失败，A3' γ7），
            // 落入下方常规目录判定即可；否则会误报 ERROR 并谎称旧模型仍在备份里。
            base = FaceRuntimeStatus.error(active, count,
                    "上次模型回滚未完成：原模型仍滞留在 " + bak.getName() + "，当前配置未生效");
        } else if (pendingTxn == null && !rollbackMarkerOf(active).exists()
                && dir.isDirectory() && mf.isFile() && bak.exists()) {
            // 无事务记录、无回滚标记，却「新目录与旧备份并存」：**无法判定哪份应生效**
            //（旧版本遗留；自动恢复 bak 会误撤销一次成功更新，故绝不猜）。保留两份数据（Codex 要求 6），
            // 同样带机器可读标记给出人工了结入口。
            base = FaceRuntimeStatus.errorAmbiguousSwap(active, count,
                    "检测到未完成的目录替换（新旧配置并存且无事务记录），已保留两份数据；"
                            + "请选择保留当前目录或还原为备份（" + bak.getName() + "）", active);
        } else if (!dir.isDirectory() || !mf.isFile()) {
            base = FaceRuntimeStatus.error(active, count, "激活配置目录缺失：" + active);
        } else {
            try {
                String json = readTextFile(mf, limits.maxManifestBytes);
                // 展示态不传 ABI，跳过 ABI 匹配（导入时已校验过）。
                FaceModelManifest.ParseResult pr = FaceModelManifest.parse(json, null);
                if (!pr.isOk()) {
                    base = FaceRuntimeStatus.error(active, count, "激活配置 manifest 无效：" + pr.message);
                } else {
                    base = FaceRuntimeStatus.ready(pr.profile.profileId, pr.profile.displayName,
                            pr.profile.modelVersion, pr.profile.licenseId, count, false, 0);
                }
            } catch (Exception ex) {
                base = FaceRuntimeStatus.error(active, count, "激活配置读取失败");
            }
        }
        return base.withNativeStatus(nativeJson);
    }

    @Nullable
    private String nativeStatusOrNull() {
        try {
            return runtime.getRuntimeStatus();
        } catch (Throwable t) {
            // native 库未加载/不可用时状态仍可用（UI 不崩）。
            return null;
        }
    }

    @NonNull
    private Result fail(@NonNull Status status, @NonNull String message) {
        return new Result(status, buildStatus(null), message);
    }

    // ---------------- 文件工具 ----------------

    private boolean ensureStoreDir() {
        return storeDir.isDirectory() || storeDir.mkdirs();
    }

    /**
     * 清理遗留临时残留：解压临时目录 {@code .tmp_<nano>} 与暂存目录 {@code .staging/}（任务 A4）。
     *
     * <p>此前**没有任何清理路径**：解压阶段（或其后任何时刻）进程被杀，临时目录就永久滞留在
     * `files/face_models/` 下——只被 `listProfiles()` 的点号前缀规则忽略，磁盘上却一直占着。
     *
     * <p>安全性：本方法只在**持 {@code dirLock}** 时调用，而 `importZip` 全程持锁，故不会误删
     * 正在使用的临时目录。前缀 {@code .tmp_} 在本目录内唯一标识解压临时目录（事务记录是
     * {@code .txn_*}、指针临时文件是 {@code active_profile.tmp}，均不匹配）。
     *
     * <p><b>已修复（同名歧义）</b>：事务记录 / 提交见证的临时文件原先放在 <b>顶层</b>，名字由
     * profileId 拼成（{@code .txn_<id>.tmp} / {@code .commit_<id>.tmp}）。而清扫只能按名字前缀匹配，
     * {@code isSafeDirName} 又放行点号——于是 profileId 恰为 {@code foo.tmp} 时，它的**活事务记录**
     * {@code .txn_foo.tmp} 与「profileId 为 foo 的临时文件」**同名**，按「{@code .txn_} 前缀 +
     * {@code .tmp} 后缀」清扫就会静默销毁一份活证据，把本可自动收敛的现场退化为必须人工了结的
     * 歧义现场。提交见证一侧当时只能靠「**刻意不进清扫**」自保，代价是「进程死在写入与改名之间」
     * 留下的半截文件永久滞留（一笔欠债）。
     *
     * <p>现在临时文件已迁入**名字恒定**的暂存子目录 {@code .staging/}（{@code .staging/txn/<profileId>}、
     * {@code .staging/commit/<profileId>}），子目录名与文件名都不含额外的点号后缀、且 {@code .staging}
     * 以点号开头，故**结构上**不可能与任何正式产物同名。因此 {@code .staging/} 里只可能是临时文件，
     * **整体清空**是纯正语义，也能安全地连带清掉提交见证的临时文件——上面那笔欠债就此还清。
     * （{@code .tmp_<nano>/} 的解压临时目录语义不变，仍单独清扫。）
     */
    private void sweepStaleTempDirs() {
        File[] files = storeDir.listFiles();
        if (files == null) return;
        for (File f : files) {
            String n = f.getName();
            // ① 解压临时目录 .tmp_<nano>（动态名）；② 暂存目录 .staging（里面只可能是临时文件）。
            if (!n.startsWith(".tmp_") && !STAGING_DIR.equals(n)) continue;
            if (f.isDirectory()) {
                deleteRecursivelyQuietly(f);
            } else {
                // 万一被占成同名文件（非目录）：一并删掉，否则仍会挡路。
                //noinspection ResultOfMethodCallIgnored
                f.delete();
            }
        }
    }

    /** canonical 路径包含性判断：{@code child} 必须位于 {@code parent} 之内。 */
    private static boolean isContained(@NonNull File parent, @NonNull File child) {
        try {
            String p = parent.getCanonicalPath();
            String c = child.getCanonicalPath();
            return c.equals(p) || c.startsWith(p + File.separator);
        } catch (IOException ex) {
            return false;
        }
    }

    /** 是否为 Git LFS pointer 文本（前 512 字节的 ASCII 头）。 */
    private static boolean isLfsPointer(@NonNull File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] head = new byte[512];
            int got = in.read(head);
            String s = new String(head, 0, Math.max(got, 0), StandardCharsets.US_ASCII);
            return s.contains(LFS_MARKER);
        } catch (Exception ex) {
            return false;
        }
    }

    /** 流式计算文件 SHA-256（小写 hex）；失败返回 null。 */
    @Nullable
    private static String sha256Hex(@NonNull File f) {
        try (FileInputStream in = new FileInputStream(f)) {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] buf = new byte[1 << 20];
            int n;
            while ((n = in.read(buf)) > 0) {
                md.update(buf, 0, n);
            }
            return toHex(md.digest());
        } catch (Exception ex) {
            return null;
        }
    }

    @NonNull
    private static String readTextFile(@NonNull File f, long maxBytes) throws ImportFailure {
        if (f.length() > maxBytes) {
            throw new ImportFailure(Status.TOO_LARGE, "文件过大：" + f.getName());
        }
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            long total = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                total += n;
                if (total > maxBytes) {
                    throw new ImportFailure(Status.TOO_LARGE, "文件过大：" + f.getName());
                }
                out.write(buf, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (IOException ex) {
            throw new ImportFailure(Status.IO_ERROR, "文件读取失败：" + f.getName());
        }
    }

    private void deleteRecursivelyQuietly(@NonNull File f) {
        try {
            fileOps.deleteRecursively(f);
        } catch (Exception ignored) {
        }
    }

    /**
     * 递归删除；返回是否**确实已不存在**。
     *
     * <p>删除失败（文件被占用等）不抛错——多数调用方只需 best-effort 清理；但回滚路径必须据此
     * 判定「新目录是否真的清空」，否则会在 target 仍在时误判为可还原（第五轮审查 P1）。
     */
    private static boolean deleteRecursively(@NonNull File f) {
        if (!f.exists()) return true;
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) {
                    deleteRecursively(c);
                }
            }
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
        return !f.exists();
    }

    @NonNull
    private Object dirLock() {
        return FileStoreUtils.dirLockFor(storeDir);
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
}
