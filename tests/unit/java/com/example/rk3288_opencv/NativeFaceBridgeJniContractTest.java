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
 *   <li>Java native 声明（名 + JNI 描述符）与 C++ registerFaceBridgeMethods 注册表逐条一致；</li>
 *   <li>边界常量（dim=512、gallery 上限 1000）与 direct ByteBuffer 守卫存在；</li>
 *   <li>安全红线：任何 rklog 调用行都不得引用模板/embedding 向量标识符；</li>
 *   <li>新 JNI 源已接入 cmake/android.cmake 构建源列表。</li>
 * </ol>
 * 真实的 C++ 边界行为（非 direct/长度不符/超限返回失败）依赖 JNIEnv + Android NDK，
 * 无法在 core_unit_tests（cmake/core.cmake 未收录 jni/*.cpp 且禁止改动）中实例化，
 * 故由本结构测试 + 运行时 JSON 错误码兜底。
 */
public class NativeFaceBridgeJniContractTest {

    /** 期望的 Java native 方法名 → JNI 描述符（唯一事实源）。 */
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

    /** Java 类型 → JNI 描述符片段（覆盖本类用到的 String/ByteBuffer/byte/void + 数组）。 */
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
}
