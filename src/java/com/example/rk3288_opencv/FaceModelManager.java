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

                // ⑦ 目录原子替换（rename 基目录到 <profileId>），但**先不写激活指针**：
                //    只有实时管线激活成功才落定指针（#2）。
                ProfileSwap swap = swapInProfileDirs(base, profile.profileId);

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
                        if (!rollbackProfileSwap(swap)) {
                            FaceRuntimeStatus st = buildStatus(nativeStatusOrNull());
                            // 按**落盘后状态**而非仅标记存在性给出如实文案（外部审计：writeRollbackMarker
                            // 的失败被静默忽略，故极端 I/O 下不得承诺可自动恢复）：
                            // · 备份已被消费 → 回滚其实已就位（buildStatus 可能刚就地完成回滚）；
                            // · 备份仍在且有标记 → 可启动恢复；
                            // · 备份仍在但**无标记** → 标记写入失败，绝不承诺「将由启动恢复」。
                            String detail;
                            if (!swap.bak.exists()) {
                                detail = "，旧配置已回滚就位";
                            } else if (rollbackMarkerOf(swap.profileId).exists()) {
                                detail = "且旧配置回滚失败（旧目录滞留于 " + swap.bak.getName() + "，将由启动恢复）";
                            } else {
                                detail = "且旧配置回滚失败，且未能记录恢复意图（旧目录滞留于 "
                                        + swap.bak.getName() + "）——请排除文件占用后重试导入，或重启应用";
                            }
                            return new Result(Status.ATOMIC_RENAME_FAILED, st,
                                    "模型已导入，但实时管线激活失败：" + ao.reason + "，" + detail);
                        }
                    } else {
                        deleteRecursivelyQuietly(swap.bak);
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
                    boolean restored = rollbackProfileSwap(swap);
                    if (!restored) {
                        return fail(Status.ATOMIC_RENAME_FAILED,
                                "激活指针写入失败，且旧配置回滚失败（旧目录滞留于 "
                                        + swap.bak.getName() + "）");
                    }
                    bestEffortReactivate(previousActive, currentAbi);
                    return fail(Status.ATOMIC_RENAME_FAILED, "激活指针写入失败，已回滚配置");
                }
                deleteRecursivelyQuietly(swap.bak);

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
    private ProfileSwap swapInProfileDirs(@NonNull File base, @NonNull String profileId)
            throws ImportFailure {
        File target = profileDir(profileId);
        File bak = bakDirOf(profileId);
        // 先了结上一次未完成的回滚（第五轮审查 A3' γ5）：若残留 .rollback_<id> 意图标记，下面的
        // deleteRecursivelyQuietly(bak) 会先删掉**旧模型备份**，而 target 仍是那个激活失败的新包
        // ——旧模型就此永久丢失。此调用同时覆盖崩溃窗口（target 缺失 + bak 存在）现场。
        recoverProfileDirIfNeeded(profileId);
        if (!target.exists() && bak.exists()) {
            // 崩溃恢复：旧目录是唯一完整副本，恢复失败不能继续（否则下一步会删掉它）。
            if (!renameQuietly(bak, target)) {
                throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                        "检测到上次替换残留（" + bak.getName() + "），恢复失败，已中止以保留旧配置");
            }
        }
        deleteRecursivelyQuietly(bak);
        boolean hadOld = target.exists();
        if (hadOld && !target.renameTo(bak)) {
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED, "旧配置目录无法移开");
        }
        boolean moved = base.renameTo(target);
        if (!moved) {
            // 回滚失败必须显式上报，绝不静默：否则旧目录仅以 .bak_<id> 滞留、指针悬空。
            boolean restored = !hadOld || bak.renameTo(target);
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

    /**
     * 目录回滚：删除激活失败的新目录，并把旧目录（{@code .bak_<id>}）还原就位。
     *
     * <p><b>返回 false 有两种现场，恢复逻辑都必须能覆盖（第五轮审查 P1）：</b>
     * <ul>
     *   <li>新目录已删掉、仅改名还原失败 → target 缺失、bak 仍在（崩溃窗口语义，直接还原）。</li>
     *   <li>新目录**没删掉**（文件被占用等）→ target 与 bak **同时存在**。此时
     *       {@code bak.renameTo(target)} 对非空目录必然失败，而恢复逻辑原判据是「target 缺失」，
     *       永远不会触发——旧模型永久滞留，激活指针却指向那个激活失败的新包。</li>
     * </ul>
     *
     * <p>故改为 <b>intent-first</b>：先写 {@code .rollback_<id>} 意图标记，再执行删除+还原，成功才清标记。
     * 这样不仅覆盖「删除失败」，也覆盖「删除过程中进程崩溃」（留半删除目录）——恢复逻辑据标记完成收尾。
     */
    private boolean rollbackProfileSwap(@NonNull ProfileSwap swap) {
        if (!swap.hadOld) {
            // 首次导入无旧配置可还原：删掉新目录即完成回滚。删除失败不影响对外语义——
            // 该目录从未被激活指针引用，最坏只是磁盘残留。
            deleteRecursivelyQuietly(swap.target);
            return true;
        }
        writeRollbackMarker(swap);
        deleteRecursivelyQuietly(swap.target);
        if (!swap.target.exists() && swap.bak.renameTo(swap.target)) {
            clearRollbackMarker(swap.profileId);  // 回滚完整完成，意图标记使命结束
            return true;
        }
        return false;
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
     * <p>写入失败不改返回值语义：调用方已按「回滚失败」上报，此处仅是让启动恢复有机会补救。
     */
    private void writeRollbackMarker(@NonNull ProfileSwap swap) {
        try (FileOutputStream fos = new FileOutputStream(rollbackMarkerOf(swap.profileId), false)) {
            fos.write(("target=" + swap.target.getName() + "\n").getBytes(StandardCharsets.UTF_8));
            fos.flush();
            try {
                fos.getFD().sync();
            } catch (Exception ignored) {
            }
        } catch (Exception ignored) {
        }
    }

    /** 清除回滚意图标记（无可清除时静默）。 */
    private void clearRollbackMarker(@NonNull String profileId) {
        //noinspection ResultOfMethodCallIgnored
        rollbackMarkerOf(profileId).delete();
    }

    /** 原子写激活指针：tmp + bak 替换（与注册表落盘同策略）。 */
    private boolean persistActive(@NonNull String profileId) {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File tmp = new File(storeDir, ACTIVE_POINTER_FILE + ".tmp");
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
        // 阶段 1：#3 崩溃恢复——上次写入在「dst→bak」之后、「tmp→dst」完成前退出时主指针缺失、
        // 备份尚在；先把备份恢复为可回滚基线，否则下面会先删备份再写，指针在崩溃窗口中丢失。
        // 恢复失败则宁可不写（绝不删除唯一副本）。
        if (!dst.exists() && bak.exists() && !renameQuietly(bak, dst)) {
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
        if (dst.exists() && !dst.renameTo(bak)) {
            tmp.delete();
            return false;
        }
        if (!tmp.renameTo(dst)) {
            if (bak.exists()) {
                bak.renameTo(dst);
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
                    renameQuietly(bak, dst);
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
                renameQuietly(bak, dst);
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
        File marker = rollbackMarkerOf(profileId);

        if (marker.exists()) {
            if (!bak.exists()) {
                // 备份已不存在（无旧模型可还原）：标记失去意义，清除以免状态永久卡在 ERROR。
                //noinspection ResultOfMethodCallIgnored
                marker.delete();
            } else if (!target.exists() || deleteRecursively(target)) {
                // 新目录已清空，备份可还原；成功则连标记一并清除（回滚到此完成）。
                if (renameQuietly(bak, target)) {
                    //noinspection ResultOfMethodCallIgnored
                    marker.delete();
                }
            }
            // 至此 target 仍存在或备份仍未还原：保持现场，交由 buildStatus 如实报错，不做无根据的承诺。
            return;
        }

        // 崩溃窗口（无回滚标记）：旧目录是唯一完整副本，直接还原。
        if (!target.exists() && bak.exists()) {
            renameQuietly(bak, target);
        }
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
            if (rollbackMarkerOf(active).exists() && bakDirOf(active).exists()) {
                // 仅当「意图标记在**且**旧模型仍在备份目录」才是真正卡住的回滚：此时既不能报 ready
                //（会掩盖真实故障、让人以为新模型在跑），也不能承诺「已回滚」——如实报 ERROR 并指出
                // 原模型位置。若备份已被消费，说明还原其实已经完成（仅标记删除失败，A3' γ7），
                // 落入下方常规目录判定即可；否则会误报 ERROR 并谎称旧模型仍在备份里。
                base = FaceRuntimeStatus.error(active, count,
                        "上次模型回滚未完成：原模型仍滞留在 " + bakDirOf(active).getName() + "，当前配置未生效");
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
            deleteRecursively(f);
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
