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
 *   <li><b>原子启用</b>：把基目录 rename 到 {@code <storeDir>/<profileId>}（旧目录先 rename 为 .bak），
 *       再原子写激活指针 {@code active_profile}；指针写失败则回滚目录。全程同盘 rename，保证原子性。</li>
 *   <li><b>激活并生效</b>：目录稳定后调 {@link NativeRuntime#activateRuntime} 绑定实时管线；是否真正
 *       加载生效由 activateRuntime 返回的 {@code effective}/{@code reason}（并经 {@code getRuntimeStatus} 暴露）
 *       决定，导入状态本身不据此失败（模型文件与激活指针此时均已有效）。</li>
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

                // ⑦ 原子启用（rename 基目录 + 原子写激活指针）。
                enableProfile(base, tmpDir, profile.profileId);

                // ⑧ 激活实时管线：目录已稳定，用最终 profileId 目录绑定（临时目录随后被清理）。
                //    是否真正生效由本次 activateRuntime 的 effective/reason 决定，并经 status() 暴露。
                nativeActivateRuntime(manifestJson, profileDir(profile.profileId), currentAbi);

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
     * 切换当前 profile：对已导入目录重新做 manifest/文件/native 校验，成功后原子改写激活指针。
     * 任一步失败都保持原激活配置不变。
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
            if (!dir.isDirectory() || !manifestFile.isFile()) {
                return fail(Status.PROFILE_NOT_FOUND, "该配置尚未导入：" + profileId);
            }
            try {
                String manifestJson = readTextFile(manifestFile, limits.maxManifestBytes);
                FaceModelManifest.Profile profile = validateManifest(manifestJson, currentAbi);
                verifyModelFiles(dir, profile);
                nativeValidateProfile(manifestJson, currentAbi);
                if (!persistActive(profile.profileId)) {
                    // 指针未改动 -> 原配置保留。
                    return fail(Status.ATOMIC_RENAME_FAILED, "激活指针写入失败，已保留原配置");
                }
                // 切换成功后重绑实时管线到该 profile 的稳定目录（best-effort）；
                // 是否真正生效由 activateRuntime 的 effective/reason（经 status()）暴露。
                nativeActivateRuntime(manifestJson, dir, currentAbi);
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
     */
    @NonNull
    public FaceRuntimeStatus status() {
        synchronized (dirLock()) {
            return buildStatus(nativeStatusOrNull());
        }
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
     * 绑定实时管线（best-effort）：{@link #nativeValidateProfile} 已校验/暂存后，把 manifest 与<b>稳定的</b>
     * 模型基目录交给 native {@code activateRuntime}，使识别真正生效（否则 Android 端恒
     * {@code missing_model}，仅预览不识别）。
     *
     * <p>调用时机有意放在目录原子启用<b>之后</b>（用最终 profileId 目录而非临时目录）：临时目录
     * 导入成功后会被清理，若以临时目录绑定，运行时快照将指向已消失的路径。
     *
     * <p><b>唯一「加载生效」判据</b>是本次返回的 {@code effective}/{@code reason}（并存于 native
     * {@code getRuntimeStatus}，映射到 {@link FaceRuntimeStatus#nativeProfileActive}）。导入/切换状态本身
     * 不据此失败：模型文件与激活指针均已有效，Engine 尚未创建时会返回 {@code pending:true}，
     * 由 UI 依 {@code effective=false} 显示「已校验未生效」，绝不误报就绪。
     */
    private void nativeActivateRuntime(@NonNull String manifestJson, @NonNull File baseDir,
                                       @NonNull String currentAbi) {
        try {
            runtime.activateRuntime(manifestJson, baseDir.getAbsolutePath(), currentAbi, galleryDirPath());
        } catch (Throwable t) {
            // native 库缺失/不可用：保留导入结果，状态区显示 not-ready。
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
     * 把已验证的 {@code base} 目录 rename 为 {@code <storeDir>/<profileId>}，再原子写激活指针。
     * 指针写失败则回滚目录，保证「失败保留原配置」。
     */
    private void enableProfile(@NonNull File base, @NonNull File tmpDir, @NonNull String profileId)
            throws ImportFailure {
        File target = profileDir(profileId);
        File bak = new File(storeDir, ".bak_" + profileId);
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
        if (!persistActive(profileId)) {
            // 指针写失败：撤销本次目录替换，回到导入前状态。
            deleteRecursivelyQuietly(target);
            boolean restored = !hadOld || bak.renameTo(target);
            if (!restored) {
                throw new ImportFailure(Status.ATOMIC_RENAME_FAILED,
                        "激活指针写入失败，且旧配置回滚失败（旧目录滞留于 " + bak.getName() + "）");
            }
            throw new ImportFailure(Status.ATOMIC_RENAME_FAILED, "激活指针写入失败，已回滚配置");
        }
        deleteRecursivelyQuietly(bak);
    }

    /** 原子写激活指针：tmp + bak 替换（与注册表落盘同策略）。 */
    private boolean persistActive(@NonNull String profileId) {
        File dst = new File(storeDir, ACTIVE_POINTER_FILE);
        File tmp = new File(storeDir, ACTIVE_POINTER_FILE + ".tmp");
        File bak = new File(storeDir, ACTIVE_POINTER_FILE + ".bak");
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

    @Nullable
    private String readActive() {
        File f = new File(storeDir, ACTIVE_POINTER_FILE);
        if (!f.isFile()) {
            return null;
        }
        try {
            String id = readTextFile(f, 4096).trim();
            return FaceModelManifest.isSafeDirName(id) ? id : null;
        } catch (Exception ex) {
            return null;
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
            File dir = profileDir(active);
            File mf = new File(dir, MANIFEST_FILE_NAME);
            if (!dir.isDirectory() || !mf.isFile()) {
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

    private static void deleteRecursively(@NonNull File f) {
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) {
                    deleteRecursively(c);
                }
            }
        }
        // 删除失败（被占用等）不抛错：临时目录清理失败不影响导入结果判定。
        //noinspection ResultOfMethodCallIgnored
        f.delete();
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
