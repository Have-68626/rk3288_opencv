package com.example.rk3288_opencv;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * EnrollmentSession（任务 17）单元测试。
 *
 * <p>覆盖：状态机、逐条采样门槛的拒收路径、姿态配额、8 样本均值 + L2 归一、
 * 与现有模板的重复阻止、取消/完成后的向量清零、与
 * {@link AndroidEnrollmentRepository} 的提交对接，以及“不存照片/不写日志”的源码结构守卫。
 *
 * <p>测试不加载 native so：每帧 embedding 由纯 Java 构造的确定向量提供，指标（灰度统计、
 * Laplacian 方差、姿态）由 {@link EnrollmentSession.ImageMetrics} 直接计算，全部可在纯 JVM 运行。
 */
public class EnrollmentSessionTest {

    @Rule
    public TemporaryFolder tmp = new TemporaryFolder();

    private static final int MV = 7;
    private static final int TSV = 2;

    /**
     * 供 {@link #newRepo()} 注入到**本用例创建的 repo 实例**上的密钥提供者替身（内存 JCE 密钥）。
     * 此前是进程级静态注入，改造后经实例的 {@code setKeyProviderForTesting} 注入。
     */
    private FeatureTemplateEncryptedStore.PlatformKeyProvider provider;

    @Before
    public void setUp() throws Exception {
        provider = jceProvider();
    }

    // 无 @After（改造前它用于复位进程级静态 provider）：密钥提供者接缝已是**实例级**，注入只落在
    // 本用例创建的实例上 ⇒ 无静态状态可复位，跨实例泄漏在结构上不可能发生。

    // ---------- ① 状态机 ----------

    @Test
    public void idleRejectsFeedUntilStarted() {
        EnrollmentSession s = new EnrollmentSession("alice");
        assertEquals(EnrollmentSession.State.IDLE, s.state());

        EnrollmentSession.FeedResult r = s.feed(sample(base(), EnrollmentSession.Pose.FRONT));
        assertFalse(r.accepted);
        assertEquals(EnrollmentSession.Reason.NOT_COLLECTING, r.reason);

        assertTrue(s.start());
        assertEquals(EnrollmentSession.State.COLLECTING, s.state());
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
    }

    @Test
    public void feedAfterCompletionRejectedAndResetRestarts() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        fillAllEight(s, base());
        assertEquals(EnrollmentSession.State.COMPLETED, s.state());

        EnrollmentSession.FeedResult r = s.feed(sample(base(), EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.ALREADY_COMPLETED, r.reason);

        // 完成后仍有均值模板；复位后清空回到 IDLE，可重新开始。
        assertNotNull(s.meanTemplate());
        s.reset();
        assertEquals(EnrollmentSession.State.IDLE, s.state());
        assertNull(s.meanTemplate());
        assertEquals(0, s.retainedEmbeddingCount());
        assertTrue(s.start());
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
    }

    // ---------- ② 各类门槛拒收 ----------

    @Test
    public void rejectsNonSingleFaceAndLowScore() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        EnrollmentSession.FeedResult two = s.feed(valid(base(), EnrollmentSession.Pose.FRONT)
                .detection(0.9f, 2).build());
        assertEquals(EnrollmentSession.Reason.FACE_COUNT_INVALID, two.reason);

        EnrollmentSession.FeedResult zero = s.feed(valid(base(), EnrollmentSession.Pose.FRONT)
                .detection(0.9f, 0).build());
        assertEquals(EnrollmentSession.Reason.FACE_COUNT_INVALID, zero.reason);

        EnrollmentSession.FeedResult weak = s.feed(valid(base(), EnrollmentSession.Pose.FRONT)
                .detection(0.49f, 1).build());
        assertEquals(EnrollmentSession.Reason.LOW_DET_SCORE, weak.reason);
        assertEquals(0, s.acceptedCount());
    }

    @Test
    public void areaRatioGateAtTenPercentBoundary() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        // 面积占比 39*25/(100*100)=0.0975 < 10% -> 拒收
        EnrollmentSession.FeedResult small = s.feed(EnrollmentSession.FrameSample.builder(base())
                .box(0f, 0f, 39f, 25f).frame(100, 100).pose(EnrollmentSession.Pose.FRONT).build());
        assertEquals(EnrollmentSession.Reason.FACE_TOO_SMALL, small.reason);

        // 恰好 10%（40*25/10000=0.10）在门槛之上，接受。
        EnrollmentSession.FeedResult exact = s.feed(EnrollmentSession.FrameSample.builder(base())
                .box(0f, 0f, 40f, 25f).frame(100, 100).pose(EnrollmentSession.Pose.FRONT).build());
        assertTrue(exact.accepted);
    }

    @Test
    public void blurGateAtLaplacianBoundary() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        EnrollmentSession.FeedResult blurry = s.feed(
                valid(base(), EnrollmentSession.Pose.FRONT).quality(128f, 40f, 79.9f).build());
        assertEquals(EnrollmentSession.Reason.TOO_BLURRY, blurry.reason);

        EnrollmentSession.FeedResult sharp = s.feed(
                valid(base(), EnrollmentSession.Pose.FRONT).quality(128f, 40f, 80f).build());
        assertTrue(sharp.accepted);
    }

    @Test
    public void grayMeanGateRejectsOutOfRange() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        assertEquals(EnrollmentSession.Reason.GRAY_MEAN_OUT_OF_RANGE,
                s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(49.9f, 40f, 120f).build()).reason);
        assertEquals(EnrollmentSession.Reason.GRAY_MEAN_OUT_OF_RANGE,
                s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(205.1f, 40f, 120f).build()).reason);

        assertTrue(s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(50f, 40f, 120f).build()).accepted);
        assertTrue(s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(205f, 40f, 120f).build()).accepted);
        assertEquals(2, s.acceptedCount());
    }

    @Test
    public void contrastGateAtStdBoundary() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        assertEquals(EnrollmentSession.Reason.LOW_CONTRAST,
                s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(128f, 24.9f, 120f).build()).reason);
        assertTrue(s.feed(valid(base(), EnrollmentSession.Pose.FRONT).quality(128f, 25f, 120f).build()).accepted);
    }

    @Test
    public void rejectsMalformedSamples() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        // 维度不符
        assertEquals(EnrollmentSession.Reason.INVALID_SAMPLE,
                s.feed(valid(new float[3], EnrollmentSession.Pose.FRONT).build()).reason);

        // 非有限数
        float[] nan = base();
        nan[0] = Float.NaN;
        assertEquals(EnrollmentSession.Reason.INVALID_SAMPLE,
                s.feed(valid(nan, EnrollmentSession.Pose.FRONT).build()).reason);

        // 非法框
        assertEquals(EnrollmentSession.Reason.INVALID_SAMPLE,
                s.feed(EnrollmentSession.FrameSample.builder(base())
                        .box(50f, 50f, 10f, 10f).frame(100, 100)
                        .pose(EnrollmentSession.Pose.FRONT).build()).reason);

        // 非法帧尺寸
        assertEquals(EnrollmentSession.Reason.INVALID_SAMPLE,
                s.feed(EnrollmentSession.FrameSample.builder(base())
                        .box(0f, 0f, 80f, 50f).frame(0, 100)
                        .pose(EnrollmentSession.Pose.FRONT).build()).reason);

        // null
        assertEquals(EnrollmentSession.Reason.INVALID_SAMPLE, s.feed(null).reason);
        assertEquals(0, s.acceptedCount());
    }

    @Test
    public void poseUnknownWhenNeitherPoseNorLandmarksProvided() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        EnrollmentSession.FeedResult r = s.feed(EnrollmentSession.FrameSample.builder(base())
                .box(0f, 0f, 80f, 50f).frame(100, 100).build());
        assertEquals(EnrollmentSession.Reason.POSE_UNKNOWN, r.reason);
    }

    @Test
    public void landmarkDerivedPoseIsUsedWhenExplicitPoseAbsent() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        // 对称关键点 -> 正脸
        assertTrue(s.feed(EnrollmentSession.FrameSample.builder(base())
                .box(0f, 0f, 80f, 50f).frame(100, 100)
                .landmarks(symmetricLandmarks()).build()).accepted);

        // 鼻尖显著右移 -> 左脸（偏航代理 > 阈值）
        EnrollmentSession.FeedResult left = s.feed(EnrollmentSession.FrameSample.builder(base())
                .box(0f, 0f, 80f, 50f).frame(100, 100)
                .landmarks(shiftedLandmarks(30f)).build());
        assertTrue(left.accepted);
        assertEquals(EnrollmentSession.Pose.LEFT, left.pose);
    }

    // ---------- ③ 姿态配额 ----------

    @Test
    public void poseQuotaFrontFourLeftTwoRightTwo() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        float[] e = base();

        for (int i = 0; i < 4; i++) {
            assertTrue("第 " + (i + 1) + " 张正脸应接受",
                    s.feed(sample(e, EnrollmentSession.Pose.FRONT)).accepted);
        }
        EnrollmentSession.FeedResult fifthFront = s.feed(sample(e, EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.POSE_QUOTA_EXCEEDED, fifthFront.reason);
        assertEquals(4, s.acceptedCount());

        assertTrue(s.feed(sample(e, EnrollmentSession.Pose.LEFT)).accepted);
        assertTrue(s.feed(sample(e, EnrollmentSession.Pose.LEFT)).accepted);
        EnrollmentSession.FeedResult thirdLeft = s.feed(sample(e, EnrollmentSession.Pose.LEFT));
        assertEquals(EnrollmentSession.Reason.POSE_QUOTA_EXCEEDED, thirdLeft.reason);

        assertTrue(s.feed(sample(e, EnrollmentSession.Pose.RIGHT)).accepted);
        assertTrue(s.feed(sample(e, EnrollmentSession.Pose.RIGHT)).accepted);
        assertEquals(EnrollmentSession.State.COMPLETED, s.state());
        assertEquals(8, s.acceptedCount());

        EnrollmentSession.SessionStatus st = s.status();
        assertEquals(4, st.frontCount);
        assertEquals(2, st.leftCount);
        assertEquals(2, st.rightCount);
        assertEquals(0, st.remainingTotal);
    }

    // ---------- ④ 一致性 / 重复 ----------

    @Test
    public void rejectsSampleInconsistentWithAlreadyCollected() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);

        // 完全不同的随机向量与已收样本 cosine 接近 0，必须拒绝。
        EnrollmentSession.FeedResult diff = s.feed(sample(otherEmbedding(), EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.INCONSISTENT_IDENTITY, diff.reason);
        assertEquals(1, s.acceptedCount());
    }

    @Test
    public void rejectsDuplicateAgainstExistingTemplate() {
        float[] e = base();
        List<EnrollmentSession.NamedTemplate> existing = new ArrayList<>();
        existing.add(new EnrollmentSession.NamedTemplate("alice", e));

        EnrollmentSession s = new EnrollmentSession("alice2",
                EnrollmentSession.fromTemplates(existing));
        s.start();

        EnrollmentSession.FeedResult dup = s.feed(sample(e, EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.DUPLICATE_PERSON, dup.reason);
        assertEquals(0, s.acceptedCount());
    }

    @Test
    public void acceptsWhenExistingTemplateIsDissimilar() {
        List<EnrollmentSession.NamedTemplate> existing = new ArrayList<>();
        existing.add(new EnrollmentSession.NamedTemplate("bob", otherEmbedding()));

        EnrollmentSession s = new EnrollmentSession("alice",
                EnrollmentSession.fromTemplates(existing));
        s.start();
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
    }

    // ---------- ⑤ 8 样本均值 + L2 归一 ----------

    @Test
    public void eightSampleMeanIsL2Normalized() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();

        float[] ref = base();
        List<float[]> fed = new ArrayList<>();
        EnrollmentSession.Pose[] poses = {
                EnrollmentSession.Pose.FRONT, EnrollmentSession.Pose.FRONT,
                EnrollmentSession.Pose.FRONT, EnrollmentSession.Pose.FRONT,
                EnrollmentSession.Pose.LEFT, EnrollmentSession.Pose.LEFT,
                EnrollmentSession.Pose.RIGHT, EnrollmentSession.Pose.RIGHT};
        for (int i = 0; i < poses.length; i++) {
            float[] e = perturb(ref, 0.05f, 100 + i);
            fed.add(e);
            assertTrue("第 " + i + " 张应接受", s.feed(sample(e, poses[i])).accepted);
        }
        assertEquals(EnrollmentSession.State.COMPLETED, s.state());

        float[] mean = s.meanTemplate();
        assertNotNull(mean);
        assertEquals(EnrollmentSession.EMBEDDING_DIM, mean.length);
        // 独立算期望：逐维均值后 L2 归一。
        double[] sum = new double[EnrollmentSession.EMBEDDING_DIM];
        for (float[] e : fed) {
            for (int i = 0; i < e.length; i++) {
                sum[i] += e[i];
            }
        }
        double norm = 0;
        double[] avg = new double[EnrollmentSession.EMBEDDING_DIM];
        for (int i = 0; i < avg.length; i++) {
            avg[i] = sum[i] / fed.size();
            norm += avg[i] * avg[i];
        }
        norm = Math.sqrt(norm);
        double outNorm = 0;
        for (int i = 0; i < mean.length; i++) {
            assertEquals("第 " + i + " 维均值/归一不符", avg[i] / norm, mean[i], 1e-5);
            outNorm += (double) mean[i] * mean[i];
        }
        assertEquals("结果必须是单位向量", 1.0, Math.sqrt(outNorm), 1e-4);
        // 完成后不再保留 8 个中间向量，仅保留 1 个均值模板（均值模板也计入保留统计）。
        assertEquals(1, s.retainedEmbeddingCount());
    }

    // ---------- ⑥ 取消清理 ----------

    @Test
    public void cancelClearsAllRetainedVectorsAndBlocksFurtherFeeds() {
        EnrollmentSession s = new EnrollmentSession("alice", EnrollmentSession.fromTemplates(
                Arrays.asList(new EnrollmentSession.NamedTemplate("bob", otherEmbedding()))));
        s.start();
        assertTrue(s.retainedEmbeddingCount() > 0);

        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.LEFT)).accepted);

        s.cancel();
        assertEquals(EnrollmentSession.State.CANCELLED, s.state());
        assertEquals(0, s.acceptedCount());
        assertNull(s.meanTemplate());
        assertEquals("取消后不得残留任何 embedding（含现有模板快照）",
                0, s.retainedEmbeddingCount());

        EnrollmentSession.FeedResult r = s.feed(sample(base(), EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.NOT_COLLECTING, r.reason);
    }

    @Test
    public void cancelFromCompletedWipesMeanTemplate() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        fillAllEight(s, base());
        assertEquals(EnrollmentSession.State.COMPLETED, s.state());
        assertNotNull(s.meanTemplate());
        assertEquals("完成态必须把均值模板计入保留统计", 1, s.retainedEmbeddingCount());

        s.cancel();
        assertEquals(EnrollmentSession.State.CANCELLED, s.state());
        assertNull(s.meanTemplate());
        assertEquals("从完成态取消后不得残留均值模板", 0, s.retainedEmbeddingCount());
    }

    @Test
    public void resetFromCompletedWipesMeanTemplate() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        fillAllEight(s, base());
        assertNotNull(s.meanTemplate());

        s.reset();
        assertEquals(EnrollmentSession.State.IDLE, s.state());
        assertNull(s.meanTemplate());
        assertEquals("复位后不得残留均值模板", 0, s.retainedEmbeddingCount());
        assertTrue("复位后可重新开始", s.start());
        assertNull(s.meanTemplate());
    }

    @Test
    public void restartAfterCancelDoesNotRetainOldMeanTemplate() {
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        fillAllEight(s, base());
        s.cancel();
        assertTrue(s.start());
        assertEquals(EnrollmentSession.State.COLLECTING, s.state());
        assertNull(s.meanTemplate());
        assertEquals(0, s.retainedEmbeddingCount());
    }

    // ---------- ⑦ 与仓库对接 ----------

    @Test
    public void commitToRepositoryPersistsMeanTemplateAndBlocksDuplicatePerson() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();

        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        fillAllEight(s, base());
        assertEquals(EnrollmentSession.State.COMPLETED, s.state());

        AndroidEnrollmentRepository.Result added = s.commitTo(repo, MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.OK, added.status);
        assertNotNull(added.entry);
        assertEquals("alice", added.entry.userId);
        assertEquals(MV, added.entry.modelVersion);
        assertEquals(TSV, added.entry.templateSchemaVersion);

        // 落盘模板字节应等于均值模板的小端 float32。
        final byte[][] stored = new byte[1][];
        assertEquals(AndroidEnrollmentRepository.Status.OK, repo.readTemplate("alice",
                new AndroidEnrollmentRepository.TemplateConsumer() {
                    @Override
                    public void accept(byte[] plaintextTemplate) {
                        stored[0] = plaintextTemplate.clone();
                    }
                }).status);
        assertArrayEquals(s.meanTemplateLe(), stored[0]);
        assertArrayEquals(s.meanTemplate(), EnrollmentSession.decodeLe(stored[0]), 0f);

        // 第二个会话以仓库为现有模板来源，同一身份应被去重拒绝。
        EnrollmentSession again = new EnrollmentSession("alice2",
                EnrollmentSession.fromRepository(repo));
        again.start();
        EnrollmentSession.FeedResult dup = again.feed(
                sample(s.meanTemplate(), EnrollmentSession.Pose.FRONT));
        assertEquals(EnrollmentSession.Reason.DUPLICATE_PERSON, dup.reason);
    }

    // ---------- ⑦b 仓库不可用：无法判重时绝不放行（第二波审查 #4） ----------

    /**
     * list() 失败（注册表头损坏）时，现有模板来源必须上报失败，判定点拒绝样本，
     * 不得静默空遍历把「无法检查」当成「没有重复人员」。
     */
    @Test
    public void repositoryListFailureRejectsSampleWithRepoUnavailable() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        // 写入非法注册表头：list() 返回 CORRUPTED（非 OK）。
        Files.write(repo.registryFile().toPath(),
                "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));
        assertFalse("前置条件：注册表必须不可读", repo.list().isOk());

        EnrollmentSession s = new EnrollmentSession("alice", EnrollmentSession.fromRepository(repo));
        s.start();
        assertTrue("list 失败必须落定失败标记", s.isExistingLoadFailed());

        EnrollmentSession.FeedResult r = s.feed(sample(base(), EnrollmentSession.Pose.FRONT));
        assertFalse("仓库不可用不得放行注册", r.accepted);
        assertEquals(EnrollmentSession.Reason.REPO_UNAVAILABLE, r.reason);
        assertEquals(0, s.acceptedCount());
    }

    /**
     * 注册表有条目但单个模板 readTemplate 失败时，必须中止遍历并整体拒绝，
     * 不得跳过该模板后放行（否则本可判重的样本会被误收）。
     */
    @Test
    public void repositoryTemplateReadFailureRejectsSampleInsteadOfSkipping() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        assertTrue(repo.add("bob", EnrollmentSession.encodeLe(otherEmbedding()), MV, TSV).isOk());
        // 注册表仍有 bob 条目，但模板文件被删除 -> readTemplate 失败。
        assertTrue("前置条件：删除 bob 模板文件", repo.templateFileFor("bob").delete());
        assertTrue("前置条件：注册表本身可读", repo.list().isOk());

        EnrollmentSession s = new EnrollmentSession("alice", EnrollmentSession.fromRepository(repo));
        s.start();
        assertTrue("单模板读取失败必须落定失败标记", s.isExistingLoadFailed());

        EnrollmentSession.FeedResult r = s.feed(sample(base(), EnrollmentSession.Pose.FRONT));
        assertFalse("存在不可读模板时不得放行注册", r.accepted);
        assertEquals(EnrollmentSession.Reason.REPO_UNAVAILABLE, r.reason);
        assertEquals(0, s.acceptedCount());
    }

    /** 正常空库仍可正常注册，失败通道不误伤。 */
    @Test
    public void emptyRepositoryStillAllowsNormalEnrollment() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        EnrollmentSession s = new EnrollmentSession("alice", EnrollmentSession.fromRepository(repo));
        s.start();
        assertFalse("空库读取成功，不得置失败标记", s.isExistingLoadFailed());
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
    }

    /** 失败标记的生命周期：start() 时确定，cancel()/reset() 后清除。 */
    @Test
    public void cancelAndResetClearExistingLoadFailedFlag() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        Files.write(repo.registryFile().toPath(),
                "NOT_A_REGISTRY\n".getBytes(StandardCharsets.UTF_8));

        EnrollmentSession s = new EnrollmentSession("alice", EnrollmentSession.fromRepository(repo));
        s.start();
        assertTrue(s.isExistingLoadFailed());
        s.cancel();
        assertFalse("cancel 必须清除失败标记", s.isExistingLoadFailed());

        // 重新开始再次落定失败标记，reset 也必须清除。
        s.start();
        assertTrue(s.isExistingLoadFailed());
        s.reset();
        assertFalse("reset 必须清除失败标记", s.isExistingLoadFailed());

        // 修复仓库（移除损坏注册表 -> 空库）后重新开始，恢复为可校验并放行。
        assertTrue(repo.registryFile().delete());
        assertTrue(s.start());
        assertFalse(s.isExistingLoadFailed());
        assertTrue(s.feed(sample(base(), EnrollmentSession.Pose.FRONT)).accepted);
    }

    @Test
    public void commitBeforeCompletionIsRejectedWithoutWriting() throws Exception {
        AndroidEnrollmentRepository repo = newRepo();
        EnrollmentSession s = new EnrollmentSession("alice");
        s.start();
        s.feed(sample(base(), EnrollmentSession.Pose.FRONT));

        AndroidEnrollmentRepository.Result r = s.commitTo(repo, MV, TSV);
        assertEquals(AndroidEnrollmentRepository.Status.INVALID_ARGUMENT, r.status);
        assertTrue(repo.list().entries.isEmpty());
        assertFalse(repo.templateFileFor("alice").exists());
    }

    // ---------- ⑧ ImageMetrics 数值工具 ----------

    @Test
    public void cosineBasics() {
        float[] x = {1f, 0f};
        float[] y = {0f, 1f};
        float[] nx = {-1f, 0f};
        assertEquals(1f, EnrollmentSession.ImageMetrics.cosine(x, x), 1e-6);
        assertEquals(0f, EnrollmentSession.ImageMetrics.cosine(x, y), 1e-6);
        assertEquals(-1f, EnrollmentSession.ImageMetrics.cosine(x, nx), 1e-6);
        assertEquals(0f, EnrollmentSession.ImageMetrics.cosine(x, new float[]{1f}), 1e-6);
    }

    @Test
    public void l2NormalizeProducesUnitVector() {
        float[] out = EnrollmentSession.ImageMetrics.l2Normalize(new float[]{3f, 4f});
        assertArrayEquals(new float[]{0.6f, 0.8f}, out, 1e-6f);
        float[] zero = EnrollmentSession.ImageMetrics.l2Normalize(new float[]{0f, 0f});
        assertArrayEquals(new float[]{0f, 0f}, zero, 0f);
    }

    @Test
    public void grayStats() {
        byte[] gray = {(byte) 0, (byte) 100, (byte) 200};
        assertEquals(100f, EnrollmentSession.ImageMetrics.grayMean(gray), 1e-4);
        assertEquals((float) Math.sqrt(20000.0 / 3.0),
                EnrollmentSession.ImageMetrics.grayStd(gray), 1e-3);
    }

    @Test
    public void laplacianVarianceFlatIsZeroImpulseIsHigh() {
        int w = 5;
        int h = 5;
        byte[] flat = new byte[w * h];
        assertEquals(0f, EnrollmentSession.ImageMetrics.laplacianVariance(flat, w, h), 0f);

        byte[] impulse = new byte[w * h];
        impulse[2 * w + 2] = (byte) 100;
        assertTrue("脉冲响应方差应显著高于清晰度门槛",
                EnrollmentSession.ImageMetrics.laplacianVariance(impulse, w, h)
                        > EnrollmentSession.MIN_LAPLACIAN_VARIANCE);

        // 尺寸不符 -> 0（拒收），不得抛异常。
        assertEquals(0f, EnrollmentSession.ImageMetrics.laplacianVariance(flat, 4, 4), 0f);
    }

    @Test
    public void areaRatioBasics() {
        EnrollmentSession.Box box = new EnrollmentSession.Box(0f, 0f, 40f, 25f);
        assertEquals(0.10f, EnrollmentSession.ImageMetrics.areaRatio(box, 100, 100), 1e-6);
        assertEquals(0f, EnrollmentSession.ImageMetrics.areaRatio(box, 0, 100), 0f);
    }

    @Test
    public void classifyPoseFromLandmarks() {
        assertEquals(EnrollmentSession.Pose.FRONT,
                EnrollmentSession.ImageMetrics.classifyPose(symmetricLandmarks()));
        assertEquals(EnrollmentSession.Pose.LEFT,
                EnrollmentSession.ImageMetrics.classifyPose(shiftedLandmarks(30f)));
        assertEquals(EnrollmentSession.Pose.RIGHT,
                EnrollmentSession.ImageMetrics.classifyPose(shiftedLandmarks(-30f)));
        assertNull(EnrollmentSession.ImageMetrics.classifyPose(null));
        assertNull(EnrollmentSession.ImageMetrics.classifyPose(new float[]{1f, 2f}));
    }

    @Test
    public void encodeDecodeRoundTripIsBitExact() {
        float[] v = base();
        float[] back = EnrollmentSession.decodeLe(EnrollmentSession.encodeLe(v));
        assertNotNull(back);
        assertArrayEquals(v, back, 0f);
        assertNull("长度不符必须拒绝解码", EnrollmentSession.decodeLe(new byte[8]));
        assertNull(EnrollmentSession.decodeLe(null));
    }

    // ---------- ⑨ “不存照片/不写日志”源码结构守卫 ----------

    @Test
    public void sourceNeverStoresImagesOrLogs() throws IOException {
        File src = new File(findRepoRoot(),
                "src/java/com/example/rk3288_opencv/EnrollmentSession.java");
        assertTrue(src.isFile());
        String text = new String(Files.readAllBytes(src.toPath()), StandardCharsets.UTF_8);
        List<String> forbidden = Arrays.asList(
                "Bitmap", "BitmapFactory", "ImageReader", "FileOutputStream", "FileInputStream",
                "android.util.Log", "System.out", "System.err", "Base64", "android.graphics");
        for (String bad : forbidden) {
            assertFalse("EnrollmentSession 不得引入图像落盘/日志设施: " + bad,
                    text.contains(bad));
        }
    }

    /**
     * 模板红线结构守卫：已置 null 的数组内容无法被外部观察，故以源码结构锁定「释放前先清零」，
     * 防止后续把 wipe 退化成裸置 null。
     */
    @Test
    public void meanTemplateBuffersAreZeroFilledBeforeReleaseInSource() throws IOException {
        File src = new File(findRepoRoot(),
                "src/java/com/example/rk3288_opencv/EnrollmentSession.java");
        assertTrue(src.isFile());
        String text = new String(Files.readAllBytes(src.toPath()), StandardCharsets.UTF_8);
        assertTrue("释放均值模板前必须 Arrays.fill 擦除",
                text.contains("Arrays.fill(meanTemplate, 0f)"));
        assertTrue("归一化中间累加器用完必须擦除",
                text.contains("Arrays.fill(sum, 0f)"));
    }

    // ---------- 工具 ----------

    /** 构造并在**本实例**上注入测试用密钥提供者替身（避免 6 处构造点各自重复注入）。 */
    private AndroidEnrollmentRepository newRepo() throws Exception {
        AndroidEnrollmentRepository r = new AndroidEnrollmentRepository(null, tmp.newFolder());
        r.setKeyProviderForTesting(provider);
        return r;
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

    /** 默认合格的帧构造器：面积占比 0.4，指标均在上限内。 */
    private static EnrollmentSession.FrameSample.Builder valid(float[] emb,
                                                              EnrollmentSession.Pose pose) {
        return EnrollmentSession.FrameSample.builder(emb)
                .box(0f, 0f, 80f, 50f).frame(100, 100).pose(pose);
    }

    private static EnrollmentSession.FrameSample sample(float[] emb,
                                                        EnrollmentSession.Pose pose) {
        return valid(emb, pose).build();
    }

    /** 集满 8 个样本（4 正 / 2 左 / 2 右），全部使用同一向量（cos=1，自然一致）。 */
    private static void fillAllEight(EnrollmentSession s, float[] emb) {
        for (int i = 0; i < 4; i++) {
            assertTrue(s.feed(sample(emb, EnrollmentSession.Pose.FRONT)).accepted);
        }
        for (int i = 0; i < 2; i++) {
            assertTrue(s.feed(sample(emb, EnrollmentSession.Pose.LEFT)).accepted);
        }
        for (int i = 0; i < 2; i++) {
            assertTrue(s.feed(sample(emb, EnrollmentSession.Pose.RIGHT)).accepted);
        }
    }

    /** 512 维确定性单位向量。 */
    private static float[] base() {
        return deterministicEmbedding(20260722L);
    }

    /** 与 base 不相关的另一 512 维单位向量（cos 接近 0）。 */
    private static float[] otherEmbedding() {
        return deterministicEmbedding(99991L);
    }

    private static float[] deterministicEmbedding(long seed) {
        Random r = new Random(seed);
        float[] v = new float[EnrollmentSession.EMBEDDING_DIM];
        for (int i = 0; i < v.length; i++) {
            v[i] = r.nextFloat() - 0.5f;
        }
        return EnrollmentSession.ImageMetrics.l2Normalize(v);
    }

    /** 在单位基准向量上叠加小扰动，模拟同一人的不同帧。 */
    private static float[] perturb(float[] base, float eps, long seed) {
        Random r = new Random(seed);
        float[] v = base.clone();
        for (int i = 0; i < v.length; i++) {
            v[i] += eps * (r.nextFloat() - 0.5f);
        }
        return EnrollmentSession.ImageMetrics.l2Normalize(v);
    }

    /** 5 点关键点：双眼 (100,100)/(200,100)，鼻尖居中 (150,120)。 */
    private static float[] symmetricLandmarks() {
        return new float[]{100f, 100f, 200f, 100f, 150f, 120f, 130f, 160f, 170f, 160f};
    }

    /** 鼻尖水平偏移 dx（正数代表偏航代理 > 0 -> 判为 LEFT）。 */
    private static float[] shiftedLandmarks(float dx) {
        return new float[]{100f, 100f, 200f, 100f, 150f + dx, 120f, 130f, 160f, 170f, 160f};
    }

    private static FeatureTemplateEncryptedStore.PlatformKeyProvider jceProvider() throws Exception {
        KeyGenerator kg = KeyGenerator.getInstance("AES");
        kg.init(128);
        final SecretKey key = kg.generateKey();
        return new FeatureTemplateEncryptedStore.PlatformKeyProvider() {
            @Override
            public boolean isSupported() {
                return true;
            }

            @Override
            public SecretKey getOrCreateKeyV1() {
                return key;
            }
        };
    }
}
