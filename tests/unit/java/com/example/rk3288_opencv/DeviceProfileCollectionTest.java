package com.example.rk3288_opencv;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.net.URL;
import java.net.URLClassLoader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

public class DeviceProfileCollectionTest {
    @Rule public TemporaryFolder tmp = new TemporaryFolder();

    @Test
    public void realCollectorDistinguishesInstalledAbsentAndInvisibleGms() throws Exception {
        Path work = tmp.newFolder().toPath();
        Path classes = work.resolve("classes");
        Files.createDirectories(classes);
        List<String> sources = new ArrayList<>();
        Path production = sourceRoot().resolve("src/java/com/example/rk3288_opencv/DeviceProfile.java");
        Path copy = work.resolve("DeviceProfile.java");
        Files.copy(production, copy);
        sources.add(copy.toString());
        // 仅替换 Android 查询边界；实际执行 collect，不能用 forTest 绕过生产探测。
        source(work, sources, "Context", "package android.content; public class Context {"
                + "public static final String ACTIVITY_SERVICE=\"activity\";"
                + "public Object getSystemService(String n){return null;}"
                + "public android.content.pm.PackageManager getPackageManager(){return new android.content.pm.PackageManager();}}");
        source(work, sources, "PackageManager", "package android.content.pm; public class PackageManager {"
                + "public static int mode; public PackageInfo getPackageInfo(String n,int f) throws NameNotFoundException {"
                + "if(!n.equals(\"com.google.android.gms\"))throw new AssertionError();"
                + "if(mode==1)throw new NameNotFoundException(); if(mode==2)throw new SecurityException();"
                + "return new PackageInfo();} public static class NameNotFoundException extends Exception {}}");
        source(work, sources, "PackageInfo", "package android.content.pm; public class PackageInfo {}");
        source(work, sources, "ActivityManager", "package android.app; public class ActivityManager {"
                + "public static class MemoryInfo {public long totalMem;} public void getMemoryInfo(MemoryInfo m){}}");
        source(work, sources, "StatFs", "package android.os; public class StatFs {public StatFs(String p){} public long getTotalBytes(){return 1;}}");
        source(work, sources, "Build", "package android.os; public class Build {"
                + "public static String MANUFACTURER=\"fixture\",BRAND=\"fixture\",MODEL=\"fixture\",DEVICE=\"fixture\","
                + "HARDWARE=\"fixture\",BOARD=\"fixture\",FINGERPRINT=\"fixture\",TYPE=\"user\";"
                + "public static class VERSION {public static int SDK_INT=34;}}");
        for (String annotation : new String[]{"NonNull", "Nullable"}) {
            source(work, sources, annotation, "package androidx.annotation; public @interface " + annotation + " {}");
        }
        String executable = System.getProperty("os.name").startsWith("Windows") ? "javac.exe" : "javac";
        File javac = new File(System.getProperty("java.home"), "bin/" + executable);
        assertTrue("行为回归需要完整 JDK", javac.isFile());
        List<String> args = new ArrayList<>(Arrays.asList(javac.getAbsolutePath(), "--release", "11",
                "-encoding", "UTF-8", "-d", classes.toString()));
        args.addAll(sources);
        Process process = new ProcessBuilder(args).redirectErrorStream(true).start();
        byte[] diagnostics = new byte[4096];
        while (process.getInputStream().read(diagnostics) >= 0) { /* 排空编译输出，避免管道阻塞。 */ }
        assertEquals("平台边界替身应能编译实际设备采集类", 0, process.waitFor());
        try (URLClassLoader loader = new URLClassLoader(new URL[]{classes.toUri().toURL()}, null)) {
            Class<?> profile = loader.loadClass("com.example.rk3288_opencv.DeviceProfile");
            Class<?> context = loader.loadClass("android.content.Context");
            Class<?> packages = loader.loadClass("android.content.pm.PackageManager");
            java.lang.reflect.Method collect = profile.getDeclaredMethod("collect", context);
            collect.setAccessible(true);
            java.lang.reflect.Field hasGms = profile.getDeclaredField("hasGms");
            hasGms.setAccessible(true);
            for (int mode = 0; mode < 3; mode++) {
                packages.getField("mode").setInt(null, mode);
                Object result = collect.invoke(null, context.getConstructor().newInstance());
                assertEquals("已安装、缺失和不可见必须区分", mode == 0, hasGms.getBoolean(result));
            }
        }
    }

    private static void source(Path work, List<String> files, String name, String contents) throws Exception {
        Path file = work.resolve(name + ".java");
        Files.write(file, contents.getBytes(StandardCharsets.UTF_8));
        files.add(file.toString());
    }

    private static Path sourceRoot() {
        String override = System.getProperty("audit.device.sourceRoot");
        if (override != null) return Paths.get(override);
        Path root = Paths.get(System.getProperty("user.dir")).toAbsolutePath();
        while (root != null && !Files.isRegularFile(root.resolve("src/java/com/example/rk3288_opencv/DeviceProfile.java"))) {
            root = root.getParent();
        }
        if (root == null) throw new IllegalStateException("未找到设备采集生产源码");
        return root;
    }
}
