package com.example.rk3288_opencv;

import org.junit.Test;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/**
 * 显示名生命周期**源码结构守卫**（任务 B）。
 *
 * <p>为什么用源码断言而不是单元测试：{@code MainActivity} 是 {@code AppCompatActivity}，
 * 纯 JVM 下不可构造（触碰 android.jar 会 "not mocked"），而本仓未引入 Robolectric/Mockito
 * （{@code app/build.gradle} 只有 junit）。这与 {@code EnrollmentSessionTest} 的
 * {@code sourceNeverStoresImagesOrLogs} 是同一手法。
 *
 * <p>被锁定的契约（均为本次改造的核心，退化成旧行为时必须失败）：
 * <ol>
 *   <li>显示名**不再以明文写入 SharedPreferences**；</li>
 *   <li>采集期显示名**仅存内存**，且该赋值发生在启动采集**之前**；</li>
 *   <li>加密落盘**只发生在注册成功分支**（图库替换成功之后）；</li>
 *   <li>取消与销毁都清空内存显示名；</li>
 *   <li>迁移顺序为「加密 → 验证可读 → 删原值」。</li>
 * </ol>
 */
public class MainActivityDisplayNameLifecycleTest {

    private static final String MAIN_ACTIVITY =
            "src/java/com/example/rk3288_opencv/MainActivity.java";

    private static String source() throws IOException {
        File src = new File(findRepoRoot(), MAIN_ACTIVITY);
        assertTrue("MainActivity.java 应存在", src.isFile());
        return new String(Files.readAllBytes(src.toPath()), StandardCharsets.UTF_8);
    }

    private static int count(String hay, String needle) {
        int n = 0;
        for (int i = hay.indexOf(needle); i >= 0; i = hay.indexOf(needle, i + needle.length())) {
            n++;
        }
        return n;
    }

    /** 契约 1：明文写入路径必须已彻底删除。 */
    @Test
    public void plaintextSharedPreferencesWriteIsGone() throws IOException {
        String text = source();
        assertFalse("不得再以明文 putString 写显示名（应改为加密存储）",
                text.contains("putString(PREF_PERSON_NAME_PREFIX"));
        assertFalse("旧的明文写入方法应已删除", text.contains("persistDisplayName"));
    }

    /** 契约 2：采集期仅存内存，且赋值早于启动采集（此前是先落盘再采集）。 */
    @Test
    public void displayNameIsMemoryOnlyDuringCaptureAndBeforeCaptureStart() throws IOException {
        String text = source();
        int assign = text.indexOf("pendingEnrollmentDisplayName = displayName;");
        assertTrue("采集应把显示名存入内存字段", assign >= 0);

        int captureStart = text.indexOf("startNativeEnrollmentCapture();");
        assertTrue("应存在启动 native 采集的调用点", captureStart >= 0);
        assertTrue("内存赋值必须发生在启动采集之前（先存内存、后开始采集）",
                assign < captureStart);
    }

    /**
     * 契约 3：加密落盘只允许出现在**注册提交路径**（`add` 成功之后），且与图库替换结果**解耦**。
     *
     * <p>契约演变（A1 审计 P3-3）：最初要求「落盘在图库替换成功之后」，但那会让「模板已落盘、
     * 图库替换失败」的人**永久丢失显示名**。正确语义是**与模板的持久化对齐**——`add` 成功即注册
     * 已落盘，此时就应写显示名。
     */
    @Test
    public void encryptedPersistHappensInCommitPathDecoupledFromGallery() throws IOException {
        String text = source();
        int save = text.indexOf("displayNameStore().save(");
        assertTrue("应存在显示名加密写入调用", save >= 0);

        int addOk = text.indexOf("AndroidEnrollmentRepository.Result add = enrollmentRepository().add(");
        assertTrue("应存在仓库写入调用", addOk >= 0);
        assertTrue("显示名落盘必须在仓库写入（add）之后", save > addOk);

        // 与图库替换解耦：不得再用「图库成功」作为落盘的前置条件。
        int galleryFailBranch = text.indexOf("已加密写入本地，但运行中图库替换失败（未更新）");
        assertTrue("应存在图库替换失败分支", galleryFailBranch >= 0);
        assertTrue("显示名落盘不得排在『图库替换失败』分支之后（那等于与图库结果耦合）",
                save < galleryFailBranch);

        // 仅此一处落盘调用（防止有人又在采集开始前加一次）
        assertTrue("显示名加密写入应只有一处调用点，实际=" + count(text, "displayNameStore().save("),
                count(text, "displayNameStore().save(") == 1);
    }

    /** 契约 4：取消、会话结束、销毁三条路径都要清空内存显示名。 */
    @Test
    public void memoryDisplayNameIsClearedOnCancelCommitAndDestroy() throws IOException {
        String text = source();
        int cleared = count(text, "pendingEnrollmentDisplayName = null;");
        assertTrue("内存显示名至少应在「会话结束 / 取消 / 销毁」三处清空，实际=" + cleared,
                cleared >= 3);

        int cancel = text.indexOf("private void cancelEnrollmentInternal()");
        assertTrue("应有 cancelEnrollmentInternal", cancel >= 0);
        int cancelClear = text.indexOf("pendingEnrollmentDisplayName = null;", cancel);
        assertTrue("取消路径必须清空内存显示名", cancelClear > cancel && cancelClear - cancel < 800);

        int destroy = text.indexOf("protected void onDestroy()");
        assertTrue("应有 onDestroy", destroy >= 0);
        int destroyClear = text.indexOf("pendingEnrollmentDisplayName = null;", destroy);
        assertTrue("销毁路径必须清空内存显示名", destroyClear > destroy && destroyClear - destroy < 400);
    }

    /** 契约 5：迁移必须「先验证密文可读、再删原明文」，绝不先删后验。 */
    @Test
    public void migrationVerifiesCiphertextBeforeDeletingPlaintext() throws IOException {
        String text = source();
        int migrate = text.indexOf("private void migrateLegacyDisplayNames()");
        assertTrue("应有一次性迁移方法", migrate >= 0);

        int verify = text.indexOf("readFromDisk(", migrate);
        assertTrue("迁移应读回密文做验证", verify > migrate);
        // 「删原值」必须存在**于验证之后**——迁移里另有一处「空值直接删」，那一路无需验证。
        int removeAfterVerify = text.indexOf("prefs.edit().remove(key).apply();", verify);
        assertTrue("必须先验证密文可读，再删原值（顺序不得颠倒）", removeAfterVerify > verify);

        assertTrue("迁移成功才置位完成标志（失败保留原明文待重试）",
                text.contains("PREF_NAME_MIGRATED"));
    }

    /**
     * 契约 6：孤儿清理必须传**注册表完整人员集合**，不得传「成功解码模板的子集」。
     *
     * <p>回归背景：若误传 ids（只含解码成功的条目），模板损坏而被 {@code skipped} 的人会被当成
     * 「已注销」，其显示名文件被当作孤儿删除——**这是数据丢失**。
     */
    @Test
    public void orphanCleanupUsesFullRegistrySetNotDecodedSubset() throws IOException {
        String text = source();
        assertTrue("应存在孤儿清理调用", text.contains("store.removeOrphans("));
        assertTrue("孤儿清理必须传注册表完整集合 allRegisteredIds",
                text.contains("store.removeOrphans(new java.util.HashSet<>(allRegisteredIds))"));
        assertFalse("不得用成功解码子集 ids 做孤儿清理（模板损坏者会被误删显示名）",
                text.contains("store.removeOrphans(new java.util.HashSet<>(ids))"));
        assertTrue("应构造注册表完整人员集合（与 ids 并存）",
                text.contains("allRegisteredIds.add(e.userId);"));
    }

    /**
     * 契约 7（A3 确认代理点出的关键缺口）：`authoritative` 门禁的**使用点**必须有测试锁定。
     *
     * <p>缺口背景：`AndroidEnrollmentRepositoryTest` 只覆盖了该标志的**计算**，而 `MainActivity`
     * 里的**使用点**无任何断言——若有人删掉 `if (list.authoritative)` 回退为无条件 `removeOrphans`，
     * 当时的全部测试**仍然全绿**，而那条回退正是会造成数据丢失的写法。
     *
     * <p><b>局限（如实说明）</b>：与本文件其他守卫一样是**字面匹配**，等价重排即可绕开；
     * 真正的行为级锁定需要能在 JVM 内构造 `MainActivity`（本仓无 Robolectric/Mockito）。
     */
    @Test
    public void orphanCleanupIsGatedByRegistryAuthoritative() throws IOException {
        String text = source();
        int gate = text.indexOf("if (list.authoritative)");
        assertTrue("孤儿清理必须以 list.authoritative 为门禁（否则损坏注册表会误删显示名）",
                gate >= 0);
        int remove = text.indexOf("store.removeOrphans(", gate);
        assertTrue("removeOrphans 必须落在权威性门禁之内", remove > gate && remove - gate < 300);
    }

    /**
     * 契约 8（历史明文清除，任务 B 决策）：**只删显示名键、绝不整体清空、且以 `commit()` 落盘**。
     *
     * <p>三条都能造成用户可见损害：误清其他设置＝配置丢失；用 `apply()`＝异步写入、无法确认
     * 是否真的落盘，却会提示「已清除」。
     */
    @Test
    public void legacyClearTouchesOnlyDisplayNameKeysAndCommitsSynchronously() throws IOException {
        String text = source();
        int clear = text.indexOf("private void clearLegacyDisplayNames()");
        assertTrue("应有历史显示名清除方法", clear >= 0);
        int end = text.indexOf("\n    }", clear);
        assertTrue(end > clear);
        String body = text.substring(clear, end);

        assertTrue("必须用 selectKeysForClearing 选定键（只匹配显示名前缀）",
                body.contains("LegacyDisplayNames.selectKeysForClearing("));
        assertTrue("必须用 commit() 同步落盘并取回结果", body.contains("ed.commit()"));
        assertFalse("不得用异步的 apply()（无法确认是否落盘却会提示完成）", body.contains("apply()"));
        assertFalse("不得整体清空 SharedPreferences（会误删其他设置）", body.contains(".clear()"));
    }

    /** 契约 9：清除前必须有二次确认（不可逆操作不得一步直达）。 */
    @Test
    public void legacyClearRequiresExplicitConfirmation() throws IOException {
        String text = source();
        int confirm = text.indexOf("private void confirmClearLegacyDisplayNames()");
        assertTrue("应有清除的二次确认方法", confirm >= 0);
        assertTrue("确认文案须说明不可恢复", text.contains("不可恢复"));
        assertTrue("确认文案须说明模板与人员 ID 不受影响", text.contains("加密模板不受影响"));
        // 提示入口只做「提示 + 提供清除」，不得直接删除。
        int prompt = text.indexOf("private void promptClearLegacyDisplayNamesIfAny()");
        assertTrue(prompt >= 0);
        int promptEnd = text.indexOf("\n    }", prompt);
        assertFalse("提示入口不得直接执行清除（须经用户确认）",
                text.substring(prompt, promptEnd).contains("clearLegacyDisplayNames()"));
    }

    /**
     * 契约（FACE-JAVA-002）：{@code displayNameStore()} 的**构造路径不得创建目录**。
     *
     * <p>为什么必须有这条：该 accessor 在「平台不支持」的早退路径上也会被取到
     * （{@code migrateLegacyDisplayNames} 的 {@code displayNameStore().isSupported()} 是一次纯能力查询；
     * {@code refreshGalleryFromRepository} 更是**无条件**先取实例）。若在构造路径里 {@code mkdirs()}，
     * API&lt;23 设备会凭空多出一个**永远用不到**的 {@code files/face_names/} 空目录。
     * 目录的实际创建推迟到**首次写入**（{@code FeatureTemplateEncryptedStore.writeEnvelopeAtomically}）；
     * 读取路径在目录缺失时本就按「无文件」正确处理（{@code listFiles()} → null → 0；文件不存在 → NOT_FOUND）。
     *
     * <p><b>判别力</b>：把 {@code if (!dir.exists()) dir.mkdirs();} 加回该方法体内 ⇒ 本用例红
     * （已覆盖 {@code mkdir} / {@code mkdirs}）；{@code Files.createDirectories(...)} 亦在射程内
     * （判别前先剥离注释与字符串/字符字面量，故「注释里提及 mkdir」不会误红）。
     *
     * <p><b>仍可绕过（本守卫拦不住的路径，如实声明，勿高估）</b>：
     * <ol>
     *   <li>把建目录抽到一个**被 accessor 调用的私有方法**里——accessor 方法体只剩调用语句，
     *       不含任何建目录字面量 ⇒ 本用例（及其它源码守卫）都不红；{@code PersonDisplayNameStoreTest}
     *       的两条用例直接 {@code new PersonDisplayNameStore(dir)}、不走 MainActivity 路径，也拦不住。</li>
     *   <li>用其它建目录 API 绕开这三个字面量（如 {@code Os.mkdir}、反射、第三方工具）——文本匹配即失效。</li>
     * </ol>
     * <p><b>假红面（残量，如实声明）</b>：判别前已剥离方法体内的行/块注释与字符串/字符字面量，故在方法体内
     * 写「不得 mkdir」之类注释、或字面量里出现这些词，均**不会**误红；但若这几个词作为**真实标识符**出现在
     * 方法体代码里（如变量名 {@code dirToMkdir}），仍会误红——此类假红需人工甄别，属词法匹配的固有代价。
     * 根因：本用例（及本文件全部守卫）是**字面匹配**，等价重排 / 抽取即绕过；真正的行为级锁定需要在
     * JVM 内构造 {@code MainActivity}（本仓无 Robolectric/Mockito）。故它**不是「已封死」**，只是当前
     * **唯一可行**的自动化近似——{@code PersonDisplayNameStoreTest} 的两条用例只钉住「存储层构造器与
     * 能力查询无文件系统副作用」，钉不住 MainActivity 这一半（它是 {@code AppCompatActivity}，纯 JVM
     * 不可构造，见类 javadoc）。
     */
    @Test
    public void displayNameStoreAccessorDoesNotCreateDirectory() throws IOException {
        String text = source();
        final String sig = "private PersonDisplayNameStore displayNameStore()";
        int start = text.indexOf(sig);
        assertTrue("应存在 displayNameStore() accessor", start >= 0);
        // 花括号配对取方法体：避免把后续方法一起圈进来（文本切片一旦外扩，断言就可能被邻接代码拯救）。
        int open = text.indexOf('{', start);
        assertTrue("accessor 应有方法体", open > start);
        int depth = 0;
        int end = -1;
        for (int i = open; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) { end = i; break; }
            }
        }
        assertTrue("应能取到 displayNameStore() 的方法体", end > open);
        String body = text.substring(open, end);
        // 剥离注释与字符串/字符字面量后再判别：消除「注释里提及 mkdir ⇒ 假红」，从而可安全把射程
        // 扩大到 mkdir（含 mkdirs）与 Files.createDirectories 两类绕过路径（局限见该类 javadoc）。
        String code = stripCommentsAndLiterals(body);
        assertFalse("displayNameStore() 构造路径不得创建目录（File.mkdir/mkdirs，FACE-JAVA-002）",
                code.contains("mkdir"));
        assertFalse("displayNameStore() 构造路径不得创建目录（java.nio.file.Files.createDirectories，FACE-JAVA-002）",
                code.contains("createDirectories"));
    }

    /**
     * 去掉 Java 源码片段中的行/块注释与字符串/字符字面量，仅保留代码，供文本匹配使用——
     * 避免「注释或字面量里出现 mkdir」造成的假红。纯词法扫描，不做完整解析；本用例的 accessor
     * 方法体很小，足够可靠（文本块 """ 之类不在适用范围内）。
     */
    private static String stripCommentsAndLiterals(String body) {
        StringBuilder out = new StringBuilder(body.length());
        int i = 0;
        final int n = body.length();
        while (i < n) {
            char c = body.charAt(i);
            if (c == '/' && i + 1 < n && body.charAt(i + 1) == '/') {
                while (i < n && body.charAt(i) != '\n') i++; // 行注释
            } else if (c == '/' && i + 1 < n && body.charAt(i + 1) == '*') {
                i += 2;
                while (i + 1 < n && !(body.charAt(i) == '*' && body.charAt(i + 1) == '/')) i++; // 块注释
                i = Math.min(i + 2, n);
            } else if (c == '"' || c == '\'') {
                final char quote = c;
                i++;
                while (i < n && body.charAt(i) != quote) {
                    if (body.charAt(i) == '\\') i++; // 跳过转义字符
                    i++;
                }
                i++; // 越过收尾引号
            } else {
                out.append(c);
                i++;
            }
        }
        return out.toString();
    }

    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, MAIN_ACTIVITY).isFile()) return dir;
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录");
    }
}
