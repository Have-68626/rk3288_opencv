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

    /** 契约 3：加密落盘只允许出现在「图库替换成功」之后的分支里。 */
    @Test
    public void encryptedPersistHappensOnlyAfterGallerySucceeded() throws IOException {
        String text = source();
        int save = text.indexOf("displayNameStore().save(");
        assertTrue("应存在显示名加密写入调用", save >= 0);

        int galleryFailBranch = text.indexOf("已加密写入本地，但运行中图库替换失败（未更新）");
        assertTrue("应存在图库替换失败分支", galleryFailBranch >= 0);
        assertTrue("显示名落盘必须在图库替换成功之后（失败分支不得落盘）",
                save > galleryFailBranch);

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

    private static File findRepoRoot() {
        File dir = new File(System.getProperty("user.dir", "."));
        while (dir != null) {
            if (new File(dir, MAIN_ACTIVITY).isFile()) return dir;
            dir = dir.getParentFile();
        }
        throw new IllegalStateException("无法定位仓库根目录");
    }
}
