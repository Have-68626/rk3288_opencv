package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * FaceModelManifest —— manifest v1 的 Java 侧解析与安全校验（任务 14）。
 *
 * <p>本类是 C++ 契约 {@code rk_core::FaceRuntimeManifest::parseV1}（{@code src/cpp/include/FaceRuntimeProfile.h}）
 * 的 Java 镜像：字段名、缺省值、非法判定必须与之一致，否则会出现「Java 通过、native 拒绝」或反之。
 * native 侧 {@code NativeFaceBridge.loadProfile(manifestJson, abi)} 仍会<b>再解析一次</b>（权威判定），
 * 本类只负责在导入前尽早发现非法 manifest，避免把坏包写到磁盘后再回滚。
 *
 * <h2>与 native 的一致性要点</h2>
 * <ul>
 *   <li>{@code schemaVersion} 必须<b>精确等于 1</b>（1.5 之类的小数一律拒绝）；</li>
 *   <li>{@code supportedAbis} 非空且必须包含当前 ABI；</li>
 *   <li>detector/embedder 的 param/bin 路径必须为安全相对路径，sha256 允许为空（表示不校验）
 *       或为 64 位 hex；</li>
 *   <li>{@code detector.outputs} 若存在必须恰 9 个（score/bbox/kps × 3）；</li>
 *   <li>阈值须 {@code >0 且 <=1}；尺寸须为正整数。</li>
 * </ul>
 *
 * <h2>为什么自带 JSON 解析器</h2>
 * 项目 Android 单元测试在纯 JVM 下运行（仅 junit，无 Robolectric），{@code org.json} /
 * {@code android.util.JsonReader} 在单测中是不可用的 framework stub（调用即抛 not mocked）。
 * 且本类不得新增 Gradle 依赖，故沿用 {@link AndroidEnrollmentRepository}「无外部依赖」的
 * 手写解析风格，内置一个极小的 JSON 解析器 {@link Json}，仅覆盖 manifest 所需的
 * object/array/string/number/bool/null。
 *
 * <p>本类不写任何日志，也不打印模型内容；错误信息只含字段名与错误码语义。
 */
public final class FaceModelManifest {

    /** 唯一受支持的 schema 版本。 */
    public static final int SCHEMA_VERSION = 1;

    /** detector 约定输出个数（score/bbox/kps × 3 个尺度）。 */
    public static final int DETECTOR_OUTPUT_COUNT = 9;

    private FaceModelManifest() {
    }

    /** manifest 解析结果码。 */
    public enum Status {
        OK,
        /** 非法 JSON 文本。 */
        INVALID_JSON,
        /** 顶层非对象。 */
        NOT_OBJECT,
        /** schemaVersion 缺失或不为精确 1。 */
        UNSUPPORTED_SCHEMA,
        /** 缺少必填字段或类型不符。 */
        MISSING_FIELD,
        /** 当前 ABI 不在 supportedAbis 中。 */
        ABI_MISMATCH,
        /** 模型路径为绝对路径或含父目录段。 */
        UNSAFE_PATH,
        /** sha256 非 64 位 hex（空串表示不校验，合法）。 */
        BAD_SHA256,
        /** 阈值不在 (0,1] 或尺寸非正整数。 */
        BAD_VALUE
    }

    /** 单个模型文件条目：相对路径 + 期望 SHA-256（空表示不校验）。 */
    public static final class Entry {
        @NonNull public final String relativePath;
        @NonNull public final String sha256;

        Entry(@NonNull String relativePath, @NonNull String sha256) {
            this.relativePath = relativePath;
            this.sha256 = sha256;
        }
    }

    /** manifest v1 解析产物（与 C++ FaceRuntimeProfile 字段对应，仅保留 Java 侧需要的部分）。 */
    public static final class Profile {
        public final int schemaVersion;
        @NonNull public final String profileId;
        @NonNull public final String displayName;
        @NonNull public final List<String> supportedAbis;

        @NonNull public final Entry detectorParam;
        @NonNull public final Entry detectorBin;
        @NonNull public final String detectorInput;
        @NonNull public final List<String> detectorOutputs;
        public final int detectorInputW;
        public final int detectorInputH;
        @NonNull public final String detectorDecodeMode;
        public final int[] detectorAnchorsPerPos;
        @NonNull public final String detectorOutputLayout;

        @NonNull public final Entry embedderParam;
        @NonNull public final Entry embedderBin;
        @NonNull public final String embedderInput;
        @NonNull public final String embedderOutput;
        public final int embedderInputW;
        public final int embedderInputH;
        public final int embeddingDim;

        public final float detectionConfidence;
        public final float nmsIou;
        public final float identitySimilarity;

        @NonNull public final String modelVersion;
        @NonNull public final String licenseId;

        Profile(int schemaVersion, @NonNull String profileId, @NonNull String displayName,
                @NonNull List<String> supportedAbis,
                @NonNull Entry detectorParam, @NonNull Entry detectorBin,
                @NonNull String detectorInput, @NonNull List<String> detectorOutputs,
                int detectorInputW, int detectorInputH, @NonNull String detectorDecodeMode,
                int[] detectorAnchorsPerPos, @NonNull String detectorOutputLayout,
                @NonNull Entry embedderParam, @NonNull Entry embedderBin,
                @NonNull String embedderInput, @NonNull String embedderOutput,
                int embedderInputW, int embedderInputH, int embeddingDim,
                float detectionConfidence, float nmsIou, float identitySimilarity,
                @NonNull String modelVersion, @NonNull String licenseId) {
            this.schemaVersion = schemaVersion;
            this.profileId = profileId;
            this.displayName = displayName;
            this.supportedAbis = Collections.unmodifiableList(supportedAbis);
            this.detectorParam = detectorParam;
            this.detectorBin = detectorBin;
            this.detectorInput = detectorInput;
            this.detectorOutputs = Collections.unmodifiableList(detectorOutputs);
            this.detectorInputW = detectorInputW;
            this.detectorInputH = detectorInputH;
            this.detectorDecodeMode = detectorDecodeMode;
            this.detectorAnchorsPerPos = detectorAnchorsPerPos.clone();
            this.detectorOutputLayout = detectorOutputLayout;
            this.embedderParam = embedderParam;
            this.embedderBin = embedderBin;
            this.embedderInput = embedderInput;
            this.embedderOutput = embedderOutput;
            this.embedderInputW = embedderInputW;
            this.embedderInputH = embedderInputH;
            this.embeddingDim = embeddingDim;
            this.detectionConfidence = detectionConfidence;
            this.nmsIou = nmsIou;
            this.identitySimilarity = identitySimilarity;
            this.modelVersion = modelVersion;
            this.licenseId = licenseId;
        }

        /** 四个模型文件条目（detector/embedder 的 param/bin），供批量校验。 */
        @NonNull
        public List<Entry> allEntries() {
            List<Entry> out = new ArrayList<>(4);
            out.add(detectorParam);
            out.add(detectorBin);
            out.add(embedderParam);
            out.add(embedderBin);
            return out;
        }
    }

    /** 解析结果：成功时 {@link #profile} 非空。 */
    public static final class ParseResult {
        @NonNull public final Status status;
        @Nullable public final Profile profile;
        @NonNull public final String message;

        ParseResult(@NonNull Status status, @Nullable Profile profile, @NonNull String message) {
            this.status = status;
            this.profile = profile;
            this.message = message;
        }

        public boolean isOk() {
            return status == Status.OK && profile != null;
        }
    }

    /**
     * 解析 manifest v1 JSON 文本。
     *
     * @param jsonText   manifest JSON
     * @param currentAbi 当前设备 ABI；传 {@code null} 表示跳过 ABI 匹配（仅用于读取已落盘 config 展示，
     *                   导入路径必须传入真实 ABI）
     */
    @NonNull
    public static ParseResult parse(@NonNull String jsonText, @Nullable String currentAbi) {
        Object root;
        try {
            root = Json.parse(jsonText);
        } catch (Json.JsonException ex) {
            return new ParseResult(Status.INVALID_JSON, null, "manifest 不是合法 JSON");
        }
        Map<String, Object> obj = asObject(root);
        if (obj == null) {
            return new ParseResult(Status.NOT_OBJECT, null, "manifest 顶层非对象");
        }

        // schemaVersion 必须为数字且精确等于 1（拒绝 1.5 截断，A2 LOW-2）。
        Double sv = asNumber(obj.get("schemaVersion"));
        if (sv == null || sv.doubleValue() != (double) SCHEMA_VERSION) {
            return new ParseResult(Status.UNSUPPORTED_SCHEMA, null,
                    "未知 manifest schema 版本（仅支持 v1）");
        }

        String profileId = asString(obj.get("profileId"));
        String displayName = asString(obj.get("displayName"));
        if (isBlank(profileId) || isBlank(displayName)) {
            return new ParseResult(Status.MISSING_FIELD, null, "缺少 profileId 或 displayName");
        }
        // 导入校验：profileId 字符集。
        // ⚠️ 它**不止作用于新导入**——本方法（parse）同时被状态读取路径 FaceModelManager.buildStatus 调用，
        // 故「升级前导入、profileId 含内部控制字符」的存量 profile 会由 READY 变为「激活配置 manifest 无效」。
        // 这是有意为之（详见 isImportSafeProfileId 的 javadoc）。读路径的 isSafeDirName 仍有意保持宽松。
        // 之所以在此拦截：profileId 会作为目录名/行式 k=v 文件（active_profile / .txn_* / .commit_*）
        // 的值落盘，含换行/首尾空格会把读回值截断成另一个 id，导致「无记录 / 无提交证据」错判。
        if (!isImportSafeProfileId(profileId)) {
            return new ParseResult(Status.MISSING_FIELD, null,
                    "profileId 含非法字符（C0 控制字符/DEL 或首尾空格）");
        }

        List<Object> abisRaw = asArray(obj.get("supportedAbis"));
        if (abisRaw == null || abisRaw.isEmpty()) {
            return new ParseResult(Status.MISSING_FIELD, null, "缺少 supportedAbis");
        }
        List<String> abis = new ArrayList<>(abisRaw.size());
        boolean abiMatched = (currentAbi == null);
        for (Object a : abisRaw) {
            String s = asString(a);
            if (s == null) {
                return new ParseResult(Status.MISSING_FIELD, null, "supportedAbis 含非字符串");
            }
            abis.add(s);
            if (s.equals(currentAbi)) {
                abiMatched = true;
            }
        }
        if (!abiMatched) {
            return new ParseResult(Status.ABI_MISMATCH, null,
                    "ABI 不匹配（当前 " + currentAbi + " 不在 supportedAbis）");
        }

        Map<String, Object> det = asObject(obj.get("detector"));
        if (det == null) {
            return new ParseResult(Status.MISSING_FIELD, null, "缺少 detector 配置");
        }
        Entry detParam = parseFilePair(det, "param");
        Entry detBin = parseFilePair(det, "bin");
        if (detParam == null || detBin == null) {
            return new ParseResult(Status.MISSING_FIELD, null, "detector 配置缺失或非法");
        }
        // 检测器输入节点缺键回落值 = "input.1"（**方向**与 C++ FaceRuntimeProfile 一致）。
        // ⚠️ 精确措辞（2026-09-26 二次审计更正）：本行**不是**「与 C++ 逐字镜像」——
        //    C++ `getString` 在**空串**时也回落（`v->s.empty()` ⇒ 用默认值），而本行的 `strOr`
        //    **只在 null 时**回落 ⇒ manifest 写 `"input": ""` 时 C++ 得 input.1、Java 得 ""。
        //    该分歧属**已知边界**（空串是坏 manifest，两条路径都会在推理期失败），不在此处改行为。
        // 证据：**`models/` 下全部 10 个** SCRFD .param（含 int8 侧）的 Input 层名都是 input.1；
        // 勿推广到 `deps/insightface/det_10g.ncnn.param`——那份未转换的 ncnn 版实测是 in0。
        // "in0" 是 ArcFace 侧的名字，曾被人误抄。
        String detInput = strOr(asString(det.get("input")), "input.1");
        List<String> detOutputs = new ArrayList<>();
        Object detOutsRaw = det.get("outputs");
        if (detOutsRaw != null) {
            List<Object> outs = asArray(detOutsRaw);
            if (outs == null) {
                return new ParseResult(Status.MISSING_FIELD, null, "detector.outputs 非数组");
            }
            for (Object n : outs) {
                String s = asString(n);
                if (s == null) {
                    return new ParseResult(Status.MISSING_FIELD, null, "detector.outputs 含非字符串");
                }
                detOutputs.add(s);
            }
            if (detOutputs.size() != DETECTOR_OUTPUT_COUNT) {
                return new ParseResult(Status.MISSING_FIELD, null,
                        "detector.outputs 必须恰 " + DETECTOR_OUTPUT_COUNT + " 个（score/bbox/kps × 3）");
            }
        }
        Integer detW = positiveIntOrNull(det.get("inputWidth"));
        Integer detH = positiveIntOrNull(det.get("inputHeight"));
        int detInputW = (detW != null) ? detW : 640;
        int detInputH = (detH != null) ? detH : 640;

        String decodeMode = "delta";
        Object dm = det.get("decodeMode");
        if (dm != null) {
            String s = asString(dm);
            if (!"delta".equals(s) && !"distance".equals(s)) {
                return new ParseResult(Status.BAD_VALUE, null, "detector.decodeMode 非法（仅 delta/distance）");
            }
            decodeMode = s;
        }
        int[] anchors = new int[]{1, 1, 1};
        Object anchorsRaw = det.get("anchorsPerPos");
        if (anchorsRaw != null) {
            List<Object> arr = asArray(anchorsRaw);
            if (arr == null || arr.size() != 3) {
                return new ParseResult(Status.BAD_VALUE, null, "detector.anchorsPerPos 必须为 3 个正整数");
            }
            for (int i = 0; i < 3; i++) {
                Integer v = positiveIntOrNull(arr.get(i));
                if (v == null) {
                    return new ParseResult(Status.BAD_VALUE, null, "detector.anchorsPerPos 元素非法（须正整数）");
                }
                anchors[i] = v;
            }
        }
        String outputLayout = "auto";
        Object ol = det.get("outputLayout");
        if (ol != null) {
            String s = asString(ol);
            if (!"auto".equals(s) && !"nchw".equals(s) && !"flat".equals(s)) {
                return new ParseResult(Status.BAD_VALUE, null, "detector.outputLayout 非法（仅 auto/nchw/flat）");
            }
            outputLayout = s;
        }

        Map<String, Object> emb = asObject(obj.get("embedder"));
        if (emb == null) {
            return new ParseResult(Status.MISSING_FIELD, null, "缺少 embedder 配置");
        }
        Entry embParam = parseFilePair(emb, "param");
        Entry embBin = parseFilePair(emb, "bin");
        if (embParam == null || embBin == null) {
            return new ParseResult(Status.MISSING_FIELD, null, "embedder 配置缺失或非法");
        }
        String embInput = strOr(asString(emb.get("input")), "in0");
        String embOutput = strOr(asString(emb.get("output")), "out0");
        Integer embW = positiveIntOrNull(emb.get("inputWidth"));
        Integer embH = positiveIntOrNull(emb.get("inputHeight"));
        Integer embDim = positiveIntOrNull(emb.get("embeddingDim"));
        int embInputW = (embW != null) ? embW : 112;
        int embInputH = (embH != null) ? embH : 112;
        int embeddingDim = (embDim != null) ? embDim : 512;

        // 路径安全 + sha256 格式（与 C++ parseV1 一致，遍历四个条目）。
        List<Entry> all = new ArrayList<>(4);
        all.add(detParam);
        all.add(detBin);
        all.add(embParam);
        all.add(embBin);
        for (Entry e : all) {
            if (!isSafeRelativePath(e.relativePath)) {
                return new ParseResult(Status.UNSAFE_PATH, null,
                        "非法模型路径（绝对或含 ..）：" + e.relativePath);
            }
            if (!e.sha256.isEmpty() && !isSha256Hex(e.sha256)) {
                return new ParseResult(Status.BAD_SHA256, null,
                        "sha256 非 64 位 hex：" + e.relativePath);
            }
        }

        // 阈值：缺失用默认；存在则必须 (0,1]。
        Float dConf = thresholdOrNull(obj.get("detectionConfidence"));
        Float nms = thresholdOrNull(obj.get("nmsIou"));
        Float sim = thresholdOrNull(obj.get("identitySimilarity"));
        if (dConf == null && obj.containsKey("detectionConfidence")) {
            return new ParseResult(Status.BAD_VALUE, null, "detectionConfidence 非法（须 >0 且 <=1）");
        }
        if (nms == null && obj.containsKey("nmsIou")) {
            return new ParseResult(Status.BAD_VALUE, null, "nmsIou 非法（须 >0 且 <=1）");
        }
        if (sim == null && obj.containsKey("identitySimilarity")) {
            return new ParseResult(Status.BAD_VALUE, null, "identitySimilarity 非法（须 >0 且 <=1）");
        }

        String modelVersion = strOr(asString(obj.get("modelVersion")), "1");
        String licenseId = strOr(asString(obj.get("licenseId")), "");

        Profile profile = new Profile(SCHEMA_VERSION, profileId, displayName, abis,
                detParam, detBin, detInput, detOutputs, detInputW, detInputH,
                decodeMode, anchors, outputLayout,
                embParam, embBin, embInput, embOutput, embInputW, embInputH, embeddingDim,
                dConf != null ? dConf : 0.55f,
                nms != null ? nms : 0.40f,
                sim != null ? sim : 0.45f,
                modelVersion, licenseId);
        return new ParseResult(Status.OK, profile, "");
    }

    /**
     * 相对路径安全校验（与 C++ {@code isSafeRelativePath} 等价）：
     * 拒绝空串、绝对路径（以 / 或 \ 开头）、Windows 盘符、以及任何 {@code ..} 路径段。
     * 同时拒绝 NUL 字符。
     */
    public static boolean isSafeRelativePath(@NonNull String p) {
        if (p.isEmpty() || p.indexOf('\0') >= 0) {
            return false;
        }
        if (p.charAt(0) == '/' || p.charAt(0) == '\\') {
            return false;
        }
        if (p.length() >= 2 && Character.isLetter(p.charAt(0)) && p.charAt(1) == ':') {
            return false;
        }
        StringBuilder seg = new StringBuilder();
        for (int i = 0; i < p.length(); i++) {
            char ch = p.charAt(i);
            if (ch == '/' || ch == '\\') {
                if (seg.toString().equals("..")) {
                    return false;
                }
                seg.setLength(0);
            } else {
                seg.append(ch);
            }
        }
        return !seg.toString().equals("..");
    }

    /** 是否为恰 64 位十六进制（大小写不敏感）。 */
    public static boolean isSha256Hex(@NonNull String s) {
        if (s.length() != 64) {
            return false;
        }
        for (int i = 0; i < s.length(); i++) {
            if (Character.digit(s.charAt(i), 16) < 0) {
                return false;
            }
        }
        return true;
    }

    /** 目录名安全校验：非空、不含分隔符/父目录段，且不是 . / ..（profileId 会作为目录名）。 */
    public static boolean isSafeDirName(@Nullable String name) {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) {
            return false;
        }
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (c == '/' || c == '\\' || c == '\0' || c == ':') {
                return false;
            }
        }
        return true;
    }

    /**
     * profileId 的字符集判据（**新导入把关 + 存量暴露**）。
     *
     * <p>为何与 {@link #isSafeDirName} 分开、而不直接收紧它：{@code isSafeDirName} 与**读路径**共享
     * （{@code FaceModelManager.parseActivePointer}、{@code listProfiles}），收紧它会让磁盘上
     * **已存在**的含控制字符目录从「按原名显式报错」退化成「静默未导入」——那是有意保留的行为。
     *
     * <p>⚠️ **但它并非只作用于新导入**：它接在 {@link #parse} 里，而 {@code parse} 同时被状态读取路径
     * 使用（{@code FaceModelManager.buildStatus}）。因此**升级前**用旧代码导入、profileId 含**内部**
     * 控制字符（如 {@code "a\tb"}）的 profile，升级后会由 READY 变为「激活配置 manifest 无效」。
     * 这是**有意**的：此类 profileId 的 {@code .txn_*} / {@code .commit_*} 本来就无法原样往返
     * （含 {@code \n} 时被截断读回），如实报错比报 READY 诚实。（首尾空白那类在旧代码下本就读不回来，
     * 不受影响。）
     *
     * <p>拒绝当且仅当满足任一条：任一字符 {@code c} 满足 {@code c < 0x20}（C0 控制字符，含
     * {@code \n \r \t}）或 {@code c == 0x7F}；或首字符/末字符为空格(0x20)。放行 {@code =}、内部空格、
     * {@code .}、{@code *}、{@code ?}、非 ASCII 等一切其它字符。**不设长度上限**（交由文件系统报错）。
     *
     * <p>规则与 C++ 侧 {@code FaceRuntimeProfile.cpp} 的 {@code isValidProfileId} 逐字一致。
     */
    public static boolean isImportSafeProfileId(@NonNull String id) {
        if (id.isEmpty()) {
            return false;
        }
        if (id.charAt(0) == ' ' || id.charAt(id.length() - 1) == ' ') {
            return false;
        }
        for (int i = 0; i < id.length(); i++) {
            char c = id.charAt(i);
            if (c < 0x20 || c == 0x7F) {
                return false;
            }
        }
        return true;
    }

    // ---------------- 字段辅助 ----------------

    @Nullable
    private static Entry parseFilePair(@NonNull Map<String, Object> parent, @NonNull String key) {
        Map<String, Object> node = asObject(parent.get(key));
        if (node == null) {
            return null;
        }
        String path = asString(node.get("path"));
        if (isBlank(path)) {
            return null;
        }
        String sha = asString(node.get("sha256"));
        return new Entry(path, sha == null ? "" : sha.trim());
    }

    @Nullable
    private static Float thresholdOrNull(@Nullable Object v) {
        Double d = asNumber(v);
        if (d == null) {
            return null;
        }
        double x = d.doubleValue();
        if (!(x > 0.0) || x > 1.0) {
            return null;
        }
        return (float) x;
    }

    @Nullable
    private static Integer positiveIntOrNull(@Nullable Object v) {
        Double d = asNumber(v);
        if (d == null) {
            return null;
        }
        double x = d.doubleValue();
        if (!(x > 0.0) || x != Math.floor(x) || x > Integer.MAX_VALUE) {
            return null;
        }
        return (int) x;
    }

    @NonNull
    private static String strOr(@Nullable String v, @NonNull String dflt) {
        return (v == null) ? dflt : v;
    }

    private static boolean isBlank(@Nullable String s) {
        return s == null || s.trim().isEmpty();
    }

    // ---------------- 供 FaceModelManager / FaceRuntimeStatus 复用的最小 JSON 访问 ----------------

    @Nullable
    static Map<String, Object> asObject(@Nullable Object o) {
        if (o instanceof Map) {
            @SuppressWarnings("unchecked")
            Map<String, Object> m = (Map<String, Object>) o;
            return m;
        }
        return null;
    }

    @Nullable
    static List<Object> asArray(@Nullable Object o) {
        if (o instanceof List) {
            @SuppressWarnings("unchecked")
            List<Object> l = (List<Object>) o;
            return l;
        }
        return null;
    }

    @Nullable
    static String asString(@Nullable Object o) {
        return (o instanceof String) ? (String) o : null;
    }

    @Nullable
    static Double asNumber(@Nullable Object o) {
        return (o instanceof Double) ? (Double) o : null;
    }

    @Nullable
    static Boolean asBoolean(@Nullable Object o) {
        return (o instanceof Boolean) ? (Boolean) o : null;
    }

    /** 解析一段 JSON 文本为通用结构；失败抛 {@link Json.JsonException}。 */
    static Object parseJsonValue(@NonNull String text) throws Json.JsonException {
        return Json.parse(text);
    }

    /**
     * 极小 JSON 解析器（object/array/string/number/bool/null）。
     *
     * <p>只服务 manifest 与 native 返回的短 JSON；不追求 RFC 全集，但对转义、数字、嵌套
     * 均按标准处理。解析失败一律抛出 {@link JsonException}，由调用方转成受控失败。
     */
    static final class Json {

        static final class JsonException extends Exception {
            JsonException(String message) {
                super(message);
            }
        }

        private final String src;
        private int pos;
        private int depth;

        /** JSON 嵌套深度上限：防止 1MiB 级深嵌套（如 {@code [[[[…}}）触发递归栈溢出。 */
        static final int MAX_DEPTH = 64;

        private Json(@NonNull String src) {
            this.src = src;
        }

        @NonNull
        static Object parse(@NonNull String text) throws JsonException {
            Json p = new Json(text);
            p.skipWs();
            Object v = p.readValue();
            p.skipWs();
            if (p.pos != p.src.length()) {
                throw new JsonException("JSON 尾部存在多余字符");
            }
            return v;
        }

        private void skipWs() {
            while (pos < src.length()) {
                char c = src.charAt(pos);
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    pos++;
                } else {
                    break;
                }
            }
        }

        private Object readValue() throws JsonException {
            if (pos >= src.length()) {
                throw new JsonException("JSON 意外结束");
            }
            char c = src.charAt(pos);
            switch (c) {
                case '{':
                    return readContainer(true);
                case '[':
                    return readContainer(false);
                case '"':
                    return readString();
                case 't':
                    expectLiteral("true");
                    return Boolean.TRUE;
                case 'f':
                    expectLiteral("false");
                    return Boolean.FALSE;
                case 'n':
                    expectLiteral("null");
                    return null;
                default:
                    return readNumber();
            }
        }

        private void expectLiteral(@NonNull String lit) throws JsonException {
            if (!src.startsWith(lit, pos)) {
                throw new JsonException("非法字面量");
            }
            pos += lit.length();
        }

        /**
         * 容器入口统一做嵌套深度限制：超限抛受控 {@link JsonException}，绝不递归到
         * 触发 {@link StackOverflowError}（后者会穿透导入线程的常规 catch）。
         */
        private Object readContainer(boolean object) throws JsonException {
            if (++depth > MAX_DEPTH) {
                throw new JsonException("JSON 嵌套深度超过上限 " + MAX_DEPTH);
            }
            try {
                return object ? readObject() : readArray();
            } finally {
                depth--;
            }
        }

        private Map<String, Object> readObject() throws JsonException {
            Map<String, Object> out = new LinkedHashMap<>();
            pos++;  // '{'
            skipWs();
            if (peek() == '}') {
                pos++;
                return out;
            }
            while (true) {
                skipWs();
                if (peek() != '"') {
                    throw new JsonException("对象键必须为字符串");
                }
                String key = readString();
                skipWs();
                if (peek() != ':') {
                    throw new JsonException("对象缺少冒号");
                }
                pos++;
                skipWs();
                out.put(key, readValue());
                skipWs();
                char c = peek();
                if (c == ',') {
                    pos++;
                    continue;
                }
                if (c == '}') {
                    pos++;
                    return out;
                }
                throw new JsonException("对象分隔符非法");
            }
        }

        private List<Object> readArray() throws JsonException {
            List<Object> out = new ArrayList<>();
            pos++;  // '['
            skipWs();
            if (peek() == ']') {
                pos++;
                return out;
            }
            while (true) {
                skipWs();
                out.add(readValue());
                skipWs();
                char c = peek();
                if (c == ',') {
                    pos++;
                    continue;
                }
                if (c == ']') {
                    pos++;
                    return out;
                }
                throw new JsonException("数组分隔符非法");
            }
        }

        private String readString() throws JsonException {
            pos++;  // '"'
            StringBuilder sb = new StringBuilder();
            while (true) {
                if (pos >= src.length()) {
                    throw new JsonException("字符串未闭合");
                }
                char c = src.charAt(pos++);
                if (c == '"') {
                    return sb.toString();
                }
                if (c == '\\') {
                    if (pos >= src.length()) {
                        throw new JsonException("转义未结束");
                    }
                    char e = src.charAt(pos++);
                    switch (e) {
                        case '"': sb.append('"'); break;
                        case '\\': sb.append('\\'); break;
                        case '/': sb.append('/'); break;
                        case 'b': sb.append('\b'); break;
                        case 'f': sb.append('\f'); break;
                        case 'n': sb.append('\n'); break;
                        case 'r': sb.append('\r'); break;
                        case 't': sb.append('\t'); break;
                        case 'u':
                            if (pos + 4 > src.length()) {
                                throw new JsonException("\\u 转义长度不足");
                            }
                            int cp = 0;
                            for (int i = 0; i < 4; i++) {
                                int d = Character.digit(src.charAt(pos++), 16);
                                if (d < 0) {
                                    throw new JsonException("\\u 转义非 hex");
                                }
                                cp = (cp << 4) | d;
                            }
                            sb.append((char) cp);
                            break;
                        default:
                            throw new JsonException("未知转义字符");
                    }
                } else {
                    sb.append(c);
                }
            }
        }

        private Double readNumber() throws JsonException {
            int start = pos;
            if (peek() == '-') {
                pos++;
            }
            while (pos < src.length() && Character.isDigit(src.charAt(pos))) {
                pos++;
            }
            if (pos < src.length() && src.charAt(pos) == '.') {
                pos++;
                while (pos < src.length() && Character.isDigit(src.charAt(pos))) {
                    pos++;
                }
            }
            if (pos < src.length() && (src.charAt(pos) == 'e' || src.charAt(pos) == 'E')) {
                pos++;
                if (pos < src.length() && (src.charAt(pos) == '+' || src.charAt(pos) == '-')) {
                    pos++;
                }
                while (pos < src.length() && Character.isDigit(src.charAt(pos))) {
                    pos++;
                }
            }
            if (pos == start) {
                throw new JsonException("非法数字");
            }
            try {
                return Double.valueOf(src.substring(start, pos));
            } catch (NumberFormatException ex) {
                throw new JsonException("数字解析失败");
            }
        }

        private char peek() throws JsonException {
            if (pos >= src.length()) {
                throw new JsonException("JSON 意外结束");
            }
            return src.charAt(pos);
        }
    }
}
