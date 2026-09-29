package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * 人脸特征**明文副本**用完即擦的**源码结构守卫**（任务：补齐 S4 登记的「无判别锚」）。
 *
 * <h2>被锁定的对象（本守卫覆盖的两处站点，非「全仓仅此两处」的断言）</h2>
 * 以下两处「解码后进内存的明文 embedding 副本」在 `finally` 中被 {@code Arrays.fill(decoded[0], 0.0f)}
 * 清零，且该清零语句**位于真正的 `finally` 块内**（覆盖 `continue` / 内层 `catch` / 正常返回）：
 * <ul>
 *   <li>{@code MainActivity.refreshGalleryFromRepository()} —— 逐条重建内存图库时的模板解码副本；</li>
 *   <li>{@code EnrollmentSession.fromRepository(...).forEach(...)} —— 判重循环里的模板解码副本。</li>
 * </ul>
 * 置 null 无法观察数组内容，故以**源码结构**锁定「释放前先清零、且在 finally 里」，防止后续把清零
 * 退化成裸置 null 或移出 finally（那会让 `continue` / 异常路径漏清，明文留到 GC）。
 *
 * <h2>为什么是结构守卫而非行为断言</h2>
 * 这两处的明文副本是方法内的**局部**变量，方法本身要么是 `AppCompatActivity` 成员（纯 JVM 不可构造），
 * 要么其可观察产物（解密结果）只经回调短暂出现 —— 纯 JVM 下无法稳定观察「数组是否被清零」。
 * 这与 {@code EnrollmentSessionTest.sourceNeverStoresImagesOrLogs} /
 * {@code CameraFrameEpochRecheckStructureTest} 是同一手法：先剥离注释与字符串字面量，再做结构文本匹配。
 *
 * <h2>判别力边界（如实声明，勿高估 —— 它是形态判据）</h2>
 * <b>能拦</b>（实测见报告）：
 * <ul>
 *   <li>删掉 `Arrays.fill(decoded[0], 0.0f)` ⇒ 对应方法红；</li>
 *   <li>把清零移出 `finally`（例如只放在成功路径）⇒ 对应方法红；</li>
 *   <li>把明文副本改由**另一个标识符**承载、却仍在 finally 里清 `decoded[0]` ⇒
 *       因 `decoded[0] = emb` 赋值锚丢失而红。</li>
 * </ul>
 * <b>拦不住</b>（可绕过面，务必知悉）：
 * <ul>
 *   <li><b>「a == b 别名」形态</b>：只要源码里同时存在 `decoded[0] = emb` 与 finally 内的
 *       `Arrays.fill(decoded[0], 0.0f)`，本守卫即绿 —— 即使真实明文实际被复制到**另一个未清零的**
 *       缓冲、`decoded[0]` 只是正好被清的空壳。本守卫不追踪数据流，判不了「被清的到底是不是那份明文」；</li>
 *   <li><b>等价重排</b>：把清零抽成私有 helper（如 `wipe(decoded)`）后再在 finally 里调用 ⇒
 *       删掉了字面 `Arrays.fill(decoded[0], 0.0f)` ⇒ 本守卫**误红**（是维护成本，不是安全漏检）；</li>
 *   <li>用反射 / 原生调用等不含该字面量的清零方式 ⇒ 本守卫误红。</li>
 * </ul>
 * 即：<b>本守卫只证明「源码里存在这一形态」，不证明「明文在运行时必定被清零」</b>。
 */
public class FeaturePlaintextCopyZeroingStructureTest {

    /** 明文副本承载槽的赋值锚：解码结果写入 {@code decoded[0]}。 */
    private static final String ASSIGN = "decoded[0] = emb";

    /** 清零锚：与被赋值的是**同一个标识符** {@code decoded[0]}。 */
    private static final String ZERO = "Arrays.fill(decoded[0], 0.0f)";

    @Test
    public void galleryRebuildZeroesDecodedPlaintextInsideFinally() throws IOException {
        String code = stripCommentsAndLiterals(readSource("MainActivity.java"));
        String body = methodBody(code, "private boolean refreshGalleryFromRepository()");
        assertTrue("refreshGalleryFromRepository 应把解码明文存入 decoded[0]", body.contains(ASSIGN));

        String finallyBody = finallyBody(body, "MainActivity.refreshGalleryFromRepository");
        assertTrue("refreshGalleryFromRepository 的明文副本必须在 **finally** 块内清零"
                        + "（否则 continue / 内层 catch 会漏清）", finallyBody.contains(ZERO));
    }

    @Test
    public void enrollmentDedupZeroesDecodedPlaintextInsideFinally() throws IOException {
        String code = stripCommentsAndLiterals(readSource("EnrollmentSession.java"));
        // 用 `@NonNull final`（`fromTemplates` 的同名方法无 final）唯一定位 fromRepository 的 forEach。
        String body = methodBody(code,
                "public boolean forEach(@NonNull final ExistingTemplateConsumer consumer)");
        assertTrue("fromRepository 的判重循环应把解码明文存入 decoded[0]", body.contains(ASSIGN));

        String finallyBody = finallyBody(body, "EnrollmentSession.fromRepository.forEach");
        assertTrue("判重循环的明文副本必须在 **finally** 块内清零（覆盖 return false / 正常返回）",
                finallyBody.contains(ZERO));
    }

    // ---------- 结构工具 ----------

    /**
     * 定位含 {@code signature} 的方法体，返回花括号内文本。
     *
     * <p>输入须已 {@link #stripCommentsAndLiterals}，故花括号只出现在真实代码里、可安全配对。
     */
    private static String methodBody(String code, String signature) {
        int sig = code.indexOf(signature);
        assertTrue("应存在方法: " + signature, sig >= 0);
        int brace = code.indexOf('{', sig);
        assertTrue("方法应有方法体: " + signature, brace > sig);
        String body = extractBlock(code, brace);
        assertNotNull("方法体花括号应闭合: " + signature, body);
        return body;
    }

    /** 取方法体内**第一个** {@code finally} 块的花括号内文本。 */
    private static String finallyBody(String methodBody, String where) {
        int f = methodBody.indexOf("finally");
        assertTrue(where + " 应含 finally 块", f >= 0);
        int brace = methodBody.indexOf('{', f);
        assertTrue(where + " finally 应有块体", brace > f);
        String body = extractBlock(methodBody, brace);
        assertNotNull(where + " finally 块花括号应闭合", body);
        return body;
    }

    /** 从 {@code openBrace}（须为 '{'} 起按大括号配对，返回块内文本；不配对返回 null。 */
    private static String extractBlock(String text, int openBrace) {
        int depth = 0;
        for (int i = openBrace; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    return text.substring(openBrace + 1, i);
                }
            }
        }
        return null;
    }

    /**
     * 剥离 Java 源码中的注释与字符串/字符字面量，只保留代码 token —— 与
     * {@code CameraFrameEpochRecheckStructureTest} / {@code MainActivityDisplayNameLifecycleTest} 同一手法。
     * 避免「注释里写 {@code decoded[0]}」骗过匹配，也避免字面量里的花括号干扰配对。
     */
    private static String stripCommentsAndLiterals(String src) {
        StringBuilder out = new StringBuilder(src.length());
        int i = 0;
        final int n = src.length();
        while (i < n) {
            char c = src.charAt(i);
            if (c == '/' && i + 1 < n && src.charAt(i + 1) == '/') {
                while (i < n && src.charAt(i) != '\n') i++;
            } else if (c == '/' && i + 1 < n && src.charAt(i + 1) == '*') {
                i += 2;
                while (i + 1 < n && !(src.charAt(i) == '*' && src.charAt(i + 1) == '/')) i++;
                i = Math.min(i + 2, n);
            } else if (c == '"' || c == '\'') {
                final char quote = c;
                i++;
                while (i < n && src.charAt(i) != quote) {
                    if (src.charAt(i) == '\\') i++;
                    i++;
                }
                i++;
            } else {
                out.append(c);
                i++;
            }
        }
        return out.toString();
    }

    private static String readSource(String fileName) throws IOException {
        File f = new File(findRepoRoot(), "src/java/com/example/rk3288_opencv/" + fileName);
        assertTrue("源文件应存在: " + f.getPath(), f.isFile());
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int read;
            while ((read = in.read(buf)) >= 0) {
                if (read > 0) {
                    out.write(buf, 0, read);
                }
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, "src/java/com/example/rk3288_opencv/EnrollmentSession.java").isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录");
    }
}
