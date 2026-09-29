package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * 注册表「整份不可枚举」恢复入口的**UI 接线源码结构守卫**（配合
 * {@link RegistryQuarantineRecoveryTest} 的仓库层行为用例）。
 *
 * <p>{@code MainActivity} 是 {@code AppCompatActivity}，纯 JVM 不可构造（本仓未引入
 * Robolectric/Mockito），故恢复入口的 UI 行为无法用行为断言覆盖 —— 与
 * {@code MainActivityDisplayNameLifecycleTest} / {@code CameraFrameEpochRecheckStructureTest}
 * 同法，用**源码结构**锁定三条安全关键形态：
 * <ol>
 *   <li>人员管理入口在「注册表整份不可读 / 全坏行」时走恢复入口，且**委托仓库层 API**
 *       （{@code quarantineCorruptRegistry()}）而非在 UI 里直接做文件 IO；旧「只报无出路」文案已删除；</li>
 *   <li>恢复必须**两步确认**（提示入口只进入二次确认，不得一步直达执行）；</li>
 *   <li>成功分支必须把**旁置文件路径**（{@code result.message}）展示给用户。</li>
 * </ol>
 *
 * <h2>判别力边界（如实声明 —— 形态判据，非行为覆盖）</h2>
 * 读源码做字符下标 / 子串 / 花括号配对算术，证明不了运行时真的弹了窗、真的删/没删。
 * <b>能拦</b>：删掉恢复入口、把两步确认改成一步直达执行、成功分支不再展示路径、旧死角文案回归。
 * <b>拦不住</b>：等价重排（抽方法、改名）即可绕开；也无法验证「旁置」在运行时确实保留了原文件
 * （那是仓库层用例 {@link RegistryQuarantineRecoveryTest} 的职责）。
 */
public class RegistryRecoveryUiStructureTest {

    private static final String MAIN_ACTIVITY = "src/java/com/example/rk3288_opencv/MainActivity.java";

    @Test
    public void corruptRegistryOffersRecoveryDelegatedToRepositoryApi() throws IOException {
        String code = stripCommentsAndLiterals(readMainActivity());
        assertTrue("UI 必须委托仓库层的非破坏性恢复 API（不在 UI 直接做文件 IO）",
                code.contains("enrollmentRepository().quarantineCorruptRegistry()"));

        int method = code.indexOf("private void showPersonManagementDialog()");
        assertTrue("应有人员管理入口", method >= 0);
        int notOk = code.indexOf("if (!list.isOk())", method);
        assertTrue("应有「整份不可读」分支", notOk > method);
        int prompt = code.indexOf("promptRegistryRecovery(", notOk);
        assertTrue("整份不可读分支必须走恢复入口，而不是只弹一句 Toast",
                prompt > notOk && prompt - notOk < 400);

        int empty = code.indexOf("if (entries.isEmpty())", method);
        assertTrue("应有「entries 为空」分支", empty > method);
        int prompt2 = code.indexOf("promptRegistryRecovery(", empty);
        assertTrue("全坏行（entries 空 + skipped>0）分支也必须走恢复入口",
                prompt2 > empty && prompt2 - empty < 400);

        assertFalse("旧的「只报无出路（清除应用数据 / 手工修复）」文案必须删除",
                code.contains("需清除应用数据或手工修复注册表文件"));
    }

    @Test
    public void recoveryRequiresExplicitSecondConfirmation() throws IOException {
        String code = stripCommentsAndLiterals(readMainActivity());
        String promptBody = methodBody(code, "private void promptRegistryRecovery(@NonNull String reason)");
        assertTrue("提示入口应进入二次确认", promptBody.contains("confirmQuarantineRegistry()"));
        assertFalse("提示入口不得一步直达执行旁置（须经二次确认）",
                promptBody.contains("quarantineRegistry()"));

        String confirmBody = methodBody(code, "private void confirmQuarantineRegistry()");
        assertTrue("二次确认才执行旁置", confirmBody.contains("quarantineRegistry()"));

        // 确认文案必须如实：原文件不删、但新表为空（用未剥离注释/字面量的原文匹配，字面量在剥离后会被移除）。
        String raw = readMainActivity();
        assertTrue("二次确认文案须说明不可逆语义", raw.contains("确认旁置"));
        assertTrue("确认文案须说明原文件不会被删除", raw.contains("原文件不会被删除"));
    }

    @Test
    public void quarantineResultSurfacesQuarantinedFilePathToUser() throws IOException {
        String code = stripCommentsAndLiterals(readMainActivity());
        String body = methodBody(code, "private void quarantineRegistry()");
        assertTrue("成功分支必须把旁置文件路径（result.message）展示给用户", body.contains("result.message"));
        assertTrue("成功分支应以弹窗展示（可读完整路径）", body.contains(".show()"));
    }

    // ---------- 结构工具（与 CameraFrameEpochRecheckStructureTest 同法） ----------

    private static String methodBody(String code, String signature) {
        int sig = code.indexOf(signature);
        assertTrue("应存在方法: " + signature, sig >= 0);
        int brace = code.indexOf('{', sig);
        assertTrue("方法应有方法体: " + signature, brace > sig);
        String body = extractBlock(code, brace);
        assertNotNull("方法体花括号应闭合: " + signature, body);
        return body;
    }

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

    private static String readMainActivity() throws IOException {
        File f = new File(findRepoRoot(), MAIN_ACTIVITY);
        assertTrue("MainActivity.java 应存在", f.isFile());
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
            if (new File(dir, MAIN_ACTIVITY).isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录");
    }
}
