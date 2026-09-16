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
import java.util.concurrent.ConcurrentHashMap;
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
 *   active_profile            当前激活的 profileId（纯文本）
 *   &lt;profileId&gt;/manifest.json
 *   &lt;profileId&gt;/scrfd.param ...（模型文件）
 *   .tmp_* / .bak_*           过渡目录，任何校验失败都会被清理
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

    /** 默认存储目录名（相对 {@code files/}）。 */
    private static final String DEFAULT_STORE_DIR = "face_models";

    /** gallery（已注册人员模板）目录名，与 {@code FeatureTemplateEncryptedStore.defaultDir} 一致。 */
    private static final String GALLERY_DIR_NAME = "face_templates";

    /** Git LFS pointer 首行特征（模型未拉取）。 */
    private static final String LFS_MARKER = "version https://git-lfs.github.com/spec/v1";

    /**
     * 按规范化 storeDir 路径共享的静态锁：同目录多实例并发导入/切换会互相破坏指针与目录，
     * 必须串行化（与 {@link AndroidEnrollmentRepository} 同一策略）。
     */
    private static final ConcurrentHashMap<String, Object> DIR_LOCKS = new ConcurrentHashMap<>();

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

                // 记录旧激活指针：激活/落定失败时保留旧配置。
                String previousActive = readActive();

                // Codex 要求 5：存在未了结的替换现场时**禁止继续覆盖导入**（先尽力恢复一次）。
                String blocked = unresolvedSwapReason(profile.profileId);
                if (blocked != null) {
                    return fail(Status.ATOMIC_RENAME_FAILED, blocked);
                }

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
                        clearTxn(profile.profileId);
                    }
                    // #1-Low-2（第三波审查）：按是否有旧同名配置区分失败文案，避免首次导入也误报
                    // 「已保留原配置」（此时并无旧配置）。
                    String tail = swap.hadOld
                            ? "已回滚到原模型"
                            : "已保留新导入的模型目录但未激活";
                    return new Result(Status.RUNTIME_ACTIVATE_FAILED, buildStatus(nativeStatusOrNull()),
                            "模型已导入，但实时管线激活失败：" + ao.reason + "，" + tail);
                }
                // ⑨ 生效后才原子落定激活指针；落定失败则回滚目录到导入前状态。
                if (!persistActive(profile.profileId)) {
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
                    return fail(Status.ATOMIC_RENAME_FAILED, "激活指针写入失败，已回滚配置");
                }
                // 事务提交（Codex 要求 3）：指针已落定 → 先把记录改写为 committed，再清备份，最后清记录。
                // 改写失败时**继续**而**不回滚**：此刻激活确已成功（native 提交 + 指针落定都在前），
                // 因「记录写失败」而回滚一个可用配置更糟。此举安全，理由见 recoverProfileDirIfNeeded
                // 的状态推导规则——本路径随后会删掉 bak，而「in_progress + 无 bak」一律判为「未曾替换」，
                // 故即便记录残留也会收敛到「保留新目录」。
                writeTxn(new DirTxn(swap.profileId, swap.hadOld, previousActive, true));
                deleteRecursivelyQuietly(swap.bak);
                clearTxn(swap.profileId);

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
                if (!persistActive(profile.profileId)) {
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
        return staticLockFor(storeDir);
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
        boolean hadOld = target.exists();

        // 事务记录（Codex 要求 1）：**破坏性改名前**可靠落 in_progress；写不进去就绝不动手，
        // 旧配置保持原位。记录里的 hadOld + 原激活指针供恢复时「退旧」使用。
        if (!writeTxn(new DirTxn(profileId, hadOld, prevActive, false))) {
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
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED, "临时目录无法原子重命名");
        }
        // 替换已成功落定：目录语义完全由本 swap 承担，此前遗留的回滚意图标记随之失效，必须清除，
        // 否则 buildStatus 会因残留标记把本次新配置误判为「回滚未完成」而报 ERROR。
        clearRollbackMarker(profileId);
        return new ProfileSwap(target, bak, hadOld, profileId);
    }

    /** 目录替换句柄：记录旧目录备份，供激活指针落定成功时清理、失败时回滚。 */
    private static final class ProfileSwap {
        final File target;
        final File bak;
        final boolean hadOld;
        /** 本次替换的 profileId——回滚写入意图标记时需要（target/bak 名称已被改写，不能反推）。 */
        final String profileId;

        ProfileSwap(File target, File bak, boolean hadOld, String profileId) {
            this.target = target;
            this.bak = bak;
            this.hadOld = hadOld;
            this.profileId = profileId;
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
            // 首次导入无旧配置可还原：删掉新目录即完成回滚。删除失败不影响对外语义——
            // 该目录从未被激活指针引用，最坏只是磁盘残留。
            deleteRecursivelyQuietly(swap.target);
            clearTxn(swap.profileId);   // 已在进程内完成回滚，无需启动恢复
            return RollbackOutcome.RESTORED;
        }
        // 尽力记录回滚意图：它能覆盖「删除过程中崩溃」等窄窗口，但记录失败不应阻止还原。
        final boolean recorded = writeRollbackMarker(swap);
        deleteRecursivelyQuietly(swap.target);
        if (!swap.target.exists() && fileOps.rename(swap.bak, swap.target)) {
            clearRollbackMarker(swap.profileId);  // 回滚完整完成，意图标记使命结束
            clearTxn(swap.profileId);             // 事务亦然：现场已回到替换前
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

    /** 事务记录文件名前缀：{@code <storeDir>/.txn_<profileId>}（点号前缀 → 被 listProfiles 忽略）。 */
    private static final String TXN_PREFIX = ".txn_";
    private static final String TXN_STATE_IN_PROGRESS = "in_progress";
    private static final String TXN_STATE_COMMITTED = "committed";

    /** 目录替换事务记录（Codex 要求 2：目标 profile / 是否有旧目录 / 原激活指针 / 提交状态）。 */
    private static final class DirTxn {
        @NonNull final String profileId;
        final boolean hadOld;
        @Nullable final String prevActive;
        final boolean committed;

        DirTxn(@NonNull String profileId, boolean hadOld, @Nullable String prevActive, boolean committed) {
            this.profileId = profileId;
            this.hadOld = hadOld;
            this.prevActive = prevActive;
            this.committed = committed;
        }

        @NonNull
        DirTxn asCommitted() {
            return new DirTxn(profileId, hadOld, prevActive, true);
        }

        @NonNull
        byte[] encode() {
            return ("profileId=" + profileId + "\n"
                    + "hadOld=" + hadOld + "\n"
                    + "prevActive=" + (prevActive == null ? "" : prevActive) + "\n"
                    + "state=" + (committed ? TXN_STATE_COMMITTED : TXN_STATE_IN_PROGRESS) + "\n")
                    .getBytes(StandardCharsets.UTF_8);
        }
    }

    /** 事务记录路径。 */
    @NonNull
    private File txnFileOf(@NonNull String profileId) {
        return new File(storeDir, TXN_PREFIX + profileId);
    }

    /**
     * 读事务记录。
     *
     * @return 记录；无文件或内容不可解析（含 profileId 不匹配）时返回 null——
     *         调用方必须把 null 当作「**无记录**」而非「无事务」，后者需按歧义现场处理。
     */
    @Nullable
    private DirTxn readTxn(@NonNull String profileId) {
        File f = txnFileOf(profileId);
        if (!f.isFile()) return null;
        String text;
        try (FileInputStream fis = new FileInputStream(f)) {
            byte[] buf = new byte[512];   // 记录 < 200 字节，单次读足够
            int n = fis.read(buf);
            if (n <= 0) return null;
            text = new String(buf, 0, n, StandardCharsets.UTF_8);
        } catch (Exception e) {
            return null;
        }
        String id = null;
        String prev = "";
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
            else if ("state".equals(k)) committed = TXN_STATE_COMMITTED.equals(v);
        }
        if (id == null || !id.equals(profileId)) return null;
        return new DirTxn(id, hadOld, prev.isEmpty() ? null : prev, committed);
    }

    /**
     * 原子落事务记录（临时文件 + fsync + 改名）。
     *
     * <p><b>返回值必须被检查</b>：破坏性改名前写不进去就**绝不能动手**（Codex 要求 1），
     * 否则会留下「改了一半、且无任何记录可依」的现场。
     */
    private boolean writeTxn(@NonNull DirTxn txn) {
        File tmp = new File(storeDir, TXN_PREFIX + txn.profileId + ".tmp");
        if (!fileOps.writeSync(tmp, txn.encode())) {
            return false;
        }
        if (!fileOps.rename(tmp, txnFileOf(txn.profileId))) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return false;
        }
        return true;
    }

    /** 清除事务记录（无可清除时静默）。 */
    private void clearTxn(@NonNull String profileId) {
        //noinspection ResultOfMethodCallIgnored
        txnFileOf(profileId).delete();
    }

    /** 原子写激活指针：tmp + bak 替换（与注册表落盘同策略）。 */
    private boolean persistActive(@NonNull String profileId) {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File tmp = new File(storeDir, ACTIVE_POINTER_FILE + ".tmp");
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
        // 阶段 1：#3 崩溃恢复——上次写入在「dst→bak」之后、「tmp→dst」完成前退出时主指针缺失、
        // 备份尚在；先把备份恢复为可回滚基线，否则下面会先删备份再写，指针在崩溃窗口中丢失。
        // 恢复失败则宁可不写（绝不删除唯一副本）。
        if (!dst.exists() && bak.exists() && !fileOps.rename(bak, dst)) {
            return false;
        }
        try (FileOutputStream fos = new FileOutputStream(tmp, false)) {
            fos.write(profileId.getBytes(StandardCharsets.UTF_8));
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Exception ignored) {
            }
        } catch (Exception ex) {
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
     * 读取当前激活指针；#3 崩溃恢复：主指针缺失/不可读时回退 {@code active_profile.bak}
     * 并把备份 rename 回主指针（复用 {@link AndroidEnrollmentRepository} 的 .bak 恢复模式）。
     * 主指针与备份都不可用时返回 null（未导入）。
     */
    @Nullable
    private String readActive() {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
        if (!dst.isFile()) {
            if (bak.isFile()) {
                // 崩溃恢复：主指针缺失但备份是唯一完整副本，先解析备份再 rename 回主。
                String id = parseActiveFile(bak);
                if (id != null) {
                    fileOps.rename(bak, dst);
                    return id;
                }
            }
            return null;
        }
        String id = parseActiveFile(dst);
        if (id == null && bak.isFile()) {
            // 主指针存在但不可读/非法：从备份恢复，避免退化为「未导入」丢失激活。
            String fromBak = parseActiveFile(bak);
            if (fromBak != null) {
                fileOps.rename(bak, dst);
                return fromBak;
            }
        }
        return id;
    }

    /** 解析指针文件内容为安全 profileId；缺失/不可读/非法返回 null。 */
    @Nullable
    private static String parseActiveFile(@NonNull File f) {
        try {
            String id = readTextFile(f, 4096).trim();
            return FaceModelManifest.isSafeDirName(id) ? id : null;
        } catch (Exception ex) {
            return null;
        }
    }

    /** 静默重命名（崩溃恢复用）；失败返回 false，由调用方决定后续语义。 */
    private static boolean renameQuietly(@NonNull File from, @NonNull File to) {
        try {
            return from.renameTo(to);
        } catch (SecurityException ex) {
            return false;
        }
    }

    // ── 文件操作接缝（任务 A，2026-09-16 决策）────────────────────────────────────
    // 用途：目录替换的事务协议有大量**破坏性**步骤（改名 / 删除 / 落事务记录），其失败分支在纯 JVM
    // 下难以构造（本类是 final、原方法均为 private static，「非空目录占位」只覆盖得了少数情形）。
    // 故把这三类操作收敛为可注入接口，与 FeatureTemplateEncryptedStore.setPlatformKeyProviderForTesting
    // 同构。默认实现即原有行为，**生产路径零变化**。

    @androidx.annotation.VisibleForTesting
    interface FileOps {
        /** 改名；语义同 {@link File#renameTo}（含吞 SecurityException）。 */
        boolean rename(@NonNull File from, @NonNull File to);

        /** 递归删除；返回是否**确实已不存在**。 */
        boolean deleteRecursively(@NonNull File f);

        /** 写入 + flush + fsync（事务记录等需落盘保证的小文件）；返回是否成功。 */
        boolean writeSync(@NonNull File f, @NonNull byte[] data);
    }

    private static final FileOps DEFAULT_FILE_OPS = new FileOps() {
        @Override
        public boolean rename(@NonNull File from, @NonNull File to) {
            return renameQuietly(from, to);
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

    private static volatile FileOps fileOps = DEFAULT_FILE_OPS;

    /** 仅供测试注入；传 null 恢复默认（真实文件系统）实现。 */
    @androidx.annotation.VisibleForTesting
    static void setFileOpsForTesting(@Nullable FileOps ops) {
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
     * <p><b>状态推导</b>（无需额外状态字段——由记录 + 磁盘即可判定「替换是否已开始」）：
     * <ul>
     *   <li>{@code hadOld} 且 bak 存在 ⇒ 已越过「旧目录移开」这一步，替换**已开始**；</li>
     *   <li>{@code !hadOld} 且 target 存在 ⇒ 新目录已就位（此前 target 不存在），替换**已开始**。</li>
     * </ul>
     * 未开始 ⇒ 磁盘即原状，清掉记录即可；已开始且**未提交** ⇒ 回滚到旧目录与原指针；
     * 已提交 ⇒ 保留新目录、继续清理备份。
     *
     * <p>任一步失败都**保持现场并保留记录**，由 {@code buildStatus} 报 ERROR 且拒绝继续覆盖导入。
     */
    private void recoverTxn(@NonNull DirTxn txn, @NonNull File target, @NonNull File bak) {
        if (txn.committed) {
            // 已提交：保留新目录，继续清理备份；备份确实清掉后才清记录（否则下次继续重试清理）。
            if (bak.exists()) {
                fileOps.deleteRecursively(bak);
            }
            if (!bak.exists()) {
                clearTxn(txn.profileId);
            }
            return;
        }
        // 未提交：恢复到替换前的状态。
        //
        // !hadOld 特例：**无需回滚**——没有旧目录可还原，且指针未改动（persistActive 在提交记录之后
        // 才执行）。保留下来的新包只是一个「可激活但尚未激活」的目录，不影响状态（active 仍为 null
        // → NOT_IMPORTED）。这与首次导入失败时的非崩溃语义一致（那里也是保留目录、不切指针）。
        if (!txn.hadOld) {
            clearTxn(txn.profileId);
            return;
        }
        // hadOld：bak 不存在 ⇒ 尚未越过「旧目录移开」，磁盘即原状。
        if (!bak.exists()) {
            clearTxn(txn.profileId);
            return;
        }
        // 已越过「旧目录移开」且未提交 ⇒ 回滚到旧目录与原指针。先删新目录，备份才有位置还原。
        if (target.exists() && !fileOps.deleteRecursively(target)) {
            return;   // 删不掉 → 保持现场，记录保留 → buildStatus 报 ERROR
        }
        if (!fileOps.rename(bak, target)) {
            return;   // 还原失败 → 保持现场，记录保留
        }
        if (!restoreActivePointer(txn.prevActive)) {
            return;   // 指针还原失败 → 保留记录待下次重试
        }
        clearTxn(txn.profileId);
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

        DirTxn txn = readTxn(profileId);
        if (txn != null && !txn.committed) {
            return "上次目录替换未完成（未提交事务），已保留现场，请先排除磁盘占用再导入";
        }
        File target = profileDir(profileId);
        File bak = bakDirOf(profileId);
        if (txn == null && !rollbackMarkerOf(profileId).exists() && target.exists() && bak.exists()) {
            return "检测到新旧配置并存且无事务记录，无法判定哪份应生效，请人工确认后再导入";
        }
        return null;
    }

    /**
     * 把激活指针写回指定值（{@code null} = 清除指针文件）。
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
            recoverProfileDirIfNeeded(active);
            File dir = profileDir(active);
            File mf = new File(dir, MANIFEST_FILE_NAME);
            DirTxn pendingTxn = readTxn(active);
            if (pendingTxn != null && !pendingTxn.committed) {
                // 未提交的事务仍在 = 恢复未能完成（删不掉新目录 / 还原失败 / 指针写不回）。
                // 既不能报 ready（会让人以为新模型在跑），也不能承诺「已回滚」——如实报错并要求人工介入。
                base = FaceRuntimeStatus.error(active, count,
                        "目录替换事务未完成（未提交），已保留现场；请排除磁盘占用后重试，勿反复导入");
            } else if (rollbackMarkerOf(active).exists() && bakDirOf(active).exists()) {
                // 仅当「意图标记在**且**旧模型仍在备份目录」才是真正卡住的回滚：此时既不能报 ready
                //（会掩盖真实故障、让人以为新模型在跑），也不能承诺「已回滚」——如实报 ERROR 并指出
                // 原模型位置。若备份已被消费，说明还原其实已经完成（仅标记删除失败，A3' γ7），
                // 落入下方常规目录判定即可；否则会误报 ERROR 并谎称旧模型仍在备份里。
                base = FaceRuntimeStatus.error(active, count,
                        "上次模型回滚未完成：原模型仍滞留在 " + bakDirOf(active).getName() + "，当前配置未生效");
            } else if (pendingTxn == null && !rollbackMarkerOf(active).exists()
                    && dir.isDirectory() && mf.isFile() && bakDirOf(active).exists()) {
                // 无事务记录、无回滚标记，却「新目录与旧备份并存」：**无法判定哪份应生效**
                //（旧版本遗留；自动恢复 bak 会误撤销一次成功更新，故绝不猜）。保留两份数据（Codex 要求 6）。
                base = FaceRuntimeStatus.error(active, count,
                        "检测到未完成的目录替换（新旧配置并存且无事务记录），已保留两份数据，请人工确认");
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

    private static void deleteRecursivelyQuietly(@NonNull File f) {
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
        return staticLockFor(storeDir);
    }

    @NonNull
    private static Object staticLockFor(@NonNull File dir) {
        String key;
        try {
            key = dir.getCanonicalPath();
        } catch (Exception ex) {
            key = dir.getAbsolutePath();
        }
        Object lock = DIR_LOCKS.get(key);
        if (lock == null) {
            Object fresh = new Object();
            Object prev = DIR_LOCKS.putIfAbsent(key, fresh);
            lock = (prev != null) ? prev : fresh;
        }
        return lock;
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
