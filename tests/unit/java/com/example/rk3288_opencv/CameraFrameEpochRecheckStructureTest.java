package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

import static org.junit.Assert.assertTrue;

/**
 * FACE-CAM-008 结构守卫：两条相机帧路径必须在「提交前」保留二次代次复检，
 * 且位置与表达式一致（同位置、同表达式）。
 *
 * 背景：Camera2 的 onImageAvailable / CameraX 的 analyze 只在入口校验一次代次；
 * 入口校验与 sink 提交之间发生 stop/切源时，旧帧会被推入已失效会话（人脸推理入队）。
 * 修复方式是在 computeRotationDegrees() 之后、sink.onYuv420888Frame(...) 之前插入
 * 二次 isCurrent(capturedEpoch) 复检。只改一条会造成行为不对称。
 *
 * <p><b>本类能挡什么</b>：在匹配前会**先剥离源码中的 {@code //} 行注释、{@code /* *}{@code /}
 * 块注释与字符串/字符字面量**（见 {@link #stripCommentsAndLiterals}），再按代码文本定位。
 * 因此：
 * <ul>
 *   <li>「删掉复检代码、只在注释里留下 {@code isCurrent(capturedEpoch)}」⇒ 剥离后不可见 ⇒ RED；</li>
 *   <li>「整段删除提交前复检」⇒ 复检不在 rotation→submit 之间 ⇒ RED；</li>
 *   <li>两条路径只改一条（不对称）⇒ 对应方法 RED。</li>
 * </ul>
 *
 * <p><b>本类挡不住什么（行为性判别负责）</b>：结构匹配无法识别**语义**回归 ——
 * <ul>
 *   <li>极性反转 {@code if(!isCurrent(...))} → {@code if(isCurrent(...))}（行为完全相反）；</li>
 *   <li>保留条件但删掉 {@code return;}（空体，条件形同虚设）。</li>
 * </ul>
 * 上述两类由**行为性**用例把关：Camera2 见
 * {@link Camera2CaptureControllerTest#frameNotCommittedIfEpochStaleAtSubmitPoint}，
 * CameraX 见
 * {@link CameraXFrameEpochRecheckBehaviorTest#frameNotCommittedIfEpochStaleAtSubmitPoint}
 * （**提交前**分支）与
 * {@link CameraXFrameEpochRecheckBehaviorTest#frameNotCommittedIfEpochAlreadyStaleAtEntry}
 * （**入口**分支）。
 *
 * <p><b>计数断言（{@code countOccurrences(text, RECHECK) >= 2}）的边界</b>：它是**文本计数**，
 * 判据为下界——只判「两处 {@code isCurrent(capturedEpoch)} 都在、且其中一处落在
 * rotation→submit 之间」，**不判极性、不判条件恒假**（写成 {@code if (x && false)}、或去掉
 * {@code !}，都照样命中）。语义判别一律由上述行为性用例承担；且**入口分支**的行为对偶目前只有
 * CameraX 有（Camera2 侧尚无对偶用例，其入口分支仅被本计数覆盖）。
 */
public class CameraFrameEpochRecheckStructureTest {

    private static final String RECHECK = "isCurrent(capturedEpoch)";
    private static final String CALL_ROTATION = "computeRotationDegrees()";
    private static final String SUBMIT = "sink.onYuv420888Frame(";

    @Test
    public void camera2RechecksEpochImmediatelyBeforeSubmit() throws IOException {
        String text = stripCommentsAndLiterals(readSource("Camera2CaptureController.java"));
        assertRecheckBetweenRotationAndSubmit(text, "Camera2CaptureController.onImageAvailable");
        // 边界见类 javadoc：本计数不判极性、不判条件恒假（`&& false` 亦命中）；语义判别见
        // CameraXFrameEpochRecheckBehaviorTest#frameNotCommittedIfEpochStaleAtSubmitPoint（提交前）
        // 与 #frameNotCommittedIfEpochAlreadyStaleAtEntry（入口）。
        assertTrue("完成帧路径应恰好有入口 + 提交前两处 isCurrent(capturedEpoch) 复检",
                countOccurrences(text, RECHECK) >= 2);
    }

    @Test
    public void cameraXRechecksEpochImmediatelyBeforeSubmit() throws IOException {
        String text = stripCommentsAndLiterals(readSource("CameraXCaptureController.java"));
        assertRecheckBetweenRotationAndSubmit(text, "CameraXCaptureController.analyze");
        // 边界见类 javadoc：本计数不判极性、不判条件恒假（`&& false` 亦命中）；语义判别见
        // CameraXFrameEpochRecheckBehaviorTest#frameNotCommittedIfEpochStaleAtSubmitPoint（提交前）
        // 与 #frameNotCommittedIfEpochAlreadyStaleAtEntry（入口）。
        assertTrue("完成帧路径应恰好有入口 + 提交前两处 isCurrent(capturedEpoch) 复检",
                countOccurrences(text, RECHECK) >= 2);
    }

    /** 断言 computeRotationDegrees() 调用与 sink 提交之间存在 isCurrent(capturedEpoch) 复检。 */
    private static void assertRecheckBetweenRotationAndSubmit(String text, String where) {
        int call = text.indexOf(CALL_ROTATION);
        assertTrue(where + " 应调用 computeRotationDegrees()", call >= 0);
        int submit = text.indexOf(SUBMIT, call);
        assertTrue(where + " 应在 computeRotationDegrees() 之后提交帧",
                submit > call);
        String between = text.substring(call, submit);
        assertTrue(where + " 在 computeRotationDegrees() 与 sink 提交之间必须二次代次复检，"
                        + "以缩小入口校验与提交之间的竞态窗口", between.contains(RECHECK));
    }

    private static int countOccurrences(String haystack, String needle) {
        int count = 0;
        int idx = haystack.indexOf(needle);
        while (idx >= 0) {
            count++;
            idx = haystack.indexOf(needle, idx + needle.length());
        }
        return count;
    }

    /**
     * 剥离 Java 源码中的注释与字符串/字符字面量，仅保留代码 token。
     *
     * <p>等效于 {@code tests/cpp/test_event_manager.cpp} 的 {@code collectGtestIncludes} 逐字符
     * 状态机（C++ 侧）：本方法把 {@code //} 行注释、{@code /* *}{@code /} 块注释、双引号字符串
     * （含 {@code \\} 转义）与单引号字符字面量替换为单个空格，避免「注释里留代码字样」骗过匹配。
     * 先剥离再匹配，是本守卫相对纯 grep 的关键差别。
     */
    private static String stripCommentsAndLiterals(String src) {
        StringBuilder out = new StringBuilder(src.length());
        int i = 0;
        int n = src.length();
        while (i < n) {
            char c = src.charAt(i);
            char d = (i + 1 < n) ? src.charAt(i + 1) : '\0';
            if (c == '/' && d == '/') {
                i += 2;
                while (i < n && src.charAt(i) != '\n') {
                    i++;
                }
                continue;
            }
            if (c == '/' && d == '*') {
                i += 2;
                while (i < n && !(src.charAt(i) == '*' && i + 1 < n && src.charAt(i + 1) == '/')) {
                    i++;
                }
                i = Math.min(n, i + 2);
                continue;
            }
            if (c == '"' || c == '\'') {
                char quote = c;
                i++;
                while (i < n) {
                    char e = src.charAt(i);
                    if (e == '\\') {
                        i += 2;
                        continue;
                    }
                    if (e == quote) {
                        i++;
                        break;
                    }
                    i++;
                }
                out.append(' ');
                continue;
            }
            out.append(c);
            i++;
        }
        return out.toString();
    }

    private static String readSource(String fileName) throws IOException {
        File f = new File(findRepoRoot(),
                "src/java/com/example/rk3288_opencv/" + fileName);
        assertTrue("源文件应存在: " + f.getPath(), f.isFile());
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) >= 0) {
                out.write(buf, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    /** 从 user.dir 向上定位仓库根（以 Camera2CaptureController.java 为锚点）。 */
    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir,
                    "src/java/com/example/rk3288_opencv/Camera2CaptureController.java").isFile()) {
                return dir;
            }
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录（未找到 Camera2CaptureController.java）");
    }
}
