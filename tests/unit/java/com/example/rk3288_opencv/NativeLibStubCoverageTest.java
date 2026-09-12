package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * Q-CQ-B07-008：native-lib-stub 描述符覆盖 RED 判别测试。
 *
 * 背景：RK_SKIP_OPENCV=ON 时 Gradle 使用 src/cpp/native-lib-stub.cpp 作为
 * native-lib（stub 无 JNI_OnLoad/RegisterNatives，纯靠 JNI mangled 符号名解析）。
 * 因此 Java 侧每一个 native 声明都必须有对应的导出符号，否则运行时抛
 * UnsatisfiedLinkError。
 *
 * 本测试枚举 src/java 下的全部 Java native 描述符（方法名 + 签名），断言
 * native-lib-stub.cpp 为每个描述符导出对应 mangled 符号，且 C 参数个数与
 * Java 参数个数一致（缺失 → 测试失败）。
 *
 * 边界（Q-CQ-B07-008）：stub 仅承担「描述符解析 + 最小占位」，不得冒充完整
 * native。本测试只核验符号/参数个数覆盖，不校验真实算法逻辑。
 */
public class NativeLibStubCoverageTest {

    private static final Pattern NATIVE_DECL = Pattern.compile(
            "\\bnative\\s+([\\w<>.$\\[\\] ]+?)\\s+([A-Za-z_$][\\w$]*)\\s*\\(([^)]*)\\)\\s*;",
            Pattern.DOTALL);
    private static final Pattern IMPORT_DECL = Pattern.compile(
            "^\\s*import\\s+([A-Za-z_$][\\w$]*(?:\\.[A-Za-z_$][\\w$]*)+)\\s*;",
            Pattern.MULTILINE);

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

    /** 从 user.dir 向上定位仓库根（src/cpp/native-lib-stub.cpp 为锚点）。 */
    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, "src/cpp/native-lib-stub.cpp").isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录（未找到 src/cpp/native-lib-stub.cpp）");
    }

    private static String readFile(File file) throws IOException {
        assertNotNull("源文件不存在: " + file, file);
        assertTrue("源文件不存在: " + file, file.isFile());
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    private static String stripComments(String src) {
        src = src.replaceAll("/\\*[\\s\\S]*?\\*/", " ");
        src = src.replaceAll("//[^\\r\\n]*", "");
        return src;
    }

    private static String parsePackage(String src) {
        Matcher m = Pattern.compile("^\\s*package\\s+([\\w.]+)\\s*;", Pattern.MULTILINE).matcher(src);
        return m.find() ? m.group(1) : "";
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

    /** 提取 native 方法：返回类型、方法名、参数类型列表。 */
    private static List<String[]> parseNativeMethods(String src) {
        List<String[]> out = new ArrayList<>();
        Matcher m = NATIVE_DECL.matcher(stripComments(src));
        while (m.find()) {
            String[] rec = {m.group(1).trim(), m.group(2), m.group(3).trim()};
            out.add(rec);
        }
        return out;
    }

    /** 参数串 → 类型列表（去参数名，保留 byte[]/ByteBuffer 等类型）。 */
    private static List<String> paramTypes(String params) {
        if (params.trim().isEmpty()) {
            return Collections.emptyList();
        }
        List<String> out = new ArrayList<>();
        for (String p : params.split(",")) {
            String t = p.trim();
            if (t.isEmpty()) {
                continue;
            }
            // 去掉尾部参数名，保留类型
            String type = t.replaceAll("\\s+[A-Za-z_$][\\w$]*$", "").trim();
            out.add(type);
        }
        return out;
    }

    /** Java 类型 → JNI 描述符片段（支持基本类型/String/数组/import 类）。 */
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

    /** 包/类/方法 → JNI mangled 符号（包内下划线转 _1，点转 _）。 */
    private static String mangleSymbol(String pkg, String cls, String method) {
        StringBuilder sb = new StringBuilder("Java_");
        String[] segs = pkg.split("\\.");
        for (int i = 0; i < segs.length; i++) {
            if (i > 0) {
                sb.append('_');
            }
            sb.append(segs[i].replace("_", "_1"));
        }
        return sb.append('_').append(cls).append('_').append(method).toString();
    }

    /** 从 stub 源中取 mangled 符号对应的 C 参数个数（env/this 不计）。 */
    private static int stubCParamCount(String stubSrc, String symbol) {
        Matcher m = Pattern.compile(Pattern.quote(symbol) + "\\(([^)]*)\\)").matcher(stubSrc);
        assertTrue("stub 缺少导出符号 " + symbol, m.find());
        String args = m.group(1).trim();
        if (args.isEmpty()) {
            return 0;
        }
        int count = 1;
        for (int i = 0; i < args.length(); i++) {
            if (args.charAt(i) == ',') {
                count++;
            }
        }
        // 去掉 JNIEnv* 与 this/clazz 两个固定参数
        return count - 2;
    }

    @Test
    public void stubExportsEveryJavaNativeDescriptor() throws IOException {
        File root = findRepoRoot();
        File javaDir = new File(root, "src/java/com/example/rk3288_opencv");
        File stubFile = new File(root, "src/cpp/native-lib-stub.cpp");
        String stubSrc = readFile(stubFile);

        // 任务13：NativeFaceBridge 静态方法组已接入 stub（此前缺失会导致 UnsatisfiedLinkError）。
        String[] javaClasses = {"MainActivity", "NativeBridge", "NativeFaceBridge"};
        StringBuilder report = new StringBuilder("Java native 描述符清单（stub 必须逐项导出）：\n");
        List<String> missingSymbols = new ArrayList<>();
        List<String> arityMismatch = new ArrayList<>();

        for (String cls : javaClasses) {
            File javaFile = new File(javaDir, cls + ".java");
            String javaSrc = readFile(javaFile);
            String pkg = parsePackage(javaSrc);
            Map<String, String> imports = parseImports(javaSrc);
            for (String[] rec : parseNativeMethods(javaSrc)) {
                String ret = rec[0];
                String name = rec[1];
                List<String> types = paramTypes(rec[2]);
                StringBuilder desc = new StringBuilder("(");
                for (String t : types) {
                    desc.append(typeToDesc(t, imports, pkg));
                }
                desc.append(')').append(typeToDesc(ret, imports, pkg));

                String symbol = mangleSymbol(pkg, cls, name);
                report.append("  ").append(cls).append('.').append(name)
                        .append(' ').append(desc).append('\n');

                boolean present = stubSrc.contains(symbol);
                if (!present) {
                    missingSymbols.add(symbol);
                    continue;
                }
                // 参数个数一致 → 最小调用可安全入参（stub 边界：仅解析 + 占位）
                int stubArgs = stubCParamCount(stubSrc, symbol);
                if (stubArgs != types.size()) {
                    arityMismatch.add(symbol + " stub=" + stubArgs + " java=" + types.size());
                }
            }
        }

        assertTrue("stub 缺失以下导出符号（Java native 描述符无法解析）：\n"
                + String.join("\n", missingSymbols)
                + "\n\n" + report, missingSymbols.isEmpty());
        assertEquals("stub 参数个数与 Java 声明不一致：\n"
                + String.join("\n", arityMismatch), Collections.emptyList(), arityMismatch);
    }
}
