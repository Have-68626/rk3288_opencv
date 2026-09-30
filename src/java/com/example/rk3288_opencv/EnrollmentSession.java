package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.VisibleForTesting;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * EnrollmentSession（任务 17）—— 八样本人脸注册会话。
 *
 * <h2>职责</h2>
 * 逐帧喂入单张已确认人脸的检测框、关键点与质量/姿态指标（不接收、不保存任何图像像素），
 * 执行采样门槛与姿态配额；集满 8 个合格样本后对 8 个 512 维 embedding 求均值并做 L2 归一，
 * 得到最终模板，可交由 {@link AndroidEnrollmentRepository#add} 加密落盘。
 *
 * <h2>状态机</h2>
 * <pre>
 *   IDLE --start()--> COLLECTING --第 8 个样本被接受--> COMPLETED
 *     ^                   |                                |
 *     |              cancel()                          cancel()
 *     |                   v                                v
 *     +---reset()---- CANCELLED <--------------------------+
 *     +---reset()---- COMPLETED / COLLECTING / CANCELLED --+
 * </pre>
 * <ul>
 *   <li>{@link State#IDLE}：未开始。{@link #feed} 一律拒绝（{@link Reason#NOT_COLLECTING}）。</li>
 *   <li>{@link State#COLLECTING}：采样中，逐帧接受/拒绝。</li>
 *   <li>{@link State#COMPLETED}：已集满 8 个样本并产出均值模板；再 {@link #feed} 返回
 *       {@link Reason#ALREADY_COMPLETED}。</li>
 *   <li>{@link State#CANCELLED}：已取消，内部 embedding 与样本统计全部清零。</li>
 * </ul>
 *
 * <h2>采样门槛（按检查顺序）</h2>
 * <ol>
 *   <li>单确认脸：{@code faceCount == 1}（否则 {@link Reason#FACE_COUNT_INVALID}）。</li>
 *   <li>检测置信度：{@code detScore >= 0.5}（{@link Reason#LOW_DET_SCORE}）。</li>
 *   <li>面积占比：人脸框面积 / 帧面积 {@code >= 10%}（{@link Reason#FACE_TOO_SMALL}）。</li>
 *   <li>清晰度：Laplacian 方差 {@code >= 80}（{@link Reason#TOO_BLURRY}）。</li>
 *   <li>灰度均值：{@code 50 <= grayMean <= 205}（{@link Reason#GRAY_MEAN_OUT_OF_RANGE}）。</li>
 *   <li>灰度标准差：{@code grayStd >= 25}（{@link Reason#LOW_CONTRAST}）。</li>
 *   <li>姿态可判定（否则 {@link Reason#POSE_UNKNOWN}）。</li>
 *   <li>姿态配额：正脸 4 / 左脸 2 / 右脸 2（{@link Reason#POSE_QUOTA_EXCEEDED}）。</li>
 *   <li>与已收样本一致性：与任一已收样本 cosine {@code >= 0.75}
 *       （低于则视为不同人/异常帧，{@link Reason#INCONSISTENT_IDENTITY}）。</li>
 *   <li>现有模板可校验：若开始会话时现有模板快照读取失败，**或枚举不完整（注册表存在被跳过并
 *       隔离的损坏行，其模板仍在磁盘）**（{@link Reason#REPO_UNAVAILABLE}），说明无法判断是否
 *       重复，直接拒绝本次样本，绝不按“库中无重复”放行。
 *       ⚠️ 该门的判据是 {@code AndroidEnrollmentRepository.ListResult#isEnumerationComplete()}，
 *       它排除的是**行级跳过**与整体不可读；从 {@code .bak} 恢复时枚举**仍可能缺行**
 *       （备份可能早于最近一次 add，新条目还留在未被消费的 {@code .tmp} 里）而该判据仍为 true。
 *       该缺口如实登记、未修，详见 {@code ListResult#isEnumerationComplete} 的 javadoc。</li>
 *   <li>与现有模板重复阻止：与任一已注册模板 cosine {@code >= 0.55} 即判定为重复人员
 *       （{@link Reason#DUPLICATE_PERSON}），避免同一人被重复注册。</li>
 * </ol>
 *
 * <h2>“不存照片”与最小保留</h2>
 * 本类<b>不持有任何图像</b>：帧样本只携带已由调用方测量好的数值指标与 512 维 embedding；
 * 输入框/关键点不会被保留。8 个样本集满后立即求均值、清空并重写已收向量，完成态仅保留
 * 一个 512 维均值模板；取消/复位同样清零。本类不调用任何日志接口，避免模板/图像泄漏。
 *
 * <h2>阈值来源</h2>
 * Laplacian 方差、灰度统计、面积占比、embedding 的 cosine/L2 归一由
 * {@link ImageMetrics} 提供可直接复用的静态实现；姿态默认由关键点偏航代理分类
 * （{@link ImageMetrics#classifyPose}），调用方也可直接给出 {@link Pose}。
 */
public final class EnrollmentSession {

    /** 单个 embedding 维度，与 {@code NativeFaceBridge.EMBEDDING_DIM} 保持一致。 */
    public static final int EMBEDDING_DIM = 512;

    /** 目标样本数（= 正4 + 左2 + 右2）。 */
    public static final int TARGET_SAMPLES = 8;

    /** 姿态配额：正脸。 */
    public static final int QUOTA_FRONT = 4;
    /** 姿态配额：左脸。 */
    public static final int QUOTA_LEFT = 2;
    /** 姿态配额：右脸。 */
    public static final int QUOTA_RIGHT = 2;

    /** 检测置信度下限。 */
    public static final float MIN_DET_SCORE = 0.5f;
    /** 人脸框面积占帧面积下限。 */
    public static final float MIN_AREA_RATIO = 0.10f;
    /** Laplacian 方差下限（清晰度）。 */
    public static final float MIN_LAPLACIAN_VARIANCE = 80f;
    /** 灰度均值下限 / 上限。 */
    public static final float MIN_GRAY_MEAN = 50f;
    public static final float MAX_GRAY_MEAN = 205f;
    /** 灰度标准差下限（对比度）。 */
    public static final float MIN_GRAY_STD = 25f;
    /** 新样本与已收样本 cosine 下限（低于即判定不同人）。 */
    public static final float MIN_SAMPLE_COSINE = 0.75f;
    /** 新样本与现有注册模板 cosine 上限（达到即判定重复人员）。 */
    public static final float DUPLICATE_TEMPLATE_COSINE = 0.55f;
    /** 关键点偏航代理阈值：|yaw| 超过该值判为左/右脸。 */
    public static final float POSE_YAW_THRESHOLD = 0.15f;

    /** 会话状态。 */
    public enum State {
        IDLE,
        COLLECTING,
        COMPLETED,
        CANCELLED
    }

    /** 姿态分类（左右为约定方向，见 {@link ImageMetrics#classifyPose}）。 */
    public enum Pose {
        FRONT,
        LEFT,
        RIGHT
    }

    /** 逐帧喂入结果原因。 */
    public enum Reason {
        ACCEPTED,
        /** 会话未处于 COLLECTING（未开始或已取消）。 */
        NOT_COLLECTING,
        /** 会话已完成，拒绝追加样本。 */
        ALREADY_COMPLETED,
        /** 入参非法（空、维度不符、非有限数、框/帧非法等）。 */
        INVALID_SAMPLE,
        FACE_COUNT_INVALID,
        LOW_DET_SCORE,
        FACE_TOO_SMALL,
        TOO_BLURRY,
        GRAY_MEAN_OUT_OF_RANGE,
        LOW_CONTRAST,
        POSE_UNKNOWN,
        POSE_QUOTA_EXCEEDED,
        INCONSISTENT_IDENTITY,
        DUPLICATE_PERSON,
        /** 现有模板仓库不可读**或枚举不完整**，无法校验是否重复；为防重复注册而拒绝本次样本。 */
        REPO_UNAVAILABLE
    }

    /*
     * 可用性收敛（REPO_UNAVAILABLE 的后果，如实记录）：REPO_UNAVAILABLE 不是一次性拒绝，而是
     * **每一帧都拒绝**——失败标记在 start() 快照阶段即落定（见 existingLoadFailed），判定点在
     * feed 的 6.5) 步对每个样本前置返回 false ⇒ 会话永远集不满 8 样本、commitTo 永不执行 ⇒
     * 注册表只要存在**一行**坏行，经 UI 的「注册」就完全不可用（清除坏行只能靠一次成功的
     * add/remove，而 add 的自愈路**仍**被本门切断）。
     * 注册表「只剩坏行」时 list().entries 为空 ⇒ 人员列表里没有可删条目这一死角，已由仓库层
     * AndroidEnrollmentRepository.quarantineCorruptRegistry() 提供**应用内恢复出口**
     * （MainActivity.promptRegistryRecovery 接线：改名旁置 + 建空表）。⚠️ 须区分「**有出口**」与
     * 「**自动修复**」：该出口只把损坏文件改名旁置（不还原任何损坏条目）、新建一张空表（不含任何
     * 已注册人员），原文件仅供手工取回；add 的自愈仍不可达，注册只能经该出口或 remove 推进。
     */

    /** 人脸检测框（帧像素坐标，右/下为开区间）。 */
    public static final class Box {
        public final float left;
        public final float top;
        public final float right;
        public final float bottom;

        public Box(float left, float top, float right, float bottom) {
            this.left = left;
            this.top = top;
            this.right = right;
            this.bottom = bottom;
        }

        public float width() {
            return right - left;
        }

        public float height() {
            return bottom - top;
        }

        public float area() {
            return Math.max(0f, width()) * Math.max(0f, height());
        }

        /** 框坐标必须有限且右下严格大于左上。 */
        public boolean isValid() {
            return isFinite(left) && isFinite(top)
                    && isFinite(right) && isFinite(bottom)
                    && right > left && bottom > top;
        }
    }

    /** 单帧输入：检测框 + 关键点 + 灰度/清晰度/姿态指标 + 该帧 embedding。不含图像。 */
    public static final class FrameSample {
        @NonNull public final float[] embedding;
        @NonNull public final Box box;
        public final int frameWidth;
        public final int frameHeight;
        public final float grayMean;
        public final float grayStd;
        public final float laplacianVariance;
        public final float detScore;
        public final int faceCount;
        /** 显式姿态；为 null 时由 {@link #landmarks} 推导。 */
        @Nullable public final Pose pose;
        /** 5 点关键点（10 个 float，可为 null）；仅用于姿态推导，不被保留。 */
        @Nullable public final float[] landmarks;

        FrameSample(@NonNull float[] embedding, @NonNull Box box, int frameWidth, int frameHeight,
                    float grayMean, float grayStd, float laplacianVariance, float detScore,
                    int faceCount, @Nullable Pose pose, @Nullable float[] landmarks) {
            this.embedding = embedding;
            this.box = box;
            this.frameWidth = frameWidth;
            this.frameHeight = frameHeight;
            this.grayMean = grayMean;
            this.grayStd = grayStd;
            this.laplacianVariance = laplacianVariance;
            this.detScore = detScore;
            this.faceCount = faceCount;
            this.pose = pose;
            this.landmarks = landmarks;
        }

        @NonNull
        public static Builder builder(@NonNull float[] embedding) {
            return new Builder(embedding);
        }

        /**
         * 构造辅助：质量/检测字段默认落在合格区间，调用方只需补上检测框与帧尺寸，
         * 并按需覆盖关心的字段。
         */
        public static final class Builder {
            private final float[] embedding;
            private Box box = new Box(0f, 0f, 0f, 0f);
            private int frameWidth = 0;
            private int frameHeight = 0;
            private float grayMean = 128f;
            private float grayStd = 40f;
            private float laplacianVariance = 120f;
            private float detScore = 0.9f;
            private int faceCount = 1;
            private Pose pose;
            private float[] landmarks;

            Builder(@NonNull float[] embedding) {
                this.embedding = embedding;
            }

            @NonNull
            public Builder box(float left, float top, float right, float bottom) {
                this.box = new Box(left, top, right, bottom);
                return this;
            }

            @NonNull
            public Builder frame(int width, int height) {
                this.frameWidth = width;
                this.frameHeight = height;
                return this;
            }

            @NonNull
            public Builder quality(float grayMean, float grayStd, float laplacianVariance) {
                this.grayMean = grayMean;
                this.grayStd = grayStd;
                this.laplacianVariance = laplacianVariance;
                return this;
            }

            @NonNull
            public Builder detection(float detScore, int faceCount) {
                this.detScore = detScore;
                this.faceCount = faceCount;
                return this;
            }

            @NonNull
            public Builder pose(@Nullable Pose pose) {
                this.pose = pose;
                return this;
            }

            @NonNull
            public Builder landmarks(@Nullable float[] landmarks) {
                this.landmarks = (landmarks == null) ? null : landmarks.clone();
                return this;
            }

            @NonNull
            public FrameSample build() {
                return new FrameSample(embedding.clone(), box, frameWidth, frameHeight,
                        grayMean, grayStd, laplacianVariance, detScore, faceCount,
                        pose, (landmarks == null) ? null : landmarks.clone());
            }
        }
    }

    /** 单帧喂入结果。 */
    public static final class FeedResult {
        public final boolean accepted;
        @NonNull public final Reason reason;
        @Nullable public final Pose pose;
        public final int acceptedCount;
        @NonNull public final State state;
        /** 中文说明；只描述错误类别，绝不含模板/向量内容。 */
        @NonNull public final String message;

        FeedResult(boolean accepted, @NonNull Reason reason, @Nullable Pose pose,
                   int acceptedCount, @NonNull State state, @NonNull String message) {
            this.accepted = accepted;
            this.reason = reason;
            this.pose = pose;
            this.acceptedCount = acceptedCount;
            this.state = state;
            this.message = message;
        }
    }

    /** 会话进度快照（不含任何向量）。 */
    public static final class SessionStatus {
        @NonNull public final State state;
        public final int acceptedCount;
        public final int target;
        public final int remainingTotal;
        public final int frontCount;
        public final int leftCount;
        public final int rightCount;

        SessionStatus(@NonNull State state, int acceptedCount, int target, int remainingTotal,
                      int frontCount, int leftCount, int rightCount) {
            this.state = state;
            this.acceptedCount = acceptedCount;
            this.target = target;
            this.remainingTotal = remainingTotal;
            this.frontCount = frontCount;
            this.leftCount = leftCount;
            this.rightCount = rightCount;
        }
    }

    /** 现有模板来源：遍历已注册人员的已解码 embedding，用于重复阻止。 */
    public interface ExistingTemplateSource {
        /**
         * 遍历已注册人员的已解码 embedding；失败通道由返回值承载。
         *
         * <p>返回 {@code true} 表示已完整、无错误地遍历全部现有模板；返回 {@code false}
         * 表示来源读取失败、遍历结果不完整（可能漏报重复人员）。调用方<b>必须</b>把 false
         * 当作“无法校验”，绝不可当作“库中没有模板”放行注册。
         *
         * @return 是否完整遍历现有模板
         */
        boolean forEach(@NonNull ExistingTemplateConsumer consumer);
    }

    /** 现有模板消费回调；{@code embedding} 仅在该回调内有效，调用方不得长期持有。 */
    public interface ExistingTemplateConsumer {
        void accept(@NonNull String userId, @NonNull float[] embedding);
    }

    /** 带用户标识的已解码模板（测试与内存来源用）。 */
    public static final class NamedTemplate {
        @NonNull public final String userId;
        @NonNull public final float[] embedding;

        public NamedTemplate(@NonNull String userId, @NonNull float[] embedding) {
            this.userId = userId;
            this.embedding = embedding.clone();
        }
    }

    @NonNull private final String userId;
    @Nullable private final ExistingTemplateSource existingSource;

    @NonNull private State state = State.IDLE;
    /** 已接受样本的 embedding；完成/取消/复位后被清零并丢弃。 */
    @NonNull private final List<float[]> accepted = new ArrayList<>();
    /** 开始会话时对现有模板的快照；完成/取消/复位后被丢弃。 */
    @NonNull private List<float[]> existingSnapshot = Collections.emptyList();
    /**
     * 现有模板快照是否读取失败（不完整）。为 true 时本会话无法可靠判重，
     * 必须拒绝样本而非按“无重复”放行；开始/快照构建时确定，取消/复位后清除。
     */
    private boolean existingLoadFailed;
    @NonNull private final int[] poseCounts = new int[Pose.values().length];
    private int acceptedTotal;
    /** 完成态保留的唯一均值模板（已 L2 归一）。 */
    @Nullable private float[] meanTemplate;

    public EnrollmentSession(@NonNull String userId) {
        this(userId, null);
    }

    /**
     * @param userId         注册人员标识
     * @param existingSource 现有模板来源，用于重复阻止；可为 null（表示库中无既有模板）
     */
    public EnrollmentSession(@NonNull String userId, @Nullable ExistingTemplateSource existingSource) {
        this.userId = userId;
        this.existingSource = existingSource;
    }

    /** 由仓库适配现有模板来源：逐个读取并解密模板，按 {@link #decodeLe} 解码后参与比对。 */
    @NonNull
    public static ExistingTemplateSource fromRepository(@NonNull final AndroidEnrollmentRepository repo) {
        return new ExistingTemplateSource() {
            @Override
            public boolean forEach(@NonNull final ExistingTemplateConsumer consumer) {
                AndroidEnrollmentRepository.ListResult list = repo.list();
                if (!list.isEnumerationComplete()) {
                    // 清单整体不可读（!isOk()），或注册表存在**被跳过并隔离的损坏行**
                    // （skippedLineCount > 0）：后者虽 status==OK，但被跳过者不在枚举内、其模板仍在磁盘
                    // ⇒ 这些人可被重复注册。故与「整体不可读」同口径拒收，绝不把不完整枚举当成
                    // “库中无重复”。
                    // ⚠️ 判据刻意用 isEnumerationComplete()（= isOk() && skippedLineCount == 0）而非
                    //     !authoritative：后者在「无注册表文件」时也为 false（无法区分全新安装与索引丢失，
                    //     见 loadRegistry），用它会把全新安装误判为不可注册。
                    // ⚠️ 该判据**不是**「枚举绝无缺行」的强保证：从 .bak 恢复时备份可能早于最近一次
                    //     add（新条目还留在未被消费的 .tmp 里），此时枚举确实缺行而本判据仍为 true。
                    //     该缺口见 ListResult#isEnumerationComplete 的 javadoc，此处如实登记、未修。
                    return false;
                }
                for (final AndroidEnrollmentRepository.Entry entry : list.entries) {
                    final float[][] decoded = new float[1][];
                    try {
                        AndroidEnrollmentRepository.Result rr = repo.readTemplate(
                                entry.userId, new AndroidEnrollmentRepository.TemplateConsumer() {
                                    @Override
                                    public void accept(@NonNull byte[] plaintextTemplate) {
                                        float[] emb = decodeLe(plaintextTemplate);
                                        if (emb != null) {
                                            decoded[0] = emb;
                                        }
                                    }
                                });
                        if (!rr.isOk() || decoded[0] == null) {
                            // 任一模板读取/解码失败即中止遍历并报告失败：跳过该模板会让本可
                            // 判重的样本被误放行，必须在判定点整体拒绝本次注册。
                            return false;   // finally 仍会清零
                        }
                        consumer.accept(entry.userId, decoded[0]);
                    } finally {
                        // ★ 2026-09-28 修（外部审查 [P2]「人脸特征明文副本未及时清零」）：
                        //   明文副本**用完即擦**。放在 finally 里是刻意的 —— 上面的 `return false`
                        //   与 `consumer.accept` 的正常返回都必须覆盖。擦除放在 accept **之后**
                        //   是安全的：回调契约是「数组仅在回调内有效」，调用方在此之后不得再使用它。
                        if (decoded[0] != null) {
                            Arrays.fill(decoded[0], 0.0f);
                        }
                    }
                }
                return true;
            }
        };
    }

    /** 由内存模板列表构造来源（测试/离线场景）。 */
    @NonNull
    public static ExistingTemplateSource fromTemplates(@NonNull final List<NamedTemplate> templates) {
        final List<NamedTemplate> snapshot = new ArrayList<>(templates);
        return new ExistingTemplateSource() {
            @Override
            public boolean forEach(@NonNull ExistingTemplateConsumer consumer) {
                for (NamedTemplate t : snapshot) {
                    consumer.accept(t.userId, t.embedding);
                }
                return true;
            }
        };
    }

    // ---------------- 状态机 ----------------

    /**
     * 开始采样：IDLE 或 CANCELLED 可开始；COLLECTING 重复调用为幂等；COMPLETED 需先
     * {@link #reset()}。开始时对现有模板做一次快照。
     *
     * @return 进入/保持 COLLECTING 返回 true；已完成需复位返回 false
     */
    public synchronized boolean start() {
        if (state == State.COLLECTING) {
            return true;
        }
        if (state == State.COMPLETED) {
            return false;
        }
        wipeSamples();
        wipeExistingSnapshot();
        snapshotExisting();
        acceptedTotal = 0;
        Arrays.fill(poseCounts, 0);
        wipeMeanTemplate();
        state = State.COLLECTING;
        return true;
    }

    /** 取消并清零本会话全部向量与统计；已取消/空闲时无副作用。 */
    public synchronized void cancel() {
        if (state == State.IDLE) {
            return;
        }
        wipeSamples();
        wipeExistingSnapshot();
        Arrays.fill(poseCounts, 0);
        acceptedTotal = 0;
        wipeMeanTemplate();
        state = State.CANCELLED;
    }

    /** 复位到 IDLE 并清零全部向量与统计。 */
    public synchronized void reset() {
        wipeSamples();
        wipeExistingSnapshot();
        Arrays.fill(poseCounts, 0);
        acceptedTotal = 0;
        wipeMeanTemplate();
        state = State.IDLE;
    }

    /**
     * 喂入一帧。仅 COLLECTING 状态接受；逐条执行采样门槛与姿态配额。
     */
    @NonNull
    public synchronized FeedResult feed(@Nullable FrameSample sample) {
        if (state == State.COMPLETED) {
            return result(false, Reason.ALREADY_COMPLETED, null, "会话已完成");
        }
        if (state != State.COLLECTING) {
            return result(false, Reason.NOT_COLLECTING, null, "会话未开始或已取消");
        }
        if (!isSampleStructurallyValid(sample)) {
            return result(false, Reason.INVALID_SAMPLE, null, "帧样本非法");
        }
        assert sample != null;

        // 1) 单确认脸 + 置信度
        if (sample.faceCount != 1) {
            return result(false, Reason.FACE_COUNT_INVALID, null, "需且仅需一张确认人脸");
        }
        if (sample.detScore < MIN_DET_SCORE) {
            return result(false, Reason.LOW_DET_SCORE, null, "检测置信度不足");
        }

        // 2) 面积占比
        float ratio = ImageMetrics.areaRatio(sample.box, sample.frameWidth, sample.frameHeight);
        if (ratio < MIN_AREA_RATIO) {
            return result(false, Reason.FACE_TOO_SMALL, null, "人脸区域过小");
        }

        // 3) 清晰度
        if (sample.laplacianVariance < MIN_LAPLACIAN_VARIANCE) {
            return result(false, Reason.TOO_BLURRY, null, "图像清晰度不足");
        }

        // 4) 灰度均值 / 标准差
        if (sample.grayMean < MIN_GRAY_MEAN || sample.grayMean > MAX_GRAY_MEAN) {
            return result(false, Reason.GRAY_MEAN_OUT_OF_RANGE, null, "灰度均值超出范围");
        }
        if (sample.grayStd < MIN_GRAY_STD) {
            return result(false, Reason.LOW_CONTRAST, null, "灰度对比度不足");
        }

        // 5) 姿态判定与配额
        Pose pose = (sample.pose != null) ? sample.pose : ImageMetrics.classifyPose(sample.landmarks);
        if (pose == null) {
            return result(false, Reason.POSE_UNKNOWN, null, "无法判定姿态");
        }
        if (poseCounts[pose.ordinal()] >= quotaFor(pose)) {
            return result(false, Reason.POSE_QUOTA_EXCEEDED, pose, "该姿态配额已满");
        }

        // 6) 与已收样本一致性（不同人/跳变帧不得混入）
        for (float[] prior : accepted) {
            if (ImageMetrics.cosine(prior, sample.embedding) < MIN_SAMPLE_COSINE) {
                return result(false, Reason.INCONSISTENT_IDENTITY, pose, "与已采集样本差异过大");
            }
        }

        // 6.5) 现有模板未能完整读取（仓库不可用）：无法判重时宁可拒绝本次样本，
        //      绝不可把“无法检查”当作“没有重复人员”而放行注册。
        if (existingLoadFailed) {
            return result(false, Reason.REPO_UNAVAILABLE, pose,
                    "无法校验现有模板，已阻止注册以免重复");
        }

        // 7) 与现有注册模板重复阻止
        for (float[] existing : existingSnapshot) {
            if (ImageMetrics.cosine(existing, sample.embedding) >= DUPLICATE_TEMPLATE_COSINE) {
                return result(false, Reason.DUPLICATE_PERSON, pose, "该人员已在库中，疑似重复注册");
            }
        }

        // 接受：克隆入模，绝不保留调用方数组引用。
        accepted.add(sample.embedding.clone());
        poseCounts[pose.ordinal()]++;
        acceptedTotal++;
        if (acceptedTotal == TARGET_SAMPLES) {
            finalizeMeanTemplate();
            return result(true, Reason.ACCEPTED, pose, "已集满样本，模板已生成");
        }
        return result(true, Reason.ACCEPTED, pose, "");
    }

    // ---------------- 结果访问 ----------------

    @NonNull
    public synchronized State state() {
        return state;
    }

    @NonNull
    public synchronized String userId() {
        return userId;
    }

    public synchronized int acceptedCount() {
        return acceptedTotal;
    }

    public synchronized boolean isComplete() {
        return state == State.COMPLETED;
    }

    public synchronized int poseCount(@NonNull Pose pose) {
        return poseCounts[pose.ordinal()];
    }

    /** 进度快照。 */
    @NonNull
    public synchronized SessionStatus status() {
        return new SessionStatus(state, acceptedTotal, TARGET_SAMPLES,
                Math.max(0, TARGET_SAMPLES - acceptedTotal),
                poseCounts[Pose.FRONT.ordinal()],
                poseCounts[Pose.LEFT.ordinal()],
                poseCounts[Pose.RIGHT.ordinal()]);
    }

    /** 完成态的均值模板（L2 归一后副本）；未完成返回 null。 */
    @Nullable
    public synchronized float[] meanTemplate() {
        return (meanTemplate == null) ? null : meanTemplate.clone();
    }

    /** 完成态均值模板的小端 float32 字节（512*4 字节）；未完成返回 null。 */
    @Nullable
    public synchronized byte[] meanTemplateLe() {
        return (meanTemplate == null) ? null : encodeLe(meanTemplate);
    }

    /**
     * 将完成态模板提交到仓库（加密落盘）。未完成时返回
     * {@link AndroidEnrollmentRepository.Status#INVALID_ARGUMENT}，不写任何数据。
     */
    @NonNull
    public synchronized AndroidEnrollmentRepository.Result commitTo(
            @NonNull AndroidEnrollmentRepository repo, int modelVersion, int templateSchemaVersion) {
        if (state != State.COMPLETED || meanTemplate == null) {
            return new AndroidEnrollmentRepository.Result(
                    AndroidEnrollmentRepository.Status.INVALID_ARGUMENT, null, "会话未完成，无法提交模板");
        }
        return repo.add(userId, encodeLe(meanTemplate), modelVersion, templateSchemaVersion);
    }

    /**
     * 当前内存中保留的 embedding 条数（含现有模板快照与完成态均值模板）；仅供测试断言清零。
     *
     * <p>把均值模板一并计入，才能让取消/复位后的「清零」断言覆盖完成态释放路径。
     */
    @VisibleForTesting
    synchronized int retainedEmbeddingCount() {
        return accepted.size() + existingSnapshot.size() + (meanTemplate != null ? 1 : 0);
    }

    /** 现有模板快照是否读取失败**或枚举不完整**；仅供测试断言失败标记在 start/cancel/reset 间的生命周期。 */
    @VisibleForTesting
    synchronized boolean isExistingLoadFailed() {
        return existingLoadFailed;
    }

    // ---------------- 内部实现 ----------------

    private void snapshotExisting() {
        if (existingSource == null) {
            existingSnapshot = Collections.emptyList();
            // 无来源即“库中确无既有模板”，是确定的空集而非读取失败。
            existingLoadFailed = false;
            return;
        }
        final List<float[]> out = new ArrayList<>();
        boolean loaded = existingSource.forEach(new ExistingTemplateConsumer() {
            @Override
            public void accept(@NonNull String ignoredUserId, @NonNull float[] embedding) {
                if (embedding.length == EMBEDDING_DIM && ImageMetrics.allFinite(embedding)) {
                    out.add(embedding.clone());
                }
            }
        });
        existingSnapshot = out;
        // 保留既有快照语义（已收下的照收），但失败标记必须落定：判定点凭它前置拒绝，
        // 避免不完整快照被当成“库中无重复”。
        existingLoadFailed = !loaded;
    }

    private void finalizeMeanTemplate() {
        float[] sum = new float[EMBEDDING_DIM];
        // 逐维求均值：先累加再统一除以样本数，避免逐项除法的重复舍入。
        for (float[] e : accepted) {
            for (int i = 0; i < EMBEDDING_DIM; i++) {
                sum[i] += e[i];
            }
        }
        for (int i = 0; i < EMBEDDING_DIM; i++) {
            sum[i] /= TARGET_SAMPLES;
        }
        meanTemplate = ImageMetrics.l2Normalize(sum);
        // 局部累加器含全部样本信息，归一化副本已生成，用完立即擦除。
        Arrays.fill(sum, 0f);
        wipeSamples();
        wipeExistingSnapshot();
        state = State.COMPLETED;
    }

    /** 擦除并释放完成态均值模板（模板红线：释放前必须先清零，不能仅置 null）。 */
    private void wipeMeanTemplate() {
        if (meanTemplate != null) {
            Arrays.fill(meanTemplate, 0f);
            meanTemplate = null;
        }
    }

    /** 清零并丢弃已收样本向量。 */
    private void wipeSamples() {
        for (float[] e : accepted) {
            Arrays.fill(e, 0f);
        }
        accepted.clear();
    }

    /** 清零并丢弃现有模板快照。 */
    private void wipeExistingSnapshot() {
        for (float[] e : existingSnapshot) {
            Arrays.fill(e, 0f);
        }
        existingSnapshot = Collections.emptyList();
        // 快照被丢弃时同步清除失败标记：cancel()/reset()/重新 start() 后不得残留
        // “仓库不可用”状态而误拒后续会话。
        existingLoadFailed = false;
    }

    private static boolean isSampleStructurallyValid(@Nullable FrameSample s) {
        if (s == null) {
            return false;
        }
        if (s.frameWidth <= 0 || s.frameHeight <= 0) {
            return false;
        }
        if (s.box == null || !s.box.isValid()) {
            return false;
        }
        if (!ImageMetrics.allFiniteValues(s.grayMean, s.grayStd, s.laplacianVariance, s.detScore)) {
            return false;
        }
        if (s.embedding == null || s.embedding.length != EMBEDDING_DIM
                || !ImageMetrics.allFinite(s.embedding)) {
            return false;
        }
        return true;
    }

    /** 有限性判定：不使用 API 24 的 {@code Float.isFinite}，兼容 minSdk 21。 */
    private static boolean isFinite(float f) {
        return !Float.isNaN(f) && !Float.isInfinite(f);
    }

    private static int quotaFor(@NonNull Pose pose) {
        switch (pose) {
            case FRONT:
                return QUOTA_FRONT;
            case LEFT:
                return QUOTA_LEFT;
            case RIGHT:
            default:
                return QUOTA_RIGHT;
        }
    }

    @NonNull
    private FeedResult result(boolean accepted, @NonNull Reason reason, @Nullable Pose pose,
                             @NonNull String message) {
        return new FeedResult(accepted, reason, pose, acceptedTotal, state, message);
    }

    // ---------------- 数值工具 ----------------

    /**
     * 指标计算工具：均为纯函数、无状态、无图像落盘。Laplacian 采用 4 邻域核
     * {@code [0 1 0; 1 -4 1; 0 1 0]}，对内部像素求响应方差；灰度统计为总体统计。
     */
    public static final class ImageMetrics {

        private ImageMetrics() {
        }

        /** 人脸框面积 / 帧面积；帧或框非法时返回 0（必然低于门槛）。 */
        public static float areaRatio(@NonNull Box box, int frameWidth, int frameHeight) {
            if (frameWidth <= 0 || frameHeight <= 0 || !box.isValid()) {
                return 0f;
            }
            return (box.area() / (float) frameWidth) / (float) frameHeight;
        }

        /** 灰度均值（0..255，按无符号字节解释）；空数组返回 0。 */
        public static float grayMean(@NonNull byte[] gray) {
            if (gray.length == 0) {
                return 0f;
            }
            long sum = 0;
            for (byte b : gray) {
                sum += (b & 0xFF);
            }
            return (float) (sum / (double) gray.length);
        }

        /** 灰度总体标准差；空数组返回 0。 */
        public static float grayStd(@NonNull byte[] gray) {
            if (gray.length == 0) {
                return 0f;
            }
            double mean = grayMean(gray);
            double acc = 0;
            for (byte b : gray) {
                double d = (b & 0xFF) - mean;
                acc += d * d;
            }
            return (float) Math.sqrt(acc / gray.length);
        }

        /**
         * Laplacian 响应方差（清晰度）。仅统计内部像素；宽或高小于 3、或灰度长度与
         * 尺寸不符时返回 0（视为不合格，门槛拒收）。
         */
        public static float laplacianVariance(@NonNull byte[] gray, int width, int height) {
            if (width < 3 || height < 3 || gray.length != width * height) {
                return 0f;
            }
            double sum = 0;
            double sumSq = 0;
            int count = 0;
            for (int y = 1; y < height - 1; y++) {
                for (int x = 1; x < width - 1; x++) {
                    int idx = y * width + x;
                    int lap = 4 * (gray[idx] & 0xFF)
                            - (gray[idx - 1] & 0xFF)
                            - (gray[idx + 1] & 0xFF)
                            - (gray[idx - width] & 0xFF)
                            - (gray[idx + width] & 0xFF);
                    sum += lap;
                    sumSq += (double) lap * lap;
                    count++;
                }
            }
            if (count == 0) {
                return 0f;
            }
            double mean = sum / count;
            double var = sumSq / count - mean * mean;
            return (float) Math.max(0.0, var);
        }

        /** 余弦相似度；任一为零向量或长度不一致时返回 0。 */
        public static float cosine(@NonNull float[] a, @NonNull float[] b) {
            if (a.length != b.length || a.length == 0) {
                return 0f;
            }
            double dot = 0;
            double na = 0;
            double nb = 0;
            for (int i = 0; i < a.length; i++) {
                dot += (double) a[i] * b[i];
                na += (double) a[i] * a[i];
                nb += (double) b[i] * b[i];
            }
            if (na <= 0 || nb <= 0) {
                return 0f;
            }
            return (float) (dot / (Math.sqrt(na) * Math.sqrt(nb)));
        }

        /** L2 归一化副本；零向量原样返回零向量。 */
        @NonNull
        public static float[] l2Normalize(@NonNull float[] v) {
            double sumSq = 0;
            for (float f : v) {
                sumSq += (double) f * f;
            }
            float[] out = new float[v.length];
            if (sumSq <= 0) {
                return out;
            }
            float inv = (float) (1.0 / Math.sqrt(sumSq));
            for (int i = 0; i < v.length; i++) {
                out[i] = v[i] * inv;
            }
            return out;
        }

        /**
         * 由 5 点关键点（10 个 float：双眼/鼻/双嘴角）分类姿态。
         * 偏航代理 {@code yaw = (鼻尖x - 双眼中心x) / 瞳距}：超过 {@link #POSE_YAW_THRESHOLD}
         * 记为 {@link Pose#LEFT}，低于负阈值记为 {@link Pose#RIGHT}，否则 {@link Pose#FRONT}。
         * 左右仅为配额分类约定，与关键点次序解耦；输入非法时返回 null。
         */
        @Nullable
        public static Pose classifyPose(@Nullable float[] landmarks) {
            if (landmarks == null || landmarks.length != 10) {
                return null;
            }
            for (float f : landmarks) {
                if (!isFinite(f)) {
                    return null;
                }
            }
            float leftEyeX = landmarks[0];
            float rightEyeX = landmarks[2];
            float noseX = landmarks[4];
            float interEye = Math.abs(rightEyeX - leftEyeX);
            if (interEye <= 0f) {
                return null;
            }
            float yaw = (noseX - (leftEyeX + rightEyeX) * 0.5f) / interEye;
            if (yaw > POSE_YAW_THRESHOLD) {
                return Pose.LEFT;
            }
            if (yaw < -POSE_YAW_THRESHOLD) {
                return Pose.RIGHT;
            }
            return Pose.FRONT;
        }

        static boolean allFinite(@NonNull float[] v) {
            for (float f : v) {
                if (!isFinite(f)) {
                    return false;
                }
            }
            return true;
        }

        static boolean allFiniteValues(float... v) {
            for (float f : v) {
                if (!isFinite(f)) {
                    return false;
                }
            }
            return true;
        }
    }

    /** 将 512 维向量编码为小端 float32 字节（模板载荷格式，本类自定义序列化）。 */
    @NonNull
    public static byte[] encodeLe(@NonNull float[] v) {
        byte[] out = new byte[v.length * 4];
        int o = 0;
        for (float f : v) {
            int bits = Float.floatToIntBits(f);
            out[o] = (byte) (bits & 0xFF);
            out[o + 1] = (byte) ((bits >>> 8) & 0xFF);
            out[o + 2] = (byte) ((bits >>> 16) & 0xFF);
            out[o + 3] = (byte) ((bits >>> 24) & 0xFF);
            o += 4;
        }
        return out;
    }

    /** 解码小端 float32 模板；长度不等于 {@code 512*4} 时返回 null（不参与比对）。 */
    @Nullable
    public static float[] decodeLe(@Nullable byte[] bytes) {
        if (bytes == null || bytes.length != EMBEDDING_DIM * 4) {
            return null;
        }
        float[] out = new float[EMBEDDING_DIM];
        for (int i = 0; i < EMBEDDING_DIM; i++) {
            int o = i * 4;
            int bits = (bytes[o] & 0xFF)
                    | ((bytes[o + 1] & 0xFF) << 8)
                    | ((bytes[o + 2] & 0xFF) << 16)
                    | ((bytes[o + 3] & 0xFF) << 24);
            out[i] = Float.intBitsToFloat(bits);
        }
        return out;
    }
}
