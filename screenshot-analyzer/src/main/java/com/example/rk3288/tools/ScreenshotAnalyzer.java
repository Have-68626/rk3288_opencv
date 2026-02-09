package com.example.rk3288.tools;

import javax.imageio.ImageIO;
import java.awt.Color;
import java.awt.Font;
import java.awt.FontMetrics;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.image.BufferedImage;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileFilter;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.Optional;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public final class ScreenshotAnalyzer {
    private static final Pattern TS_PATTERN = Pattern.compile(".*?(\\d{8})_(\\d{6}).*");
    private static final DateTimeFormatter TS_FORMAT = DateTimeFormatter.ofPattern("yyyyMMdd_HHmmss", Locale.US);

    public static void main(String[] args) throws Exception {
        Args parsed = Args.parse(args);
        File projectRoot = detectProjectRoot();
        File dir = resolveExistingDir(projectRoot, parsed.dirPath);
        if (!dir.isDirectory()) {
            throw new IllegalArgumentException("目录不存在或不是目录: " + dir.getAbsolutePath());
        }

        File outFile = resolveForWrite(projectRoot, parsed.outPath);
        File cropsDir = resolveForWrite(projectRoot, parsed.cropsPath);
        Files.createDirectories(outFile.getParentFile().toPath());
        Files.createDirectories(cropsDir.toPath());

        List<Screenshot> screenshots = listScreenshots(dir);
        if (screenshots.isEmpty()) {
            writeEmptyReport(outFile, dir);
            return;
        }

        PrototypeSet prototypeSet = PrototypeSet.createDefault();

        BufferedImage prev = null;
        AnalysisSummary summary = new AnalysisSummary();
        List<FrameResult> results = new ArrayList<>();

        for (Screenshot sc : screenshots) {
            BufferedImage img = ImageIO.read(sc.file);
            if (img == null) {
                results.add(FrameResult.unreadable(sc));
                continue;
            }

            FrameResult r = analyzeOne(sc, img, prev, prototypeSet, cropsDir);
            results.add(r);
            summary.accumulate(r);
            prev = img;
        }

        writeReport(outFile, dir, results, summary);
    }

    private static File detectProjectRoot() {
        File cwd = new File(System.getProperty("user.dir"));
        if (looksLikeProjectRoot(cwd)) return cwd;
        File parent = cwd.getParentFile();
        if (parent != null && looksLikeProjectRoot(parent)) return parent;
        return cwd;
    }

    private static boolean looksLikeProjectRoot(File dir) {
        return new File(dir, "settings.gradle").isFile() && new File(dir, "gradlew.bat").isFile();
    }

    private static File resolveExistingDir(File base, String path) {
        File f = new File(path);
        if (f.isAbsolute()) return f;
        File inBase = new File(base, path);
        if (inBase.exists()) return inBase;
        return inBase;
    }

    private static File resolveForWrite(File base, String path) {
        File f = new File(path);
        if (f.isAbsolute()) return f;
        return new File(base, path);
    }

    private static FrameResult analyzeOne(Screenshot sc,
                                         BufferedImage img,
                                         BufferedImage prev,
                                         PrototypeSet prototypeSet,
                                         File cropsDir) throws IOException {
        ImageStats stats = ImageStats.compute(img);
        boolean blackScreen = stats.meanLuma < 18.0 && stats.varLuma < 45.0 && stats.darkRatio > 0.92;

        double diffScore = prev == null ? Double.NaN : ImageDiff.compute(prev, img);
        boolean maybeFrozen = prev != null && diffScore < 2.5;

        Roi readyRoi = Roi.of(img, 0.00, 0.06, 0.55, 0.33);
        YellowDetect yellow = YellowDetect.detect(img, readyRoi);
        boolean systemReadyVisible = yellow.yellowRatio > 0.0018 && yellow.maxClusterPixels >= 120;

        Roi statusBarRoi = Roi.of(img, 0.00, 0.00, 1.00, 0.10);
        Roi overlayRoi = Roi.of(img, 0.25, 0.00, 0.75, 0.16);

        BufferedImage statusCrop = crop(img, statusBarRoi);
        BufferedImage overlayCrop = crop(img, overlayRoi);
        File statusOut = new File(cropsDir, sc.baseName + "_statusbar.png");
        File overlayOut = new File(cropsDir, sc.baseName + "_overlay.png");
        ImageIO.write(statusCrop, "png", statusOut);
        ImageIO.write(overlayCrop, "png", overlayOut);

        MetricTriple statusMetrics = MetricsFromRoi.extractStatusBar(img, statusBarRoi, prototypeSet);
        MetricTriple overlayMetrics = MetricsFromRoi.extractOverlay(img, overlayRoi, prototypeSet);

        MetricDiff metricDiff = MetricDiff.compute(statusMetrics, overlayMetrics);

        return new FrameResult(
                sc,
                img.getWidth(),
                img.getHeight(),
                blackScreen,
                maybeFrozen,
                diffScore,
                systemReadyVisible,
                yellow.yellowRatio,
                statusMetrics,
                overlayMetrics,
                metricDiff,
                stats,
                statusOut,
                overlayOut
        );
    }

    private static BufferedImage crop(BufferedImage img, Roi roi) {
        int x = clamp(roi.x, 0, img.getWidth() - 1);
        int y = clamp(roi.y, 0, img.getHeight() - 1);
        int w = clamp(roi.w, 1, img.getWidth() - x);
        int h = clamp(roi.h, 1, img.getHeight() - y);
        return img.getSubimage(x, y, w, h);
    }

    private static int clamp(int v, int lo, int hi) {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    private static void writeEmptyReport(File outFile, File dir) throws IOException {
        try (BufferedWriter w = new BufferedWriter(new FileWriter(outFile, StandardCharsets.UTF_8))) {
            w.write("# 截图分析报告\n\n");
            w.write("- 目录: " + dir.getAbsolutePath() + "\n");
            w.write("- 结果: 未找到 PNG 截图\n");
        }
    }

    private static void writeReport(File outFile, File dir, List<FrameResult> results, AnalysisSummary summary) throws IOException {
        try (BufferedWriter w = new BufferedWriter(new FileWriter(outFile, StandardCharsets.UTF_8))) {
            w.write("# 截图分析报告\n\n");
            w.write("- 目录: " + dir.getAbsolutePath() + "\n");
            w.write("- 截图数量: " + results.size() + "\n");
            w.write("- 黑屏判定: " + summary.blackCount + " / " + results.size() + "\n");
            w.write("- UI 可能停滞: " + summary.freezeCount + " / " + Math.max(0, results.size() - 1) + "\n");
            w.write("- SYSTEM READY 出现: " + summary.readyCount + " / " + results.size() + "\n");
            w.write("- 指标可解析(状态栏/悬浮窗): " + summary.metricsParseableStatus + " / " + summary.metricsParseableOverlay + "\n");
            w.write("\n");

            w.write("## 指标不一致统计（可解析样本）\n");
            w.write("- FPS 差值: " + summary.fpsDiffSummary() + "\n");
            w.write("- CPU 差值: " + summary.cpuDiffSummary() + "\n");
            w.write("- MEM 差值: " + summary.memDiffSummary() + "\n");
            w.write("\n");

            w.write("## 明细\n");
            w.write("| 时间戳 | 文件 | 分辨率 | 黑屏 | 停滞 | diffScore | READY | READY黄像素占比 | 状态栏(FPS/CPU/MEM) | 悬浮窗(FPS/CPU/MEM) | 差值(FPS/CPU/MEM) | 状态栏裁剪 | 悬浮窗裁剪 |\n");
            w.write("|---|---|---:|---:|---:|---:|---:|---:|---|---|---|---|---|\n");

            for (FrameResult r : results) {
                String ts = r.screenshot.timestamp.map(Object::toString).orElse("--");
                String status = r.statusMetrics.toShortString();
                String overlay = r.overlayMetrics.toShortString();
                String diff = r.metricDiff.toShortString();
                String statusLink = relLink(outFile, r.statusCropFile);
                String overlayLink = relLink(outFile, r.overlayCropFile);
                w.write("| " + ts + " | " + r.screenshot.file.getName() + " | " + r.width + "x" + r.height +
                        " | " + yesNo(r.blackScreen) +
                        " | " + yesNo(r.maybeFrozen) +
                        " | " + fmt(r.diffScore) +
                        " | " + yesNo(r.systemReadyVisible) +
                        " | " + String.format(Locale.US, "%.4f", r.readyYellowRatio) +
                        " | " + status +
                        " | " + overlay +
                        " | " + diff +
                        " | " + statusLink +
                        " | " + overlayLink +
                        " |\n");
            }
        }
    }

    private static String relLink(File outFile, File target) {
        String base = outFile.getParentFile().toURI().relativize(target.toURI()).getPath();
        if (base == null || base.isEmpty()) {
            base = target.getName();
        }
        return "[" + target.getName() + "](" + base + ")";
    }

    private static String fmt(double v) {
        if (Double.isNaN(v)) return "--";
        return String.format(Locale.US, "%.3f", v);
    }

    private static String yesNo(boolean b) {
        return b ? "Y" : "N";
    }

    private static List<Screenshot> listScreenshots(File dir) {
        File[] list = dir.listFiles(new FileFilter() {
            @Override
            public boolean accept(File pathname) {
                String n = pathname.getName().toLowerCase(Locale.US);
                return pathname.isFile() && n.endsWith(".png");
            }
        });
        List<Screenshot> out = new ArrayList<>();
        if (list != null) {
            for (File f : list) {
                out.add(Screenshot.of(f));
            }
        }
        out.sort(Comparator.comparing((Screenshot s) -> s.timestamp.orElse(LocalDateTime.MIN)).thenComparing(s -> s.file.getName()));
        return out;
    }

    private static final class Screenshot {
        final File file;
        final String baseName;
        final Optional<LocalDateTime> timestamp;

        private Screenshot(File file, String baseName, Optional<LocalDateTime> timestamp) {
            this.file = file;
            this.baseName = baseName;
            this.timestamp = timestamp;
        }

        static Screenshot of(File file) {
            String name = file.getName();
            String base = name;
            int dot = name.lastIndexOf('.');
            if (dot > 0) base = name.substring(0, dot);
            Optional<LocalDateTime> ts = parseTimestamp(name);
            return new Screenshot(file, base, ts);
        }
    }

    private static Optional<LocalDateTime> parseTimestamp(String fileName) {
        Matcher m = TS_PATTERN.matcher(fileName);
        if (!m.matches()) return Optional.empty();
        String ts = m.group(1) + "_" + m.group(2);
        try {
            return Optional.of(LocalDateTime.parse(ts, TS_FORMAT));
        } catch (Exception ignored) {
            return Optional.empty();
        }
    }

    private static final class Args {
        final String dirPath;
        final String outPath;
        final String cropsPath;

        private Args(String dirPath, String outPath, String cropsPath) {
            this.dirPath = dirPath;
            this.outPath = outPath;
            this.cropsPath = cropsPath;
        }

        static Args parse(String[] args) {
            String dir = "ErrorLog";
            String out = "ErrorLog/SCREENSHOT_REPORT.md";
            String crops = "ErrorLog/crops";

            for (int i = 0; i < args.length; i++) {
                String a = args[i];
                if ("--dir".equals(a) && i + 1 < args.length) {
                    dir = args[++i];
                } else if ("--out".equals(a) && i + 1 < args.length) {
                    out = args[++i];
                } else if ("--crops".equals(a) && i + 1 < args.length) {
                    crops = args[++i];
                }
            }
            return new Args(dir, out, crops);
        }
    }

    private static final class Roi {
        final int x;
        final int y;
        final int w;
        final int h;

        private Roi(int x, int y, int w, int h) {
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
        }

        static Roi of(BufferedImage img, double x0, double y0, double x1, double y1) {
            int x = (int) Math.round(img.getWidth() * x0);
            int y = (int) Math.round(img.getHeight() * y0);
            int w = (int) Math.round(img.getWidth() * (x1 - x0));
            int h = (int) Math.round(img.getHeight() * (y1 - y0));
            return new Roi(x, y, w, h);
        }
    }

    private static final class YellowDetect {
        final double yellowRatio;
        final int maxClusterPixels;

        private YellowDetect(double yellowRatio, int maxClusterPixels) {
            this.yellowRatio = yellowRatio;
            this.maxClusterPixels = maxClusterPixels;
        }

        static YellowDetect detect(BufferedImage img, Roi roi) {
            int x0 = clamp(roi.x, 0, img.getWidth() - 1);
            int y0 = clamp(roi.y, 0, img.getHeight() - 1);
            int x1 = clamp(roi.x + roi.w, 0, img.getWidth());
            int y1 = clamp(roi.y + roi.h, 0, img.getHeight());

            int w = Math.max(1, x1 - x0);
            int h = Math.max(1, y1 - y0);

            boolean[] mask = new boolean[w * h];
            int yellowCount = 0;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int rgb = img.getRGB(x0 + x, y0 + y);
                    int r = (rgb >> 16) & 0xFF;
                    int g = (rgb >> 8) & 0xFF;
                    int b = rgb & 0xFF;
                    boolean isYellow = r > 210 && g > 210 && b < 150 && ((r + g) / 2 - b) > 80;
                    if (isYellow) {
                        mask[y * w + x] = true;
                        yellowCount++;
                    }
                }
            }
            int maxCluster = maxClusterSize(mask, w, h);
            double ratio = yellowCount / (double) (w * h);
            return new YellowDetect(ratio, maxCluster);
        }

        private static int maxClusterSize(boolean[] mask, int w, int h) {
            boolean[] visited = new boolean[mask.length];
            int max = 0;
            int[] qx = new int[mask.length];
            int[] qy = new int[mask.length];

            for (int i = 0; i < mask.length; i++) {
                if (!mask[i] || visited[i]) continue;
                int sx = i % w;
                int sy = i / w;

                int head = 0;
                int tail = 0;
                qx[tail] = sx;
                qy[tail] = sy;
                tail++;
                visited[i] = true;

                int size = 0;
                while (head < tail) {
                    int x = qx[head];
                    int y = qy[head];
                    head++;
                    size++;

                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            int ni = ny * w + nx;
                            if (!mask[ni] || visited[ni]) continue;
                            visited[ni] = true;
                            qx[tail] = nx;
                            qy[tail] = ny;
                            tail++;
                        }
                    }
                }
                if (size > max) max = size;
            }
            return max;
        }
    }

    private static final class ImageStats {
        final double meanLuma;
        final double varLuma;
        final double darkRatio;

        private ImageStats(double meanLuma, double varLuma, double darkRatio) {
            this.meanLuma = meanLuma;
            this.varLuma = varLuma;
            this.darkRatio = darkRatio;
        }

        static ImageStats compute(BufferedImage img) {
            int w = img.getWidth();
            int h = img.getHeight();
            int step = Math.max(1, Math.min(w, h) / 320);

            double sum = 0;
            double sum2 = 0;
            long count = 0;
            long dark = 0;

            for (int y = 0; y < h; y += step) {
                for (int x = 0; x < w; x += step) {
                    int rgb = img.getRGB(x, y);
                    int r = (rgb >> 16) & 0xFF;
                    int g = (rgb >> 8) & 0xFF;
                    int b = rgb & 0xFF;
                    double yv = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                    sum += yv;
                    sum2 += yv * yv;
                    count++;
                    if (yv < 18.0) dark++;
                }
            }

            double mean = sum / Math.max(1, count);
            double var = sum2 / Math.max(1, count) - mean * mean;
            double darkRatio = dark / (double) Math.max(1, count);
            return new ImageStats(mean, Math.max(0, var), darkRatio);
        }
    }

    private static final class ImageDiff {
        static double compute(BufferedImage a, BufferedImage b) {
            int w = Math.min(a.getWidth(), b.getWidth());
            int h = Math.min(a.getHeight(), b.getHeight());
            int step = Math.max(1, Math.min(w, h) / 180);
            long sum = 0;
            long count = 0;

            for (int y = 0; y < h; y += step) {
                for (int x = 0; x < w; x += step) {
                    int ra = a.getRGB(x, y);
                    int rb = b.getRGB(x, y);
                    int dr = Math.abs(((ra >> 16) & 0xFF) - ((rb >> 16) & 0xFF));
                    int dg = Math.abs(((ra >> 8) & 0xFF) - ((rb >> 8) & 0xFF));
                    int db = Math.abs((ra & 0xFF) - (rb & 0xFF));
                    sum += dr + dg + db;
                    count += 3;
                }
            }
            return sum / (double) Math.max(1, count);
        }
    }

    private static final class MetricTriple {
        final OptionalDoubleValue fps;
        final OptionalDoubleValue cpuPercent;
        final OptionalDoubleValue memMb;
        final String raw;

        private MetricTriple(OptionalDoubleValue fps, OptionalDoubleValue cpuPercent, OptionalDoubleValue memMb, String raw) {
            this.fps = fps;
            this.cpuPercent = cpuPercent;
            this.memMb = memMb;
            this.raw = raw;
        }

        static MetricTriple empty(String raw) {
            return new MetricTriple(OptionalDoubleValue.empty(), OptionalDoubleValue.empty(), OptionalDoubleValue.empty(), raw);
        }

        boolean anyPresent() {
            return fps.present || cpuPercent.present || memMb.present;
        }

        String toShortString() {
            String f = fps.present ? fmt1(fps.value) : "--";
            String c = cpuPercent.present ? fmt1(cpuPercent.value) : "--";
            String m = memMb.present ? fmt1(memMb.value) : "--";
            return f + "/" + c + "/" + m;
        }
    }

    private static final class OptionalDoubleValue {
        final boolean present;
        final double value;
        final double confidence;

        private OptionalDoubleValue(boolean present, double value, double confidence) {
            this.present = present;
            this.value = value;
            this.confidence = confidence;
        }

        static OptionalDoubleValue empty() {
            return new OptionalDoubleValue(false, Double.NaN, 0);
        }

        static OptionalDoubleValue of(double v, double conf) {
            return new OptionalDoubleValue(true, v, conf);
        }
    }

    private static String fmt1(double v) {
        return String.format(Locale.US, "%.1f", v);
    }

    private static final class MetricDiff {
        final OptionalDoubleValue fpsAbs;
        final OptionalDoubleValue cpuAbs;
        final OptionalDoubleValue memAbs;

        private MetricDiff(OptionalDoubleValue fpsAbs, OptionalDoubleValue cpuAbs, OptionalDoubleValue memAbs) {
            this.fpsAbs = fpsAbs;
            this.cpuAbs = cpuAbs;
            this.memAbs = memAbs;
        }

        static MetricDiff compute(MetricTriple a, MetricTriple b) {
            OptionalDoubleValue fps = absDiff(a.fps, b.fps);
            OptionalDoubleValue cpu = absDiff(a.cpuPercent, b.cpuPercent);
            OptionalDoubleValue mem = absDiff(a.memMb, b.memMb);
            return new MetricDiff(fps, cpu, mem);
        }

        private static OptionalDoubleValue absDiff(OptionalDoubleValue a, OptionalDoubleValue b) {
            if (!a.present || !b.present) return OptionalDoubleValue.empty();
            double conf = Math.min(a.confidence, b.confidence);
            return OptionalDoubleValue.of(Math.abs(a.value - b.value), conf);
        }

        String toShortString() {
            String f = fpsAbs.present ? fmt1(fpsAbs.value) : "--";
            String c = cpuAbs.present ? fmt1(cpuAbs.value) : "--";
            String m = memAbs.present ? fmt1(memAbs.value) : "--";
            return f + "/" + c + "/" + m;
        }
    }

    private static final class MetricsFromRoi {
        static MetricTriple extractStatusBar(BufferedImage img, Roi roi, PrototypeSet prototypeSet) {
            BufferedImage crop = crop(img, roi);
            BinaryImage green = BinaryImage.fromColorMask(crop, ColorMask.GREEN_TEXT);
            BinaryImage white = BinaryImage.fromColorMask(crop, ColorMask.WHITE_TEXT);

            List<TextBlob> greenStrings = Ocr.extractStrings(green, prototypeSet);
            List<TextBlob> whiteStrings = Ocr.extractStrings(white, prototypeSet);

            OptionalDoubleValue fps = pickNumberByRegion(greenStrings, crop.getWidth(), 0.00, 0.34, 0, 120, true);
            OptionalDoubleValue cpu = pickNumberByRegion(whiteStrings, crop.getWidth(), 0.34, 0.67, 0, 1000, false);
            OptionalDoubleValue mem = pickNumberByRegion(whiteStrings, crop.getWidth(), 0.67, 1.00, 0, 600000, false);

            String raw = "green=" + join(greenStrings) + ";white=" + join(whiteStrings);
            return new MetricTriple(fps, cpu, mem, raw);
        }

        static MetricTriple extractOverlay(BufferedImage img, Roi roi, PrototypeSet prototypeSet) {
            BufferedImage crop = crop(img, roi);
            BinaryImage green = BinaryImage.fromColorMask(crop, ColorMask.GREEN_TEXT);
            List<TextBlob> strings = Ocr.extractStrings(green, prototypeSet);
            OptionalDoubleValue fps = pickFirstFloat(strings, 0, 120, true);
            OptionalDoubleValue cpu = pickNumberNearSuffixPercent(strings, 0, 1000);
            OptionalDoubleValue mem = pickLastNumber(strings, 0, 600000);
            String raw = "green=" + join(strings);
            return new MetricTriple(fps, cpu, mem, raw);
        }

        private static OptionalDoubleValue pickFirstFloat(List<TextBlob> blobs, double lo, double hi, boolean allowFloat) {
            double bestConf = 0;
            double bestVal = Double.NaN;
            for (TextBlob b : blobs) {
                for (CandidateNumber c : b.numbers) {
                    if (!allowFloat && c.hasDot) continue;
                    if (c.value >= lo && c.value <= hi && c.confidence > bestConf) {
                        bestConf = c.confidence;
                        bestVal = c.value;
                    }
                }
            }
            if (bestConf < 0.55) return OptionalDoubleValue.empty();
            return OptionalDoubleValue.of(bestVal, bestConf);
        }

        private static OptionalDoubleValue pickLastNumber(List<TextBlob> blobs, double lo, double hi) {
            CandidateNumber best = null;
            for (TextBlob b : blobs) {
                for (CandidateNumber c : b.numbers) {
                    if (c.value < lo || c.value > hi) continue;
                    if (best == null || c.centerX > best.centerX) {
                        best = c;
                    }
                }
            }
            if (best == null || best.confidence < 0.55) return OptionalDoubleValue.empty();
            return OptionalDoubleValue.of(best.value, best.confidence);
        }

        private static OptionalDoubleValue pickNumberNearSuffixPercent(List<TextBlob> blobs, double lo, double hi) {
            CandidateNumber best = null;
            for (TextBlob b : blobs) {
                for (CandidateNumber c : b.numbers) {
                    if (!c.hasPercent) continue;
                    if (c.value < lo || c.value > hi) continue;
                    if (best == null || c.confidence > best.confidence) best = c;
                }
            }
            if (best == null || best.confidence < 0.55) return OptionalDoubleValue.empty();
            return OptionalDoubleValue.of(best.value, best.confidence);
        }

        private static OptionalDoubleValue pickNumberByRegion(List<TextBlob> blobs,
                                                             int width,
                                                             double x0,
                                                             double x1,
                                                             double lo,
                                                             double hi,
                                                             boolean allowFloat) {
            double left = width * x0;
            double right = width * x1;
            CandidateNumber best = null;
            for (TextBlob b : blobs) {
                for (CandidateNumber c : b.numbers) {
                    if (!allowFloat && c.hasDot) continue;
                    if (c.centerX < left || c.centerX > right) continue;
                    if (c.value < lo || c.value > hi) continue;
                    if (best == null || c.confidence > best.confidence) best = c;
                }
            }
            if (best == null || best.confidence < 0.55) return OptionalDoubleValue.empty();
            return OptionalDoubleValue.of(best.value, best.confidence);
        }

        private static String join(List<TextBlob> blobs) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < blobs.size(); i++) {
                if (i > 0) sb.append(" | ");
                sb.append(blobs.get(i).text);
            }
            return sb.toString();
        }
    }

    private enum ColorMask {
        GREEN_TEXT,
        WHITE_TEXT
    }

    private static final class BinaryImage {
        final int width;
        final int height;
        final boolean[] pixels;

        private BinaryImage(int width, int height, boolean[] pixels) {
            this.width = width;
            this.height = height;
            this.pixels = pixels;
        }

        static BinaryImage fromColorMask(BufferedImage img, ColorMask mask) {
            int w = img.getWidth();
            int h = img.getHeight();
            boolean[] bin = new boolean[w * h];
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int rgb = img.getRGB(x, y);
                    int r = (rgb >> 16) & 0xFF;
                    int g = (rgb >> 8) & 0xFF;
                    int b = rgb & 0xFF;
                    boolean on;
                    if (mask == ColorMask.GREEN_TEXT) {
                        on = g > 170 && r < 170 && b < 170 && g - Math.max(r, b) > 50;
                    } else {
                        on = r > 210 && g > 210 && b > 210;
                    }
                    bin[y * w + x] = on;
                }
            }
            bin = Morphology.close(bin, w, h, 1);
            return new BinaryImage(w, h, bin);
        }
    }

    private static final class Morphology {
        static boolean[] close(boolean[] src, int w, int h, int r) {
            return erode(dilate(src, w, h, r), w, h, r);
        }

        private static boolean[] dilate(boolean[] src, int w, int h, int r) {
            boolean[] out = new boolean[src.length];
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    boolean any = false;
                    for (int dy = -r; dy <= r && !any; dy++) {
                        for (int dx = -r; dx <= r && !any; dx++) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            if (src[ny * w + nx]) any = true;
                        }
                    }
                    out[y * w + x] = any;
                }
            }
            return out;
        }

        private static boolean[] erode(boolean[] src, int w, int h, int r) {
            boolean[] out = new boolean[src.length];
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    boolean all = true;
                    for (int dy = -r; dy <= r && all; dy++) {
                        for (int dx = -r; dx <= r && all; dx++) {
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
                                all = false;
                                break;
                            }
                            if (!src[ny * w + nx]) all = false;
                        }
                    }
                    out[y * w + x] = all;
                }
            }
            return out;
        }
    }

    private static final class TextBlob {
        final String text;
        final double confidence;
        final List<CandidateNumber> numbers;

        private TextBlob(String text, double confidence, List<CandidateNumber> numbers) {
            this.text = text;
            this.confidence = confidence;
            this.numbers = numbers;
        }
    }

    private static final class CandidateNumber {
        final double value;
        final double confidence;
        final double centerX;
        final boolean hasDot;
        final boolean hasPercent;

        private CandidateNumber(double value, double confidence, double centerX, boolean hasDot, boolean hasPercent) {
            this.value = value;
            this.confidence = confidence;
            this.centerX = centerX;
            this.hasDot = hasDot;
            this.hasPercent = hasPercent;
        }
    }

    private static final class Ocr {
        static List<TextBlob> extractStrings(BinaryImage img, PrototypeSet prototypeSet) {
            List<Box> boxes = ConnectedComponents.findBoxes(img);
            boxes.removeIf(b -> b.w < 6 || b.h < 10);
            boxes.sort(Comparator.comparingInt(b -> b.x));

            List<List<Box>> lines = groupByLines(boxes);
            List<TextBlob> out = new ArrayList<>();
            for (List<Box> line : lines) {
                line.sort(Comparator.comparingInt(b -> b.x));
                List<List<Box>> groups = groupIntoWords(line);
                for (List<Box> word : groups) {
                    StringBuilder sb = new StringBuilder();
                    double confSum = 0;
                    int confN = 0;
                    int left = Integer.MAX_VALUE;
                    int right = Integer.MIN_VALUE;

                    for (Box b : word) {
                        left = Math.min(left, b.x);
                        right = Math.max(right, b.x + b.w);
                        BufferedImage glyph = b.cropFrom(img);
                        Match m = prototypeSet.match(glyph);
                        if (m.confidence >= 0.55) {
                            sb.append(m.ch);
                            confSum += m.confidence;
                            confN++;
                        }
                    }

                    if (confN == 0) continue;
                    String txt = sb.toString();
                    double conf = confSum / confN;
                    List<CandidateNumber> nums = parseNumbersWithPositions(txt, (left + right) / 2.0, conf);
                    out.add(new TextBlob(txt, conf, nums));
                }
            }
            return out;
        }

        private static List<CandidateNumber> parseNumbersWithPositions(String s, double centerX, double conf) {
            List<CandidateNumber> out = new ArrayList<>();
            Matcher m = Pattern.compile("(\\d+(?:\\.\\d+)?)(%)?").matcher(s);
            while (m.find()) {
                try {
                    double v = Double.parseDouble(m.group(1));
                    boolean hasDot = m.group(1).contains(".");
                    boolean hasPercent = m.group(2) != null;
                    out.add(new CandidateNumber(v, conf, centerX, hasDot, hasPercent));
                } catch (Exception ignored) {
                }
            }
            return out;
        }

        private static List<List<Box>> groupByLines(List<Box> boxes) {
            List<List<Box>> lines = new ArrayList<>();
            for (Box b : boxes) {
                boolean placed = false;
                for (List<Box> line : lines) {
                    if (isSameLine(line.get(0), b)) {
                        line.add(b);
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    List<Box> line = new ArrayList<>();
                    line.add(b);
                    lines.add(line);
                }
            }
            return lines;
        }

        private static boolean isSameLine(Box a, Box b) {
            int ay0 = a.y;
            int ay1 = a.y + a.h;
            int by0 = b.y;
            int by1 = b.y + b.h;
            int overlap = Math.max(0, Math.min(ay1, by1) - Math.max(ay0, by0));
            int minH = Math.min(a.h, b.h);
            return overlap >= minH * 0.5;
        }

        private static List<List<Box>> groupIntoWords(List<Box> boxes) {
            List<List<Box>> words = new ArrayList<>();
            List<Box> current = new ArrayList<>();
            Box prev = null;
            for (Box b : boxes) {
                if (prev == null) {
                    current.add(b);
                } else {
                    int gap = b.x - (prev.x + prev.w);
                    if (gap > Math.max(6, prev.h / 2)) {
                        if (!current.isEmpty()) words.add(current);
                        current = new ArrayList<>();
                    }
                    current.add(b);
                }
                prev = b;
            }
            if (!current.isEmpty()) words.add(current);
            return words;
        }
    }

    private static final class ConnectedComponents {
        static List<Box> findBoxes(BinaryImage img) {
            int w = img.width;
            int h = img.height;
            boolean[] p = img.pixels;
            boolean[] vis = new boolean[p.length];
            int[] qx = new int[p.length];
            int[] qy = new int[p.length];

            List<Box> boxes = new ArrayList<>();
            for (int i = 0; i < p.length; i++) {
                if (!p[i] || vis[i]) continue;
                int sx = i % w;
                int sy = i / w;

                int head = 0;
                int tail = 0;
                qx[tail] = sx;
                qy[tail] = sy;
                tail++;
                vis[i] = true;

                int minX = sx, minY = sy, maxX = sx, maxY = sy;
                int pixels = 0;

                while (head < tail) {
                    int x = qx[head];
                    int y = qy[head];
                    head++;
                    pixels++;
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;

                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            int nx = x + dx;
                            int ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            int ni = ny * w + nx;
                            if (!p[ni] || vis[ni]) continue;
                            vis[ni] = true;
                            qx[tail] = nx;
                            qy[tail] = ny;
                            tail++;
                        }
                    }
                }

                Box b = new Box(img, minX, minY, maxX - minX + 1, maxY - minY + 1, pixels);
                boxes.add(b);
            }
            return boxes;
        }
    }

    private static final class Box {
        final BinaryImage src;
        final int x;
        final int y;
        final int w;
        final int h;
        final int pixels;

        private Box(BinaryImage src, int x, int y, int w, int h, int pixels) {
            this.src = src;
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
            this.pixels = pixels;
        }

        BufferedImage cropFrom(BinaryImage img) {
            BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_INT_RGB);
            for (int yy = 0; yy < h; yy++) {
                for (int xx = 0; xx < w; xx++) {
                    boolean on = img.pixels[(y + yy) * img.width + (x + xx)];
                    out.setRGB(xx, yy, on ? 0xFFFFFF : 0x000000);
                }
            }
            return out;
        }
    }

    private static final class PrototypeSet {
        final List<Prototype> prototypes;

        private PrototypeSet(List<Prototype> prototypes) {
            this.prototypes = prototypes;
        }

        static PrototypeSet createDefault() {
            List<Prototype> protos = new ArrayList<>();
            String[] fonts = new String[]{"SansSerif", "Dialog", "Monospaced"};
            int[] sizes = new int[]{22, 26, 30};
            char[] chars = new char[]{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '-', '%'};

            for (String fn : fonts) {
                for (int size : sizes) {
                    Font f = new Font(fn, Font.BOLD, size);
                    for (char ch : chars) {
                        Prototype p = Prototype.render(ch, f);
                        protos.add(p);
                    }
                }
            }
            return new PrototypeSet(protos);
        }

        Match match(BufferedImage glyph) {
            NormalizedGlyph g = NormalizedGlyph.from(glyph, 20, 30);
            Prototype best = null;
            double bestDist = Double.POSITIVE_INFINITY;
            for (Prototype p : prototypes) {
                double d = g.distanceTo(p.normalized);
                if (d < bestDist) {
                    bestDist = d;
                    best = p;
                }
            }
            double conf = best == null ? 0 : Math.max(0, 1.0 - bestDist);
            return new Match(best == null ? '?' : best.ch, conf);
        }
    }

    private static final class Match {
        final char ch;
        final double confidence;

        private Match(char ch, double confidence) {
            this.ch = ch;
            this.confidence = confidence;
        }
    }

    private static final class Prototype {
        final char ch;
        final NormalizedGlyph normalized;

        private Prototype(char ch, NormalizedGlyph normalized) {
            this.ch = ch;
            this.normalized = normalized;
        }

        static Prototype render(char ch, Font font) {
            BufferedImage img = new BufferedImage(64, 64, BufferedImage.TYPE_INT_RGB);
            Graphics2D g = img.createGraphics();
            g.setColor(Color.BLACK);
            g.fillRect(0, 0, img.getWidth(), img.getHeight());
            g.setFont(font);
            g.setColor(Color.WHITE);
            g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING, RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);
            g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
            FontMetrics fm = g.getFontMetrics();
            int x = 6;
            int y = 6 + fm.getAscent();
            g.drawString(String.valueOf(ch), x, y);
            g.dispose();
            NormalizedGlyph ng = NormalizedGlyph.from(img, 20, 30);
            return new Prototype(ch, ng);
        }
    }

    private static final class NormalizedGlyph {
        final int w;
        final int h;
        final boolean[] bits;

        private NormalizedGlyph(int w, int h, boolean[] bits) {
            this.w = w;
            this.h = h;
            this.bits = bits;
        }

        static NormalizedGlyph from(BufferedImage src, int w, int h) {
            BufferedImage bin = toBinary(src);
            Box2d box = tightBox(bin);
            if (box == null) {
                return new NormalizedGlyph(w, h, new boolean[w * h]);
            }
            BufferedImage cropped = bin.getSubimage(box.x, box.y, box.w, box.h);
            boolean[] out = new boolean[w * h];
            for (int yy = 0; yy < h; yy++) {
                for (int xx = 0; xx < w; xx++) {
                    int sx = (int) Math.round((xx + 0.5) * cropped.getWidth() / (double) w - 0.5);
                    int sy = (int) Math.round((yy + 0.5) * cropped.getHeight() / (double) h - 0.5);
                    sx = clamp(sx, 0, cropped.getWidth() - 1);
                    sy = clamp(sy, 0, cropped.getHeight() - 1);
                    int rgb = cropped.getRGB(sx, sy) & 0xFF;
                    out[yy * w + xx] = rgb > 127;
                }
            }
            return new NormalizedGlyph(w, h, out);
        }

        double distanceTo(NormalizedGlyph o) {
            int n = Math.min(bits.length, o.bits.length);
            int diff = 0;
            for (int i = 0; i < n; i++) {
                if (bits[i] != o.bits[i]) diff++;
            }
            return diff / (double) Math.max(1, n);
        }

        private static BufferedImage toBinary(BufferedImage src) {
            int w = src.getWidth();
            int h = src.getHeight();
            BufferedImage out = new BufferedImage(w, h, BufferedImage.TYPE_BYTE_GRAY);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int rgb = src.getRGB(x, y);
                    int r = (rgb >> 16) & 0xFF;
                    int g = (rgb >> 8) & 0xFF;
                    int b = rgb & 0xFF;
                    int v = (r + g + b) / 3;
                    out.setRGB(x, y, v > 127 ? 0xFFFFFF : 0x000000);
                }
            }
            return out;
        }

        private static Box2d tightBox(BufferedImage img) {
            int w = img.getWidth();
            int h = img.getHeight();
            int minX = w, minY = h, maxX = -1, maxY = -1;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int v = img.getRGB(x, y) & 0xFF;
                    if (v > 127) {
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (x > maxX) maxX = x;
                        if (y > maxY) maxY = y;
                    }
                }
            }
            if (maxX < minX || maxY < minY) return null;
            return new Box2d(minX, minY, maxX - minX + 1, maxY - minY + 1);
        }
    }

    private static final class Box2d {
        final int x;
        final int y;
        final int w;
        final int h;

        private Box2d(int x, int y, int w, int h) {
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
        }
    }

    private static final class FrameResult {
        final Screenshot screenshot;
        final int width;
        final int height;
        final boolean blackScreen;
        final boolean maybeFrozen;
        final double diffScore;
        final boolean systemReadyVisible;
        final double readyYellowRatio;
        final MetricTriple statusMetrics;
        final MetricTriple overlayMetrics;
        final MetricDiff metricDiff;
        final ImageStats imageStats;
        final File statusCropFile;
        final File overlayCropFile;

        private FrameResult(Screenshot screenshot,
                            int width,
                            int height,
                            boolean blackScreen,
                            boolean maybeFrozen,
                            double diffScore,
                            boolean systemReadyVisible,
                            double readyYellowRatio,
                            MetricTriple statusMetrics,
                            MetricTriple overlayMetrics,
                            MetricDiff metricDiff,
                            ImageStats imageStats,
                            File statusCropFile,
                            File overlayCropFile) {
            this.screenshot = screenshot;
            this.width = width;
            this.height = height;
            this.blackScreen = blackScreen;
            this.maybeFrozen = maybeFrozen;
            this.diffScore = diffScore;
            this.systemReadyVisible = systemReadyVisible;
            this.readyYellowRatio = readyYellowRatio;
            this.statusMetrics = statusMetrics;
            this.overlayMetrics = overlayMetrics;
            this.metricDiff = metricDiff;
            this.imageStats = imageStats;
            this.statusCropFile = statusCropFile;
            this.overlayCropFile = overlayCropFile;
        }

        static FrameResult unreadable(Screenshot sc) {
            return new FrameResult(
                    sc,
                    0,
                    0,
                    false,
                    false,
                    Double.NaN,
                    false,
                    0,
                    MetricTriple.empty("unreadable"),
                    MetricTriple.empty("unreadable"),
                    MetricDiff.compute(MetricTriple.empty(""), MetricTriple.empty("")),
                    new ImageStats(Double.NaN, Double.NaN, Double.NaN),
                    new File(""),
                    new File("")
            );
        }
    }

    private static final class AnalysisSummary {
        int blackCount = 0;
        int freezeCount = 0;
        int readyCount = 0;
        int metricsParseableStatus = 0;
        int metricsParseableOverlay = 0;

        final List<Double> fpsDiffs = new ArrayList<>();
        final List<Double> cpuDiffs = new ArrayList<>();
        final List<Double> memDiffs = new ArrayList<>();

        void accumulate(FrameResult r) {
            if (r.blackScreen) blackCount++;
            if (r.maybeFrozen) freezeCount++;
            if (r.systemReadyVisible) readyCount++;
            if (r.statusMetrics.anyPresent()) metricsParseableStatus++;
            if (r.overlayMetrics.anyPresent()) metricsParseableOverlay++;

            if (r.metricDiff.fpsAbs.present) fpsDiffs.add(r.metricDiff.fpsAbs.value);
            if (r.metricDiff.cpuAbs.present) cpuDiffs.add(r.metricDiff.cpuAbs.value);
            if (r.metricDiff.memAbs.present) memDiffs.add(r.metricDiff.memAbs.value);
        }

        String fpsDiffSummary() {
            return summarize(fpsDiffs, "FPS");
        }

        String cpuDiffSummary() {
            return summarize(cpuDiffs, "CPU");
        }

        String memDiffSummary() {
            return summarize(memDiffs, "MEM");
        }

        private static String summarize(List<Double> xs, String unit) {
            if (xs.isEmpty()) return "无可用样本";
            double min = xs.stream().min(Double::compare).orElse(0.0);
            double max = xs.stream().max(Double::compare).orElse(0.0);
            double mean = xs.stream().mapToDouble(x -> x).average().orElse(0.0);
            return String.format(Locale.US, "min=%.1f, max=%.1f, mean=%.1f (%s, n=%d)", min, max, mean, unit, xs.size());
        }
    }
}

