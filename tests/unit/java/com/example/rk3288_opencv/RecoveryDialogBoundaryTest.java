package com.example.rk3288_opencv;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.net.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;
import static org.junit.Assert.*;

/**
 * 编译执行恢复 UI 的生产方法，验证菜单和二次确认的可操作性。
 * AppCompatActivity 在 JVM 中不可构造，故只替换 Android 界面边界，不复制生产分支。
 * 替身按实际 AppCompat 1.6.1 的消息/列表互斥规则呈现内容，不覆盖真机布局或像素。
 */
public class RecoveryDialogBoundaryTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    @Test public void emptyRegistryStillOffersOrphanRecovery() throws Exception {
        AndroidEnrollmentRepository repo = emptyRepo();
        File orphan = orphan(repo);
        try (Probe probe = compileProbe(repo)) {
            probe.invoke("people");
            probe.ui.awaitUi();
            assertEquals("空表也必须展示操作菜单", 1, probe.ui.dialogs.size());
            Dialog menu = probe.ui.dialogs.get(0);
            assertNotNull("必须保留孤儿恢复入口", menu.neutral);
            menu.neutral.click(null, 0);
            probe.ui.awaitUi();
            assertEquals("入口必须实际打开孤儿列表", 2, probe.ui.dialogs.size());
            assertArrayEquals(new String[]{orphan.getName()}, probe.ui.dialogs.get(1).visibleItems());
            assertTrue("查看入口不得删除原模板", orphan.isFile());
        }
    }

    @Test public void orphanListRemainsSelectableAndOnlyRequestsConfirmation() throws Exception {
        AndroidEnrollmentRepository repo = emptyRepo();
        File orphan = orphan(repo);
        try (Probe probe = compileProbe(repo)) {
            probe.invoke("orphans");
            probe.ui.awaitUi();
            Dialog list = probe.ui.dialogs.get(0);
            assertArrayEquals("警告不能遮蔽可选文件", new String[]{orphan.getName()}, list.visibleItems());
            assertNotNull("旁置前仍须展示风险说明", list.warning);
            probe.ui.repo = emptyRepo();
            list.select(0);
            assertEquals(orphan.getName(), probe.ui.selectedOrphan);
            assertSame("切换模型后，旧列表只能操作原空间", repo, probe.ui.selectedRepository);
            assertTrue("选择仅进入二次确认", orphan.isFile());
        }
    }

    @Test public void damagedRegistryKeepsHealthyPersonSelectable() throws Exception {
        AndroidEnrollmentRepository repo = emptyRepo();
        File template = repo.templateFileFor("seed");
        Files.write(template.toPath(), new byte[]{1, 2, 3});
        String row = "73656564 7 2 3 " + String.join("", Collections.nCopies(64, "a")) + " 1";
        Files.write(repo.registryFile().toPath(),
                ("FREG2\nCOUNT 2\n" + row + "\nBAD-ROW\nEND 2\n").getBytes(StandardCharsets.UTF_8));
        assertEquals("夹具确实保留一个人员", 1, repo.list().entries.size());
        try (Probe probe = compileProbe(repo)) {
            probe.invoke("people");
            probe.ui.awaitUi();
            Dialog list = probe.ui.dialogs.get(0);
            assertEquals("损坏警告不能遮蔽人员列表", 1, list.visibleItems().length);
            assertTrue("仍须说明损坏行及重写后果", list.warning.contains("重写注册表"));
            probe.ui.repo = emptyRepo();
            list.select(0);
            assertEquals("seed", probe.ui.selectedPerson);
            assertSame("删除确认必须绑定列表来源空间", repo, probe.ui.selectedRepository);
            assertTrue("选择仅进入删除确认", template.isFile());
        }
    }

    @Test public void pendingIdentityHasSeparateTextUntilVerification() throws Exception {
        try (Probe probe = compileProbe(emptyRepo())) {
            java.lang.reflect.Method format = probe.ui.getClass().getMethod("format", String.class);
            assertEquals("识别中", format.invoke(probe.ui, "RECOGNIZING"));
            assertEquals("未识别（Unknown）", format.invoke(probe.ui, "AUTH_FAIL Unknown"));
            assertEquals("识别: sample 90%", format.invoke(probe.ui, "VERIFIED sample 90%"));
        }
    }

    @Test public void selectedImageActuallyCallsNativeOnWorkerAndDisplaysDecision() throws Exception {
        try (Probe probe = compileProbe(emptyRepo())) {
            probe.ui.getClass().getMethod("image", String.class).invoke(probe.ui, "synthetic.jpg");
            probe.ui.awaitUi();
            assertEquals("synthetic.jpg", HarnessUi.NativeBridge.lastImage);
            assertNotEquals(Thread.currentThread().getName(), HarnessUi.NativeBridge.threadName);
            assertEquals("识别通过：synthetic_person", probe.ui.dialogs.get(0).message);
        }
    }

    @Test public void changedImageGenerationDiscardsInFlightResult() throws Exception {
        try (Probe probe = compileProbe(emptyRepo())) {
            HarnessUi.NativeBridge.entered = new CountDownLatch(1);
            HarnessUi.NativeBridge.release = new CountDownLatch(1);
            probe.ui.getClass().getMethod("image", String.class).invoke(probe.ui, "synthetic.jpg");
            assertTrue(HarnessUi.NativeBridge.entered.await(5, TimeUnit.SECONDS));
            probe.ui.singleImageGeneration.incrementAndGet();
            HarnessUi.NativeBridge.release.countDown();
            probe.ui.awaitUi();
            assertTrue("退出/换源后旧结果不得弹窗", probe.ui.dialogs.isEmpty());
        }
    }

    private AndroidEnrollmentRepository emptyRepo() throws Exception {
        AndroidEnrollmentRepository repo = new AndroidEnrollmentRepository(null, tmp.newFolder());
        javax.crypto.KeyGenerator generator = javax.crypto.KeyGenerator.getInstance("AES");
        generator.init(128);
        final javax.crypto.SecretKey key = generator.generateKey();
        repo.setKeyProviderForTesting(new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            @Override public boolean isSupported() { return true; }
            @Override public javax.crypto.SecretKey getOrCreateKeyV1() { return key; }
        });
        Files.write(repo.registryFile().toPath(), "FREG2\nCOUNT 0\nEND 0\n".getBytes(StandardCharsets.UTF_8));
        assertTrue("完整注册表及支持的平台必须允许旁置", repo.listOrphanTemplates().quarantineAllowed);
        return repo;
    }
    private File orphan(AndroidEnrollmentRepository repo) throws Exception {
        File file = repo.templateFileFor("orphan");
        Files.write(file.toPath(), new byte[]{1, 2, 3});
        return file;
    }

    private Probe compileProbe(AndroidEnrollmentRepository repo) throws Exception {
        HarnessUi.NativeBridge.entered = null;
        HarnessUi.NativeBridge.release = null;
        File root = new File(System.getProperty("audit.ui.sourceRoot", System.getProperty("user.dir")));
        File source;
        while (!(source = new File(root, "src/java/com/example/rk3288_opencv/MainActivity.java")).isFile()) {
            root = root.getParentFile();
            assertNotNull("无法定位生产 UI 源文件", root);
        }
        String text = new String(Files.readAllBytes(source.toPath()), StandardCharsets.UTF_8);
        String methods = productionMethod(text, "showPersonManagementDialog")
                + "\n" + productionMethod(text, "showOrphanTemplateRecovery")
                + "\n" + productionMethod(text, "formatRecognitionResult")
                + "\n" + productionMethod(text, "inferSelectedImage")
                + "\n" + productionMethod(text, "jsonFirstStringField");
        String generated = "package com.example.rk3288_opencv;\nimport java.util.List;\n"
                + "public final class RecoveryUiProbe extends RecoveryDialogBoundaryTest.HarnessUi {\n"
                + " public RecoveryUiProbe(AndroidEnrollmentRepository repo) { super(repo); }\n"
                + " public void people() { showPersonManagementDialog(); }\n"
                + " public void image(String path) { mockFilePath = path; inferSelectedImage(path); }\n"
                + " public void orphans() { showOrphanTemplateRecovery(enrollmentRepository()); }\n" + methods + "\n}";
        generated = generated.substring(0, generated.length() - 1)
                + " public String format(String raw) { return formatRecognitionResult(raw); }\n}";
        File output = tmp.newFolder();
        String classpath = new File(getClass().getProtectionDomain().getCodeSource().getLocation().toURI())
                + File.pathSeparator
                + new File(AndroidEnrollmentRepository.class.getProtectionDomain().getCodeSource().getLocation().toURI())
                + File.pathSeparator + new File(Test.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        File generatedFile = new File(output, "RecoveryUiProbe.java");
        Files.write(generatedFile.toPath(), generated.getBytes(StandardCharsets.UTF_8));
        // Android 的测试编译类路径没有 javax.tools；使用 CI 已有的完整 JDK，不新增依赖。
        String executable = System.getProperty("os.name").startsWith("Windows") ? "javac.exe" : "javac";
        File javac = new File(System.getProperty("java.home"), "bin/" + executable);
        assertTrue("测试需要完整 JDK", javac.isFile());
        Process compile = new ProcessBuilder(javac.getAbsolutePath(), "--release", "11",
                "-proc:none", "-encoding", "UTF-8", "-classpath", classpath,
                "-d", output.getAbsolutePath(), generatedFile.getAbsolutePath()).redirectErrorStream(true).start();
        ByteArrayOutputStream diagnostics = new ByteArrayOutputStream();
        byte[] bytes = new byte[4096];
        int count;
        while ((count = compile.getInputStream().read(bytes)) >= 0) diagnostics.write(bytes, 0, count);
        assertEquals("应能编译执行原生产方法：" + diagnostics.toString("UTF-8"), 0, compile.waitFor());
        URLClassLoader loader = new URLClassLoader(new URL[]{output.toURI().toURL()}, getClass().getClassLoader());
        HarnessUi ui = (HarnessUi) loader.loadClass("com.example.rk3288_opencv.RecoveryUiProbe")
                .getConstructor(AndroidEnrollmentRepository.class).newInstance(repo);
        return new Probe(loader, ui);
    }
    private static String productionMethod(String text, String name) {
        java.util.regex.Matcher method = java.util.regex.Pattern.compile(
                "private\\s+(?:static\\s+)?(?:void|String)\\s+" + name + "\\s*\\(").matcher(text);
        assertTrue("应有生产方法 " + name, method.find());
        int start = method.start();
        int open = text.indexOf('{', start), depth = 0;
        char quote = 0;
        boolean line = false, block = false, escaped = false;
        for (int i = open; i < text.length(); i++) {
            char c = text.charAt(i), next = i + 1 < text.length() ? text.charAt(i + 1) : 0;
            if (line) { if (c == '\n') line = false; continue; }
            if (block) { if (c == '*' && next == '/') { block = false; i++; } continue; }
            if (quote != 0) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == quote) quote = 0;
                continue;
            }
            if (c == '/' && next == '/') { line = true; i++; continue; }
            if (c == '/' && next == '*') { block = true; i++; continue; }
            if (c == '"' || c == '\'') { quote = c; continue; }
            if (c == '{') depth++;
            if (c == '}' && --depth == 0) return text.substring(start, i + 1);
        }
        throw new AssertionError("生产方法未闭合：" + name);
    }
    private static final class Probe implements AutoCloseable {
        final URLClassLoader loader;
        final HarnessUi ui;
        Probe(URLClassLoader loader, HarnessUi ui) { this.loader = loader; this.ui = ui; }
        void invoke(String method) throws Exception { ui.getClass().getMethod(method).invoke(ui); }
        @Override public void close() throws Exception { loader.close(); }
    }

    public interface Click { void click(Object dialog, int which); }
    public static final class Dialog {
        String message, warning;
        String[] items = new String[0];
        Click itemClick, neutral;
        String[] visibleItems() { return message == null ? items.clone() : new String[0]; }
        void select(int index) {
            assertTrue("只能选择实际可见项", index >= 0 && index < visibleItems().length);
            itemClick.click(null, index);
        }
    }
    public static class HarnessUi {
        public @interface Nullable {}
        public @interface NonNull {}
        AndroidEnrollmentRepository repo;
        volatile AndroidEnrollmentRepository selectedRepository;
        public final List<Dialog> dialogs = new CopyOnWriteArrayList<>();
        final Semaphore completed = new Semaphore(0);
        volatile Throwable uiError;
        volatile String selectedPerson, selectedOrphan;
        protected final AtomicLong singleImageGeneration = new AtomicLong();
        protected String templateSpaceId = "", mockFilePath;
        protected boolean isRunning, isStopped;
        public HarnessUi(AndroidEnrollmentRepository repo) { this.repo = repo; }
        protected AndroidEnrollmentRepository enrollmentRepository() { return repo; }
        protected PersonDisplayNameStore displayNameStore() { return new PersonDisplayNameStore(); }
        protected boolean isFinishing() { return false; }
        protected boolean isDestroyed() { return false; }
        protected void stopMonitoring() { isRunning = false; singleImageGeneration.incrementAndGet(); }
        protected String displayNameFor(String id) { return id; }
        protected String getString(int resource) { return "恢复提示"; }
        protected void promptRegistryRecovery(String reason, AndroidEnrollmentRepository selected) {}
        protected void promptClearLegacyDisplayNamesIfAny() {}
        protected void confirmDeletePerson(AndroidEnrollmentRepository.Entry entry, AndroidEnrollmentRepository selected, PersonDisplayNameStore names) {
            selectedPerson = entry.userId;
            selectedRepository = selected;
        }
        protected void confirmQuarantineOrphan(String file, AndroidEnrollmentRepository selected) {
            selectedOrphan = file;
            selectedRepository = selected;
        }
        protected void runOnUiThread(Runnable action) {
            try { action.run(); } catch (Throwable t) { uiError = t; }
            finally { completed.release(); }
        }
        public static final class NativeBridge {
            static volatile String lastImage, threadName;
            static volatile CountDownLatch entered, release;
            public static String nativeInferFaceFromImage(String path, int topK) {
                lastImage = path; threadName = Thread.currentThread().getName();
                if (entered != null) entered.countDown();
                if (release != null) try { release.await(5, TimeUnit.SECONDS); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); }
                return "{\"ok\":true,\"decision\":{\"passNow\":true},\"TopK\":{\"hits\":[{\"personId\":\"synthetic_person\"}]}}";
            }
        }
        public static final class SingleImageResult {
            public static String summary(String json) { return com.example.rk3288_opencv.SingleImageResult.summary(json); }
        }
        public static final class PersonDisplayNameStore {
            public String get(String id) { return id; }
        }
        void awaitUi() throws Exception {
            assertTrue("生产异步 UI 回调应完成", completed.tryAcquire(5, TimeUnit.SECONDS));
            if (uiError != null) throw new AssertionError("UI 回调失败", uiError);
        }
        public static final class AlertDialog {
            public static final class Builder {
                private final HarnessUi owner;
                private final Dialog dialog = new Dialog();
                public Builder(HarnessUi owner) { this.owner = owner; }
                public Builder setTitle(String title) { return this; }
                public Builder setMessage(String message) { dialog.message = message; return this; }
                public Builder setView(TextView view) { dialog.warning = view.text; return this; }
                public Builder setItems(String[] items, Click action) {
                    dialog.items = items.clone(); dialog.itemClick = action; return this;
                }
                public Builder setPositiveButton(String label, Click action) { return this; }
                public Builder setNegativeButton(String label, Click action) { return this; }
                public Builder setNeutralButton(String label, Click action) { dialog.neutral = action; return this; }
                public Dialog show() { owner.dialogs.add(dialog); return dialog; }
            }
        }
        public static final class Toast {
            public static final int LENGTH_SHORT = 0, LENGTH_LONG = 1;
            public static Toast makeText(HarnessUi owner, String text, int duration) { return new Toast(); }
            public void show() {}
        }
        public static final class TextView {
            String text;
            public TextView(HarnessUi owner) {}
            public void setText(String text) { this.text = text; }
        }
        public static final class R {
            public static final class string {
                public static final int ui_orphan_templates_entry = 1;
                public static final int ui_orphan_templates_warning = 2;
                public static final int ui_orphan_templates_title = 3;
            }
        }
    }
}
