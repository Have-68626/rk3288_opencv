package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * 任务13：NativeFaceBridge JNI 契约与边界守卫（RED 判别）测试。
 *
 * <p>NATIVE 方法由 JniMethodRegistry 动态注册（registry.cpp → NativeFaceBridge），
 * 无法在纯 JVM 单测中加载 so 调用。因此本测试在源码层做等价核对：
 * <ol>
 *   <li>{@link #javaNativeDeclarationsMatchExpectedSignatures()} —— Java {@code native} 声明的
 *       （名 + JNI 描述符）必须与 EXPECTED 表一致（EXPECTED 为唯一事实源）；</li>
 *   <li>{@link #cppRegistryMatchesJavaDeclarationsExactly()} —— C++ {@code registerFaceBridgeMethods}
 *       注册表必须与上述 Java 声明逐条一致；</li>
 *   <li>{@link #registryClassMappingWiresNativeFaceBridge()} —— registry.cpp 必须把该注册表绑定到
 *       NativeFaceBridge 类名；</li>
 *   <li>{@link #enrollmentFrameCallbackDescriptorMatchesCppStaticMethodLookup()} —— 非 {@code native}
 *       的 JNI <b>回调</b> 描述符（onEnrollmentFrame）Java 声明 ↔ C++ {@code GetStaticMethodID} 一致。
 *       回调是 Java 侧普通 {@code private static} 方法，<b>结构上不可能是 native 声明</b>，故上表的
 *       EXPECTED 收不了它，与注册表比对也够不着 —— 必须单独核对；</li>
 *   <li>{@link #boundaryGuardsAndDirectBufferChecksPresent()} —— 边界常量（dim=512、gallery 上限 1000）
 *       与 direct ByteBuffer 守卫存在；</li>
 *   <li>{@link #logCallsNeverReferenceTemplateOrEmbeddingIdentifiers()} —— 安全红线：任何 rklog
 *       调用行都不得引用模板/embedding 向量标识符；</li>
 *   <li>{@link #loadProfileSemanticsAreValidateAndStageNotEffective()} —— loadProfile 语义必须是
 *       「校验并暂存」而非「已生效」；</li>
 *   <li>{@link #jniSourceIsWiredIntoCmakeAndroidBuild()} —— 新 JNI 源已接入 cmake/android.cmake
 *       构建源列表。</li>
 * </ol>
 * 真实的 C++ 边界行为（非 direct/长度不符/超限返回失败）依赖 JNIEnv + Android NDK，
 * 无法在 core_unit_tests（cmake/core.cmake 未收录 jni/*.cpp 且禁止改动）中实例化，
 * 故由本结构测试 + 运行时 JSON 错误码兜底。
 */
public class NativeFaceBridgeJniContractTest {

    /**
     * 期望的 Java native 方法名 → JNI 描述符（唯一事实源）。
     *
     * <p>只覆盖 <b>{@code native}</b> 声明，用于与 C++ {@code registerFaceBridgeMethods} 的注册表逐条比对。
     * JNI 侧另有不走动态注册的<b>回调</b>方法（Java 侧是普通 {@code private static}），
     * 本表结构上收不了它们 —— 那部分由
     * {@link #enrollmentFrameCallbackDescriptorMatchesCppStaticMethodLookup()} 单独做源码↔源码核对。
     */
    private static final Map<String, String> EXPECTED = new LinkedHashMap<>();

    static {
        EXPECTED.put("loadProfile",
                "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        EXPECTED.put("activateRuntime",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        EXPECTED.put("replaceGallery",
                "([Ljava/lang/String;[Ljava/nio/ByteBuffer;)Ljava/lang/String;");
        EXPECTED.put("beginEnrollment",
                "(Ljava/lang/String;Ljava/nio/ByteBuffer;)Ljava/lang/String;");
        EXPECTED.put("cancelEnrollment", "()Ljava/lang/String;");
        EXPECTED.put("getEnrollmentStatus", "()Ljava/lang/String;");
        EXPECTED.put("takeEnrollmentTemplate", "()[B");
        EXPECTED.put("getRuntimeStatus", "()Ljava/lang/String;");
        // #1 注册采集逐帧 API（打通注册闭环）
        EXPECTED.put("startEnrollmentCapture", "(Ljava/nio/ByteBuffer;)Ljava/lang/String;");
        EXPECTED.put("stopEnrollmentCapture", "()Ljava/lang/String;");
    }

    private static final Map<String, String> PRIMS = new HashMap<>();
    static {
        PRIMS.put("void", "V");
        PRIMS.put("boolean", "Z");
        PRIMS.put("byte", "B");
        PRIMS.put("char", "C");
        PRIMS.put("short", "S");
        PRIMS.put("int", "I");
        PRIMS.put("long", "J");
        PRIMS.put("float", "F");
        PRIMS.put("double", "D");
    }

    private static final Pattern NATIVE_DECL = Pattern.compile(
            "\\bnative\\s+([\\w<>.$\\[\\] ]+?)\\s+([A-Za-z_$][\\w$]*)\\s*\\(([^)]*)\\)\\s*;",
            Pattern.DOTALL);
    private static final Pattern IMPORT_DECL = Pattern.compile(
            "^\\s*import\\s+([A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)+)\\s*;",
            Pattern.MULTILINE);
    private static final Pattern CPP_ENTRY = Pattern.compile(
            "\\{\\s*\"([A-Za-z_$][\\w$]*)\"\\s*,\\s*\"([^\"]*)\"\\s*,");

    /**
     * 本测试唯一额外核对的 JNI <b>回调</b> 方法名（Java 侧为 {@code private static}，非 {@code native}）。
     * 由 C++ {@code bridge_startEnrollmentCapture} 经 {@code GetStaticMethodID} 缓存并回调。
     */
    private static final String CALLBACK_METHOD = "onEnrollmentFrame";

    /**
     * C++ {@code GetStaticMethodID(接收者, "方法名", "JNI 描述符")} 三元组。
     *
     * <p>刻意允许调用折行：本仓该调用被写成两行（{@code GetStaticMethodID(} 换行后
     * {@code localCls, "名", "描述符"}），故各处分隔用 {@code \s*} 通配（含换行），
     * 单行正则会在换行处失配。<b>刻意不含任何行号</b> —— 本仓行号大面积漂移，行号定位必然腐化。
     */
    private static final Pattern CPP_GET_STATIC_METHOD_ID = Pattern.compile(
            "GetStaticMethodID\\s*\\(\\s*([A-Za-z_$][\\w$]*)\\s*,\\s*\"([^\"]+)\"\\s*,"
                    + "\\s*\"([^\"]+)\"\\s*\\)");

    // ---------- 辅助 ----------

    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, "src/cpp/jni/native_face_bridge.cpp").isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录（未找到 src/cpp/jni/native_face_bridge.cpp）");
    }

    private static String read(File f) throws IOException {
        assertTrue("源文件不存在: " + f, f.isFile());
        return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
    }

    private static String stripComments(String src) {
        src = src.replaceAll("/\\*[\\s\\S]*?\\*/", " ");
        src = src.replaceAll("//[^\\r\\n]*", "");
        return src;
    }

    private static Map<String, String> parseImports(String src) {
        Map<String, String> imports = new HashMap<>();
        Matcher m = IMPORT_DECL.matcher(src);
        while (m.find()) {
            String fqn = m.group(1);
            imports.put(fqn.substring(fqn.lastIndexOf('.') + 1), fqn);
        }
        return imports;
    }

    private static String parsePackage(String src) {
        Matcher m = Pattern.compile("^\\s*package\\s+([\\w.]+)\\s*;", Pattern.MULTILINE).matcher(src);
        return m.find() ? m.group(1) : "";
    }

    /**
     * Java 类型 → JNI 描述符片段：基本类型/void 查 {@link #PRIMS}，{@code String} 特判为
     * {@code Ljava/lang/String;}，其余按 import（缺省同包）解析成 {@code L…;}；
     * 尾部每层 {@code []} 转为前导一个 {@code [}。因此 {@code ByteBuffer} → {@code Ljava/nio/ByteBuffer;}、
     * {@code float[]} → {@code [F}、{@code void} → {@code V}。
     */
    private static String typeToDesc(String type, Map<String, String> imports, String pkg) {
        String t = type.replaceAll("\\s+", "");
        int arrays = 0;
        while (t.endsWith("[]")) {
            arrays++;
            t = t.substring(0, t.length() - 2);
        }
        String base;
        if (PRIMS.containsKey(t)) {
            base = PRIMS.get(t);
        } else if (t.equals("String")) {
            base = "Ljava/lang/String;";
        } else {
            String fqn = imports.containsKey(t) ? imports.get(t) : pkg + "." + t;
            base = "L" + fqn.replace('.', '/') + ";";
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < arrays; i++) {
            sb.append('[');
        }
        return sb.append(base).toString();
    }

    private static List<String> paramTypes(String params) {
        if (params.trim().isEmpty()) {
            return Collections.emptyList();
        }
        List<String> out = new ArrayList<>();
        for (String p : params.split(",")) {
            String t = p.trim();
            if (!t.isEmpty()) {
                out.add(t.replaceAll("\\s+[A-Za-z_$][\\w$]*$", "").trim());
            }
        }
        return out;
    }

    private static Map<String, String> javaNativeDescriptors(String src) {
        String clean = stripComments(src);
        String pkg = parsePackage(clean);
        Map<String, String> imports = parseImports(clean);
        Map<String, String> out = new TreeMap<>();
        Matcher m = NATIVE_DECL.matcher(clean);
        while (m.find()) {
            List<String> types = paramTypes(m.group(3));
            StringBuilder desc = new StringBuilder("(");
            for (String t : types) {
                desc.append(typeToDesc(t, imports, pkg));
            }
            desc.append(')').append(typeToDesc(m.group(1).trim(), imports, pkg));
            out.put(m.group(2), desc.toString());
        }
        return out;
    }

    private static Map<String, String> cppRegistryEntries(String src) {
        Map<String, String> out = new TreeMap<>();
        Matcher m = CPP_ENTRY.matcher(stripComments(src));
        while (m.find()) {
            out.put(m.group(1), m.group(2).trim());
        }
        return out;
    }

    /**
     * 抽取 Java 源中 {@code private static} 方法（含 {@code final} 修饰）的 JNI 描述符，按名筛选。
     *
     * <p>复用与 {@link #javaNativeDescriptors(String)} 完全相同的类型映射
     * （{@link #paramTypes} + {@link #typeToDesc}），保证「native 声明」与「非 native 回调」
     * 两条路径不会因映射口径不同而产生假阴性/假阳性。
     *
     * @return 匹配到的声明描述符列表；调用方须断言 size()==1，否则抽取器失配/歧义会被静默吞掉
     */
    private static List<String> javaPrivateStaticMethodDescriptors(String src, String methodName) {
        String clean = stripComments(src);
        String pkg = parsePackage(clean);
        Map<String, String> imports = parseImports(clean);
        Pattern p = Pattern.compile(
                "\\bprivate\\s+static\\s+(?:final\\s+)?([\\w<>.$\\[\\] ]+?)\\s+"
                        + Pattern.quote(methodName) + "\\s*\\(([^)]*)\\)");
        List<String> out = new ArrayList<>();
        Matcher m = p.matcher(clean);
        while (m.find()) {
            StringBuilder desc = new StringBuilder("(");
            for (String t : paramTypes(m.group(2))) {
                desc.append(typeToDesc(t, imports, pkg));
            }
            desc.append(')').append(typeToDesc(m.group(1).trim(), imports, pkg));
            out.add(desc.toString());
        }
        return out;
    }

    /**
     * 抽取 C++ 源中全部 {@code GetStaticMethodID(接收者, "方法名", "JNI 描述符")} 调用点。
     *
     * <p>先 {@link #stripComments(String)} 再匹配：被注释掉的调用点不算数，
     * 否则「注释里留一份旧描述符」会污染计数。
     *
     * @return 每个元素为 {@code [接收者变量, 方法名, 描述符]}；调用方须先校验
     *         「{@code GetStaticMethodID} 字面量出现次数 == 本列表长度」，以防正则失配静默漏采
     */
    private static List<String[]> cppStaticMethodLookups(String src) {
        List<String[]> out = new ArrayList<>();
        Matcher m = CPP_GET_STATIC_METHOD_ID.matcher(stripComments(src));
        while (m.find()) {
            out.add(new String[]{m.group(1), m.group(2), m.group(3)});
        }
        return out;
    }

    /** 子串出现次数（不重叠），用于把「正则漏采」暴露成计数不等而非静默通过。 */
    private static int countOccurrences(String haystack, String needle) {
        int n = 0;
        int idx = haystack.indexOf(needle);
        while (idx >= 0) {
            n++;
            idx = haystack.indexOf(needle, idx + needle.length());
        }
        return n;
    }

    /**
     * 抽取 C++ 源中以 {@code anchor} 标识的**函数体**（最外层花括号内的文本；花括号配平，
     * 并跳过字符串/字符字面量）。
     *
     * <p><b>为什么需要它</b>：本仓多处断言用「全文件 {@code contains}」找符号或字面量 ——
     * 只要文件里**任意一处**出现即通过，<b>证明不了</b>断言想钉的那个函数真的写了它。
     * 实例：{@code \"effective\":} 在 {@code native_face_bridge.cpp} 里有<b>多处</b>生产写入，
     * 其中只有一处在 {@code bridge_getRuntimeStatus} 内。限定到函数体后，「在别处写一份」
     * 不再能让断言通过。
     *
     * <p>同本类下方 getRuntimeStatus 段（{@code loadProfileSemanticsAreValidateAndStageNotEffective}）
     * 的口径：此处<b>刻意不写具体处数</b>——桥接文件由其它任务并行修改、数目会漂移；而「多处」这一
     * 量级判断、以及「判据限定在目标函数体内」本身，都不随并行修改漂移。
     *
     * <p>定位方式**刻意按锚点字符串而非行号**（本仓行号大面积漂移），且调用方应同时断言锚点
     * 在源中恰好出现一次 —— 否则锚点失配会静默抽取到**别的**函数体（比抽取失败更危险）。
     *
     * @param strippedSrc 已 {@link #stripComments(String)} 的源码（未去注释时，注释里的同名
     *                    签名会被误当锚点）
     * @return 函数体文本（不含最外层花括号）；锚点缺失、锚点后无 {@code &#123;}、或配平失败返回 {@code null}
     */
    private static String cppFunctionBody(String strippedSrc, String anchor) {
        int at = strippedSrc.indexOf(anchor);
        if (at < 0) {
            return null;
        }
        int open = strippedSrc.indexOf('{', at);
        if (open < 0) {
            return null;
        }
        int depth = 0;
        boolean inStr = false;
        boolean inChr = false;
        boolean esc = false;
        for (int i = open; i < strippedSrc.length(); i++) {
            char c = strippedSrc.charAt(i);
            if (esc) {
                esc = false;
                continue;
            }
            if (inStr || inChr) {
                if (c == '\\') {
                    esc = true;
                } else if (inStr && c == '"') {
                    inStr = false;
                } else if (inChr && c == '\'') {
                    inChr = false;
                }
                continue;
            }
            if (c == '"') {
                inStr = true;
            } else if (c == '\'') {
                inChr = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    return strippedSrc.substring(open + 1, i);
                }
            }
        }
        return null;
    }

    // ---------- 测试 ----------

    @Test
    public void javaNativeDeclarationsMatchExpectedSignatures() throws IOException {
        File root = findRepoRoot();
        String javaSrc = read(new File(root,
                "src/java/com/example/rk3288_opencv/NativeFaceBridge.java"));

        Map<String, String> actual = javaNativeDescriptors(javaSrc);
        assertEquals("NativeFaceBridge native 方法集合与签名必须与契约一致",
                new TreeMap<>(EXPECTED), actual);
    }

    @Test
    public void cppRegistryMatchesJavaDeclarationsExactly() throws IOException {
        File root = findRepoRoot();
        String javaSrc = read(new File(root,
                "src/java/com/example/rk3288_opencv/NativeFaceBridge.java"));
        String cppSrc = read(new File(root, "src/cpp/jni/native_face_bridge.cpp"));

        Map<String, String> javaDesc = javaNativeDescriptors(javaSrc);
        Map<String, String> cppDesc = cppRegistryEntries(cppSrc);

        assertEquals("C++ registerFaceBridgeMethods 注册表必须与 Java 声明逐条一致",
                javaDesc, cppDesc);

        // 注册函数名必须为 register<Domain>Methods(JniMethodRegistry&)，供 jni-sync-audit 归属类。
        assertTrue("缺少 registerFaceBridgeMethods(JniMethodRegistry&) 定义",
                stripComments(cppSrc).matches("(?s).*void\\s+registerFaceBridgeMethods\\s*\\(\\s*JniMethodRegistry\\s*&\\s*\\w+\\s*\\).*"));
    }

    @Test
    public void registryClassMappingWiresNativeFaceBridge() throws IOException {
        File root = findRepoRoot();
        String registry = read(new File(root, "src/cpp/jni/registry.cpp"));
        assertTrue("registry.cpp 必须 FindClass NativeFaceBridge",
                registry.contains("com/example/rk3288_opencv/NativeFaceBridge"));
        assertTrue("registry.cpp 必须调用 registerFaceBridgeMethods(...)",
                Pattern.compile("registerFaceBridgeMethods\\s*\\(\\s*\\w+\\s*\\)\\s*;")
                        .matcher(stripComments(registry)).find());
    }

    @Test
    public void boundaryGuardsAndDirectBufferChecksPresent() throws IOException {
        File root = findRepoRoot();
        String cppSrc = read(new File(root, "src/cpp/jni/native_face_bridge.cpp"));

        assertTrue("dim 常量必须为 512", cppSrc.matches("(?s).*kEmbeddingDim\\s*=\\s*512\\s*;.*"));
        assertTrue("gallery 上限必须为 1000", cppSrc.matches("(?s).*kGalleryCapacity\\s*=\\s*1000\\s*;.*"));
        assertTrue("必须用 GetDirectBufferAddress 校验 direct",
                cppSrc.contains("GetDirectBufferAddress"));
        assertTrue("必须用 GetDirectBufferCapacity 校验长度",
                cppSrc.contains("GetDirectBufferCapacity"));
        assertTrue("embedding 字节数必须为 512*sizeof(float)",
                cppSrc.contains("kEmbeddingDim * sizeof(float)"));

        // 数量一致性：personIds 数 == embedding 数。
        assertTrue("必须校验 personIds 与 embeddings 数量一致",
                cppSrc.contains("FACE_GALLERY_COUNT_MISMATCH"));
        // 超限失败。
        assertTrue("必须校验 gallery 超限", cppSrc.contains("FACE_GALLERY_TOO_LARGE"));
        // 非 direct / 长度不符失败码。
        assertTrue("必须拒绝非 direct buffer", cppSrc.contains("FACE_EMB_NOT_DIRECT"));
        assertTrue("必须拒绝长度不符", cppSrc.contains("FACE_EMB_BAD_LENGTH"));
    }

    @Test
    public void logCallsNeverReferenceTemplateOrEmbeddingIdentifiers() throws IOException {
        File root = findRepoRoot();
        String cppSrc = read(new File(root, "src/cpp/jni/native_face_bridge.cpp"));

        List<String> forbidden = Arrays.asList(
                "embedding", "enrollEmbedding", ".data(", "bytes", "vec", "payload");
        List<String> violations = new ArrayList<>();
        int idx = 0;
        for (String line : cppSrc.split("\\r?\\n", -1)) {
            idx++;
            if (!line.contains("rklog::log")) {
                continue;
            }
            String lower = line.toLowerCase();
            for (String bad : forbidden) {
                if (lower.contains(bad)) {
                    violations.add("第" + idx + "行日志引用敏感标识符 '" + bad + "': " + line.trim());
                }
            }
        }
        assertTrue("模板/原始 embedding 严禁写入日志：\n" + String.join("\n", violations),
                violations.isEmpty());
    }

    @Test
    public void jniSourceIsWiredIntoCmakeAndroidBuild() throws IOException {
        File root = findRepoRoot();
        String cmake = read(new File(root, "cmake/android.cmake"));
        assertTrue("cmake/android.cmake 未接入 native_face_bridge.cpp",
                cmake.contains("src/cpp/jni/native_face_bridge.cpp"));

        File cpp = new File(root, "src/cpp/jni/native_face_bridge.cpp");
        assertNotNull(cpp);
        assertFalse("实现文件不应为空", read(cpp).trim().isEmpty());
    }

    /**
     * P1-2：loadProfile 语义必须是「校验并暂存」（validated/staged），不得宣称「已加载生效」；
     * 生效判据必须走 getRuntimeStatus 的 {@code effective}。本测试为源码层结构守卫。
     */
    @Test
    public void loadProfileSemanticsAreValidateAndStageNotEffective() throws IOException {
        File root = findRepoRoot();
        String cpp = read(new File(root, "src/cpp/jni/native_face_bridge.cpp"));
        String mgr = read(new File(root,
                "src/java/com/example/rk3288_opencv/FaceModelManager.java"));
        String status = read(new File(root,
                "src/java/com/example/rk3288_opencv/FaceRuntimeStatus.java"));

        // C++：loadProfile 返回 validated/staged，内部状态位改名，彻底去除 profileLoaded 状态位。
        // 断言同样**限定在 bridge_loadProfile 的函数体内**（理由见下方 getRuntimeStatus 处的说明）：
        //   * `\"staged\":true` 在桥接文件里除本函数外**至少还有一处** —— `bridge_beginEnrollment`
        //     的 ENROLLING 响应文案，全文件 contains 靠它即可满足 ⇒ 证明不了 loadProfile 返回了 staged；
        //   * 裸标识符 `profileValidated` 出现得更多（含注释、局部量与其它 JSON 键），故状态位断言改用
        //     带类型的 `bool profileValidated` 形式，与紧邻的 `bool profileLoaded` 反断言同口径。
        String cppClean = stripComments(cpp);
        assertEquals("bridge_loadProfile 定义应恰好一处（锚点失配会让下面的作用域断言静默失效）",
                1, countOccurrences(cppClean, "jstring bridge_loadProfile("));
        String loadProfileBody = cppFunctionBody(cppClean, "jstring bridge_loadProfile(");
        assertNotNull("必须能定位 bridge_loadProfile 的函数体，否则下面两条断言无判别力",
                loadProfileBody);
        assertTrue("loadProfile 必须返回 validated:true",
                loadProfileBody.contains("\\\"validated\\\":true"));
        assertTrue("loadProfile 必须返回 staged:true",
                loadProfileBody.contains("\\\"staged\\\":true"));
        assertTrue("状态位必须为 bool profileValidated（带类型，避免被注释/局部量满足）",
                cppClean.contains("bool profileValidated"));
        assertFalse("不得再以 profileLoaded 作状态位（仅允许作为兼容别名输出）",
                cppClean.contains("bool profileLoaded"));
        // getRuntimeStatus 必须同时暴露 profileValidated 与 effective。
        // 断言**限定在 bridge_getRuntimeStatus 的函数体内**（此前是全文件 contains，判别力近乎为零）：
        //   * `\"effective\":` 这一带转义的键字面量在桥接文件里另有多处生产写入
        //     （`bridge_loadProfile` 与 `bridge_activateRuntime` 的返回文案），
        //     旧写法靠它们任意一处即可满足 ⇒ 它**证明不了**「getRuntimeStatus 暴露了 effective」；
        //   * 裸标识符 `profileValidated` 同理（注释、局部量、JSON 键均含）。
        // 限定函数体后，「在别的函数里写一份」不再能让断言通过；配合「锚点恰好一处」的计数断言，
        // 锚点失配（改签名/改函数名）会红而非静默抽取到别的函数体。
        // ⚠️ 本注释刻意不写具体处数：桥接文件由其它任务并行修改、数目会漂移；锚点与函数体判据不漂移。
        assertEquals("bridge_getRuntimeStatus 定义应恰好一处（锚点失配会让下面的作用域断言静默失效）",
                1, countOccurrences(cppClean, "jstring bridge_getRuntimeStatus("));
        String statusBody = cppFunctionBody(cppClean, "jstring bridge_getRuntimeStatus(");
        assertNotNull("必须能定位 bridge_getRuntimeStatus 的函数体，否则下面两条断言无判别力",
                statusBody);
        assertTrue("getRuntimeStatus 函数体必须暴露 profileValidated",
                statusBody.contains("\\\"profileValidated\\\":"));
        assertTrue("getRuntimeStatus 函数体必须暴露 effective",
                statusBody.contains("\\\"effective\\\":"));

        // Java：判定必须要求 validated，且就绪必须以 effective 为准。
        assertTrue("FaceModelManager 必须要求 validated 字段", mgr.contains("isNativeValidated"));
        assertTrue("FaceRuntimeStatus 必须以 effective 判 nativeProfileActive",
                status.contains("root.get(\"effective\")"));
        assertTrue("FaceRuntimeStatus 必须暴露 nativeProfileValidated",
                status.contains("nativeProfileValidated"));
    }

    /**
     * onEnrollmentFrame 回调描述符的 Java↔C++ 机械核对。
     *
     * <p><b>为什么必须单独开一个用例：</b>{@code onEnrollmentFrame} 在 Java 侧是普通
     * {@code private static void}（由 C++ 经 {@code GetStaticMethodID} 查找后回调），
     * <b>不是</b> {@code native} 声明。因此 {@code EXPECTED} 表结构上收不了它，
     * {@link #javaNativeDeclarationsMatchExpectedSignatures()} 与
     * {@link #cppRegistryMatchesJavaDeclarationsExactly()} 两个断言也都够不着它 ——
     * C++ 里那串硬编码描述符此前是<b>零机械守护</b>：Java 侧形参一变而 C++ 侧没跟着改，
     * 只会在真机上运行时才暴露（{@code GetStaticMethodID} 返回 null → 静默
     * {@code FACE_ENROLL_CAPTURE_NO_CALLBACK}），本地/CI 均不可见。
     *
     * <p><b>判别点（改坏任一处本用例必红）：</b>
     * <ul>
     *   <li>把 {@code src/cpp/jni/native_face_bridge.cpp} 中 {@code GetStaticMethodID} 的第三个实参
     *       改成任意别的字符串（如 {@code "(Ljava/nio/ByteBuffer;[D)V"}）⇒ 两侧描述符不等；</li>
     *   <li>把 {@code NativeFaceBridge.java} 的 {@code onEnrollmentFrame} 形参改掉
     *       （如 {@code float[] metrics} → {@code double[] metrics}、调换两个形参顺序、
     *       或改成 {@code ByteBuffer[]}）⇒ 由 Java 源码重新求得的描述符随之变化，两侧不等；</li>
     *   <li>把该回调改成非 {@code private static}（例如去掉 {@code static}）⇒ Java 侧抽取数为 0，
     *       命中「恰好 1 处」断言。</li>
     * </ul>
     *
     * <p><b>防「静默通过」（本仓硬性取向）：</b>本用例<b>不</b>把期望描述符写成字面量
     * ——写死等于把同一份字面量抄第二遍，两侧日后会一起漂移而用例恒绿。这里改为
     * <b>两侧各自从源码现求</b>再比对；正因如此，必须显式断言「确实抽到了」：
     * <ol>
     *   <li>Java 侧 {@code private static ... onEnrollmentFrame(...)} 声明数 == 1
     *       （0 处 = 抽取器失配，&gt;1 处 = 抽取器歧义，两者都必须红）；</li>
     *   <li>{@code native_face_bridge.cpp} 中 {@code GetStaticMethodID} 字面量出现次数 ==
     *       正则解析出的调用点数（正则一旦失配，计数不等即红，而不是「空 == 空」通过）；</li>
     *   <li>按方法名筛出的回调查找点 == 1，且其方法名恰为 {@code onEnrollmentFrame}；</li>
     *   <li>两侧描述符均非空、Java 侧形如 {@code (参数…)返回}、C++ 侧接收者 jclass 变量名非空。</li>
     * </ol>
     *
     * <p>定位方式<b>刻意不含行号</b>（本仓行号大面积漂移）：C++ 侧一律按
     * {@code GetStaticMethodID} 符号 + 字面量定位。
     */
    @Test
    public void enrollmentFrameCallbackDescriptorMatchesCppStaticMethodLookup() throws IOException {
        File root = findRepoRoot();
        String javaSrc = read(new File(root,
                "src/java/com/example/rk3288_opencv/NativeFaceBridge.java"));
        String cppSrc = read(new File(root, "src/cpp/jni/native_face_bridge.cpp"));

        // ---- Java 侧：从 private static 声明的真实形参现求描述符 ----
        List<String> javaDecls = javaPrivateStaticMethodDescriptors(javaSrc, CALLBACK_METHOD);
        assertEquals("NativeFaceBridge 中 `private static " + CALLBACK_METHOD + "` 声明必须恰好 1 处"
                        + "（0 处 = 抽取器失配，>1 处 = 抽取器歧义；两者都必须红）",
                1, javaDecls.size());
        String javaDesc = javaDecls.get(0);
        assertFalse("Java 侧描述符不得为空（否则下面的等值断言会「空 == 空」通过）",
                javaDesc.trim().isEmpty());
        assertTrue("Java 侧描述符必须形如 (参数…)返回：实际 " + javaDesc,
                javaDesc.startsWith("(") && javaDesc.indexOf(')') > 0);

        // ---- C++ 侧：按符号/字面量定位，取 GetStaticMethodID 的第 2/3 个字符串实参 ----
        List<String[]> lookups = cppStaticMethodLookups(cppSrc);
        assertEquals("native_face_bridge.cpp 中 GetStaticMethodID 字面量出现次数必须等于正则解析出的"
                        + "调用点数（不等即正则失配，必须红而非静默漏采）",
                countOccurrences(stripComments(cppSrc), "GetStaticMethodID"), lookups.size());

        List<String[]> matched = new ArrayList<>();
        for (String[] lookup : lookups) {
            if (CALLBACK_METHOD.equals(lookup[1])) {
                matched.add(lookup);
            }
        }
        assertEquals("native_face_bridge.cpp 中 GetStaticMethodID(\"" + CALLBACK_METHOD + "\", …)"
                        + " 查找点必须恰好 1 处（缺失即 C++ 回调链断裂，属真实回归）",
                1, matched.size());
        String[] hit = matched.get(0);
        assertFalse("C++ 侧接收者 jclass 变量名不得为空", hit[0].trim().isEmpty());
        String cppDesc = hit[2];
        assertFalse("C++ 侧描述符字面量不得为空", cppDesc.trim().isEmpty());

        assertEquals("onEnrollmentFrame 的 JNI 描述符两侧必须一致"
                        + "（Java 声明 vs C++ GetStaticMethodID）：Java=" + javaDesc
                        + " / C++=" + cppDesc,
                javaDesc, cppDesc);
    }
}
