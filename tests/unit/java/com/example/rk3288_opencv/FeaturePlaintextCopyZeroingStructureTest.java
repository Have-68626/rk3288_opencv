package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * 人脸特征**明文副本**用完即擦的**源码结构守卫**（任务：补齐 S4 登记的「无判别锚」）。
 *
 * <h2>被锁定的对象（本守卫覆盖的两处站点，非「全仓仅此两处」的断言）</h2>
 * 以下两处「解码后进内存的明文 embedding 副本」在 `finally` 中被 `Arrays.fill(<表达式>, 0.0f)`
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
 * <h2>本守卫**只**是形态判据（勿高估其能力）</h2>
 * 它证明的是「源码里存在这一**形态**」，**不**证明「明文在运行时必定被清零」。判据只做两件事：
 * <ol>
 *   <li><b>赋值形态</b>：方法体内出现 {@code <标识符>[0] = ...}（明文副本落入某个下标 0 的槽）；</li>
 *   <li><b>清零形态（槽关联）</b>：在**该赋值之后的某个 `finally` 块内**出现
 *       {@code Arrays.fill(<同一槽>, 0.0f)} —— 清零实参须**回指赋值槽本身**（同一标识符）。</li>
 * </ol>
 *
 * <h2>判别设计（槽关联 + 一致重命名仍绿）</h2>
 * <ul>
 *   <li><b>槽关联</b>：先由赋值形态**捕获**槽标识符，再要求某一 `finally` 块内的 `Arrays.fill` 实参
 *       **回指同一标识符**（按捕获到的槽构造 pattern）—— 这样「赋值槽」与「清零槽」被绑定，一个无关的、
 *       恰含 {@code Arrays.fill(<别的槽>, 0.0f)} 的 finally **掩盖不了**本应清零的那处被删；</li>
 *   <li><b>一致重命名仍绿</b>：锚点是**模式 + 捕获组**而非固定字面量，把槽在两处**同名**改写
 *       （如 {@code decoded} → {@code plainBuf}）时捕获组随之变化、关联仍成立 ⇒ 不误红（这是引入槽关联的
 *       前提，不能以「回到固定字面量」换取关联）；</li>
 *   <li>`finally` 的**定位**为「赋值之后出现的**全部** `finally` 块，任一块内出现与槽关联的清零形态即通过」
 *       —— 方法内新增更早的 try/finally 不会再把判据引到错误的块上。</li>
 * </ul>
 *
 * <h2>判别力边界（如实声明，勿高估 —— 它是形态判据）</h2>
 * <b>能拦</b>（变异实测见报告；下列均为「**删去 / 改写字面形态**」类变异 —— 对「**保留形态但使其实不可达**」不成立，见下）：
 * <ul>
 *   <li>删掉清零调用 ⇒ 对应方法红（赋值之后的 any finally 内都不含与槽关联的 `Arrays.fill`）；</li>
 *   <li>删掉真清零、却在赋值之后**新增一个无关的** finally（`Arrays.fill(<别的槽>, 0.0f)`）⇒ 对应方法红
 *       （槽关联使别的槽的清零**掩盖不了**本处被删）。</li>
 * </ul>
 * <b>拦不住 / 仍会误红</b>（可绕过面与维护成本，务必知悉）：
 * <ul>
 *   <li><b>不可达形态 ⇒ 仍绿（形态仍在、语义已死）</b>：把真清零保留在**同一个 finally** 内、但**包进永不执行
 *       的分支**（如 `if (false) { Arrays.fill(<槽>, 0.0f); }`）⇒ 字面形态仍在，判据仍绿，而运行时明文**根本不会**
 *       被清零（`continue` / 异常路径照样泄漏）。本守卫**只看文本、不看可达性**，故「把清零移出 finally ⇒ 红」
 *       这类结论**对不可达形态不成立** —— 「离开 finally」的语义后果可由「留在 finally 内但不可达」复现而仍绿；</li>
 *   <li><b>诱饵槽抢占 ⇒ 仍绿</b>：在真槽赋值**之前**插入任意一个自带 finally 清零的 `decoy[0] = ...`，
 *       则本守卫取到的「方法体内**首个** `[0]=`」变成 decoy 的槽、关联到 decoy 的清零 ⇒ 判据绿，而真明文
 *       那处是否被清**完全未被检查**。当前两处站点的 `[0]=` 槽唯一、不触发，但新增任何 `<x>[0]=` 都可能抢占；</li>
 *   <li><b>「a == b 别名」形态仍可绕</b>：只要方法里同时存在 `<槽>[0] = ...` 与赋值之后某个 finally 内对
 *       **同一槽**调用的 `Arrays.fill(<槽>, 0.0f)`，本守卫即绿 —— 即使真实明文实际被复制到**另一个未清零的**
 *       缓冲。本守卫**不追踪数据流**，判不了「被清的到底是不是那份明文」；</li>
 *   <li><b>抽 helper 仍会误红</b>：把清零抽成私有方法（如 `wipe(slot)`）后再在 finally 里调用 ⇒ finally
 *       块内不再出现 `Arrays.fill(...)` 字面 —— 属维护成本（需同步更新本守卫），不是安全漏检；</li>
 *   <li>用反射 / 原生调用等不含该字面形态的清零方式 ⇒ 误红；<b>等价字面量写法亦误红</b>：
 *       `Arrays.fill(<槽>, 0.0F)` / `Arrays.fill(<槽>, 0f)` / `Arrays.fill(<槽>, 0)` 以及多参形式
 *       `Arrays.fill(<槽>, 0.0f, 0, len)`（本守卫 pattern 只认单参 `0.0f`，此处的大小写、字面后缀与参数个数皆不宽容）⇒ 判红；</li>
 *   <li><b>「同名异槽」极端形态</b>：若明文被复制进某个 `[0]` 槽、而该槽名恰好又出现在一个**无关** finally
 *       的 `Arrays.fill` 里，本守卫仍会判绿（槽关联**不看控制流 / 作用域**）。当前两处站点各只有一个 finally
 *       且槽名唯一，不触发。</li>
 * </ul>
 */
public class FeaturePlaintextCopyZeroingStructureTest {

    /**
     * 明文副本承载槽的赋值**形态**：某标识符的下标 0 槽被赋值（`=(?!=)` 排除 `==` 比较）。
     * **捕获组 1 = 槽标识符**，供后文把「赋值槽」与「清零槽」关联。
     */
    private static final Pattern SLOT_ZERO_ASSIGN =
            Pattern.compile("\\b([A-Za-z_]\\w*)\\s*\\[\\s*0\\s*]\\s*=(?!=)");

    /**
     * 针对**指定槽**的清零**形态**：{@code Arrays.fill(<槽>, 0.0f)}，实参须**回指该槽标识符**本身
     * （槽关联）。用 {@link Pattern#quote} 锚定槽名，避免槽名含正则元字符时的误配。
     */
    private static Pattern zeroFillForSlot(String slot) {
        return Pattern.compile("Arrays\\s*\\.\\s*fill\\s*\\(\\s*"
                + Pattern.quote(slot) + "\\b[^(),]*,\\s*0\\.0f\\s*\\)");
    }

    /** 赋值形态的匹配结果：{@code slot}=槽标识符，{@code at}=赋值在方法体中的起点。 */
    private static final class SlotAssign {
        final String slot;
        final int at;

        SlotAssign(String slot, int at) {
            this.slot = slot;
            this.at = at;
        }
    }

    @Test
    public void galleryRebuildZeroesDecodedPlaintextInsideFinally() throws IOException {
        String code = stripCommentsAndLiterals(readSource("MainActivity.java"));
        String body = methodBody(code, "private boolean refreshGalleryFromRepository()");
        SlotAssign slot = firstSlotAssign(body, "MainActivity.refreshGalleryFromRepository");
        assertTrue("refreshGalleryFromRepository 的明文副本清零（Arrays.fill(..., 0.0f)）必须位于"
                        + "赋值之后的某个 finally 块内，且实参须回指同一赋值槽（否则 continue / 内层 catch"
                        + "会漏清明文，或清零被无关形态掩盖）",
                zeroedInSomeFinallyAfter(body, slot));
    }

    @Test
    public void enrollmentDedupZeroesDecodedPlaintextInsideFinally() throws IOException {
        String code = stripCommentsAndLiterals(readSource("EnrollmentSession.java"));
        // 用 `@NonNull final`（`fromTemplates` 的同名方法无 final）唯一定位 fromRepository 的 forEach。
        String body = methodBody(code,
                "public boolean forEach(@NonNull final ExistingTemplateConsumer consumer)");
        SlotAssign slot = firstSlotAssign(body, "EnrollmentSession.fromRepository.forEach");
        assertTrue("判重循环的明文副本清零必须位于赋值之后的某个 finally 块内"
                        + "（覆盖 return false / 正常返回），且实参须回指同一赋值槽",
                zeroedInSomeFinallyAfter(body, slot));
    }

    // ---------- 结构工具 ----------

    /** 定位含 {@code signature} 的方法体，返回花括号内文本。 */
    private static String methodBody(String code, String signature) {
        int sig = code.indexOf(signature);
        assertTrue("应存在方法: " + signature, sig >= 0);
        int brace = code.indexOf('{', sig);
        assertTrue("方法应有方法体: " + signature, brace > sig);
        String body = extractBlock(code, brace);
        assertNotNull("方法体花括号应闭合: " + signature, body);
        return body;
    }

    /** 找到**首个** {@code <标识符>[0] = ...} 赋值，返回其槽标识符与位置；找不到即断言失败。 */
    private static SlotAssign firstSlotAssign(String body, String where) {
        Matcher m = SLOT_ZERO_ASSIGN.matcher(body);
        assertTrue(where + " 应把解码明文副本存入某个 <标识符>[0] 槽", m.find());
        return new SlotAssign(m.group(1), m.start());
    }

    /**
     * 从赋值位置起**逐个扫描**全部 {@code finally} 块（而非只取第一个），只要任一 finally 块体内出现
     * **与赋值槽关联**的 {@code Arrays.fill(<槽>, 0.0f)} 形态即返回 true。
     *
     * <p>槽关联使三种真问题都落为 false：「清零被移出 finally」「清零被删」，以及「真清零被删、却被一个
     * 无关的、恰含别的槽 {@code Arrays.fill} 的 finally 掩盖」。
     */
    private static boolean zeroedInSomeFinallyAfter(String body, SlotAssign assign) {
        Pattern fill = zeroFillForSlot(assign.slot);
        int search = assign.at;
        while (true) {
            int f = body.indexOf("finally", search);
            if (f < 0) {
                return false;
            }
            int brace = body.indexOf('{', f);
            if (brace < 0) {
                return false;
            }
            String block = extractBlock(body, brace);
            if (block == null) {
                return false;
            }
            if (fill.matcher(block).find()) {
                return true;
            }
            search = brace + 1;
        }
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
