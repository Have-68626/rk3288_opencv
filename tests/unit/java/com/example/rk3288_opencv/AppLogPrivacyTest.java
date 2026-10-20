package com.example.rk3288_opencv;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;


import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

public class AppLogPrivacyTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void warningMasksIdentityBeforeLogcatAndBothFileSinks() throws Exception {
        assertRealLogOutputsAreMasked("w");
    }

    @Test
    public void debugIdentityWithoutDigitsStillTriggersMasking() throws Exception {
        assertRealLogOutputsAreMasked("d");
    }

    private void assertRealLogOutputsAreMasked(String method) throws Exception {
        Path work = tmp.newFolder().toPath();
        Path classes = work.resolve("classes");
        Files.createDirectories(classes);
        List<String> sources = new ArrayList<>();
        Path root = sourceRoot();
        for (String name : new String[]{"AppLog", "FileLogSink", "SensitiveDataUtil"}) {
            Path source = work.resolve(name + ".java");
            Files.copy(root.resolve("src/java/com/example/rk3288_opencv/" + name + ".java"), source);
            sources.add(source.toString());
        }
        // 仅替换 Android/JNI 边界，日志格式、预筛、脱敏与双目录写入均执行完整生产类。
        addSource(work, sources, "Log", "package android.util; public class Log {"
                + "public static final int VERBOSE=2,DEBUG=3,INFO=4,WARN=5,ERROR=6,ASSERT=7;"
                + "public static String captured; public static int println(int p,String t,String m){captured=m;return 0;}"
                + "public static String getStackTraceString(Throwable t){return t.toString();}}");
        addSource(work, sources, "Process", "package android.os; public class Process {public static int myTid(){return 1;}}");
        addSource(work, sources, "Context", "package android.content; public class Context {"
                + "public static final int MODE_PRIVATE=0; public java.io.File getFilesDir(){return null;}"
                + "public java.io.File getExternalFilesDir(String t){return null;}"
                + "public SharedPreferences getSharedPreferences(String n,int m){return null;}}");
        addSource(work, sources, "SharedPreferences", "package android.content; public interface SharedPreferences {"
                + "int getInt(String k,int d); Editor edit(); interface Editor {Editor putInt(String k,int v);void apply();}}");
        addSource(work, sources, "NativeBridge", "package com.example.rk3288_opencv; class NativeBridge {"
                + "static void nativeConfigureLog(String a,String b,String c){}}");
        for (String name : new String[]{"NonNull", "Nullable"}) {
            addSource(work, sources, name, "package androidx.annotation; public @interface " + name + " {}");
        }
        // Android 的测试编译类路径没有 javax.tools；调用项目 CI 已提供的完整 JDK。
        String executable = System.getProperty("os.name").startsWith("Windows") ? "javac.exe" : "javac";
        java.io.File javac = new java.io.File(System.getProperty("java.home"), "bin/" + executable);
        assertTrue("行为测试需要完整 JDK", javac.isFile());
        List<String> args = new ArrayList<>(Arrays.asList(javac.getAbsolutePath(), "--release", "11",
                "-encoding", "UTF-8", "-d", classes.toString()));
        args.addAll(sources);
        Process compile = new ProcessBuilder(args).redirectErrorStream(true).start();
        ByteArrayOutputStream diagnostics = new ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        int count;
        while ((count = compile.getInputStream().read(buffer)) >= 0) diagnostics.write(buffer, 0, count);
        assertTrue("完整生产日志类应能在平台边界替身下编译",
                compile.waitFor() == 0);

        try (URLClassLoader loader = new URLClassLoader(new URL[]{classes.toUri().toURL()},
                null)) {
            Class<?> log = loader.loadClass("com.example.rk3288_opencv.AppLog");
            Class<?> sink = loader.loadClass("com.example.rk3288_opencv.FileLogSink");
            Constructor<?> constructor = sink.getDeclaredConstructor(List.class, String.class);
            constructor.setAccessible(true);
            Path internal = work.resolve("internal");
            Path external = work.resolve("external");
            Field sinkField = log.getDeclaredField("sink");
            sinkField.setAccessible(true);
            sinkField.set(null, constructor.newInstance(Arrays.asList(internal.toFile(), external.toFile()), "probe.log"));
            java.lang.reflect.Method write = log.getDeclaredMethod(method, String.class, String.class, String.class);
            write.setAccessible(true);
            String identity = "private_identity_fixture";
            for (String key : new String[]{"personId", "person_id", "PERSON-ID"}) {
                write.invoke(null, "Privacy", "migration", "显示名迁移失败 " + key + "=" + identity);
                String logcat = (String) loader.loadClass("android.util.Log").getField("captured").get(null);
                assertTrue("Logcat 保留诊断键与显式掩码", logcat.contains(key + "=") && logcat.contains("***"));
                assertFalse("Logcat 不得泄漏身份原值", logcat.contains(identity));
                for (Path dir : Arrays.asList(internal, external)) {
                    String contents = new String(Files.readAllBytes(dir.resolve("probe.log")), StandardCharsets.UTF_8);
                    assertFalse("内部与外部日志都不得泄漏身份原值", contents.contains(identity));
                    assertTrue("文件日志须保留显式掩码", contents.contains("***"));
                }
            }
        }
    }

    private static void addSource(Path work, List<String> sources, String name, String content) throws Exception {
        Path source = work.resolve(name + ".java");
        Files.write(source, content.getBytes(StandardCharsets.UTF_8));
        sources.add(source.toString());
    }

    private static Path sourceRoot() {
        String override = System.getProperty("audit.storage.sourceRoot");
        if (override != null) return Paths.get(override);
        Path root = Paths.get(System.getProperty("user.dir")).toAbsolutePath();
        while (root != null && !Files.isRegularFile(root.resolve("src/java/com/example/rk3288_opencv/AppLog.java"))) {
            root = root.getParent();
        }
        if (root == null) throw new IllegalStateException("未找到日志生产源码");
        return root;
    }
}
