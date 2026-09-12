package com.example.rk3288_opencv;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.util.Map;

/**
 * FaceRuntimeStatus —— 人脸模型运行时状态快照（任务 14）。
 *
 * <p>本类是模型管理器（{@link FaceModelManager}）对外暴露的<b>只读状态</b>，供 UI 判断：
 * <ul>
 *   <li>首启无内置模型 → {@link State#NOT_IMPORTED}，{@link #message} 为用户引导文案；</li>
 *   <li>已导入并成功激活 → {@link State#READY}，携带 profileId / 版本 / 许可；</li>
 *   <li>已激活但 manifest/文件损坏 → {@link State#ERROR}。</li>
 * </ul>
 *
 * <h2>模型来源信号（供「模型与加速状态」界面使用）</h2>
 * P1-6 路线下 <b>APK 不内置任何权重</b>，因此所有可用模型都来自用户导入：
 * <ul>
 *   <li>{@link #isUserImported()} —— 是否有用户导入且激活的模型；</li>
 *   <li>{@link #sourceLabel()} —— 可直接展示的中文来源标签（「用户导入」/「未导入」/「不可用」）。</li>
 * </ul>
 * UI 只需调用 {@code FaceModelManager.forDefault(ctx).status().sourceLabel()} 即可，无需改动
 * native/C++。
 *
 * <p>本类不包含任何模型权重/向量内容，只含元数据与中文提示。
 */
public final class FaceRuntimeStatus {

    /** 运行时状态。 */
    public enum State {
        /** 未导入任何模型（首启默认态），需引导用户通过 SAF 导入 ZIP。 */
        NOT_IMPORTED,
        /** 已导入并成功激活（native 自检通过）。 */
        READY,
        /** 存在激活配置但校验失败（manifest 损坏、文件缺失、native 拒绝）。 */
        ERROR
    }

    @NonNull public final State state;
    /** 当前激活的 profileId；未导入时为空串。 */
    @NonNull public final String profileId;
    /** manifest 展示名；未导入时为空串。 */
    @NonNull public final String displayName;
    /** 模型版本；未导入时为空串。 */
    @NonNull public final String modelVersion;
    /** 许可标识（如 insightface-models-noncommercial-research）；未导入时为空串。 */
    @NonNull public final String licenseId;
    /** 已导入的 profile 数量（storeDir 下含合法 manifest 的目录数，含未激活者）。 */
    public final int importedProfileCount;
    /** native 侧 {@code getRuntimeStatus} 报告的 profileLoaded；native 不可用时为 false。 */
    public final boolean nativeProfileActive;
    /** native 侧报告的 embedding 维度；未知时为 0。 */
    public final int nativeEmbeddingDim;
    /** 可直接展示的中文提示（引导或错误原因），不含敏感内容。 */
    @NonNull public final String message;

    FaceRuntimeStatus(@NonNull State state, @NonNull String profileId, @NonNull String displayName,
                      @NonNull String modelVersion, @NonNull String licenseId,
                      int importedProfileCount, boolean nativeProfileActive, int nativeEmbeddingDim,
                      @NonNull String message) {
        this.state = state;
        this.profileId = profileId;
        this.displayName = displayName;
        this.modelVersion = modelVersion;
        this.licenseId = licenseId;
        this.importedProfileCount = importedProfileCount;
        this.nativeProfileActive = nativeProfileActive;
        this.nativeEmbeddingDim = nativeEmbeddingDim;
        this.message = message;
    }

    public boolean isReady() {
        return state == State.READY;
    }

    public boolean isNotImported() {
        return state == State.NOT_IMPORTED;
    }

    /** 是否有用户导入且激活的模型（P1-6 下所有可用模型均为用户导入）。 */
    public boolean isUserImported() {
        return state == State.READY && !profileId.isEmpty();
    }

    /** 中文来源标签，UI 可直接展示。 */
    @NonNull
    public String sourceLabel() {
        switch (state) {
            case READY:
                return "用户导入";
            case NOT_IMPORTED:
                return "未导入";
            default:
                return "不可用";
        }
    }

    /** 首启未导入态工厂，携带 SAF 导入引导文案。 */
    @NonNull
    public static FaceRuntimeStatus notImported(int importedProfileCount) {
        return new FaceRuntimeStatus(State.NOT_IMPORTED, "", "", "", "",
                importedProfileCount, false, 0,
                "尚未导入人脸模型：APK 不内置权重，请通过「导入模型」选择包含 manifest.json 的 ZIP 包（≤256 MiB）。");
    }

    /** 就绪态工厂。 */
    @NonNull
    public static FaceRuntimeStatus ready(@NonNull String profileId, @NonNull String displayName,
                                          @NonNull String modelVersion, @NonNull String licenseId,
                                          int importedProfileCount, boolean nativeProfileActive,
                                          int nativeEmbeddingDim) {
        return new FaceRuntimeStatus(State.READY, profileId, displayName, modelVersion, licenseId,
                importedProfileCount, nativeProfileActive, nativeEmbeddingDim,
                "模型已就绪：" + displayName + "（" + profileId + "）");
    }

    /** 错误态工厂；{@code reason} 为中文原因，不含模型内容。 */
    @NonNull
    public static FaceRuntimeStatus error(@NonNull String profileId, int importedProfileCount,
                                          @NonNull String reason) {
        return new FaceRuntimeStatus(State.ERROR, profileId, "", "", "",
                importedProfileCount, false, 0, reason);
    }

    /**
     * 合并 native {@code getRuntimeStatus()} 的 JSON（{@code {"ok":true,"profileLoaded":..}}），
     * 补全 {@link #nativeProfileActive}/{@link #nativeEmbeddingDim}。
     *
     * <p>解析失败或 native 不可用时<b>原样返回 this</b>，保证 UI 层拿到稳定状态、不因 native
     * 缺失而崩溃。
     */
    @NonNull
    public FaceRuntimeStatus withNativeStatus(@Nullable String nativeStatusJson) {
        if (nativeStatusJson == null || nativeStatusJson.isEmpty()) {
            return this;
        }
        try {
            Map<String, Object> root = FaceModelManifest.asObject(
                    FaceModelManifest.parseJsonValue(nativeStatusJson));
            if (root == null) {
                return this;
            }
            Boolean loaded = FaceModelManifest.asBoolean(root.get("profileLoaded"));
            Double dim = FaceModelManifest.asNumber(root.get("embeddingDim"));
            if (loaded == null && dim == null) {
                // native 返回里没有可合并字段时保持原对象，便于调用方按引用判断「未变化」。
                return this;
            }
            boolean active = loaded != null && loaded;
            int d = (dim != null) ? dim.intValue() : 0;
            return new FaceRuntimeStatus(state, profileId, displayName, modelVersion, licenseId,
                    importedProfileCount, active, d, message);
        } catch (Exception ex) {
            return this;
        }
    }

    @NonNull
    @Override
    public String toString() {
        return "FaceRuntimeStatus{state=" + state + ", profileId='" + profileId
                + "', imported=" + importedProfileCount
                + ", nativeActive=" + nativeProfileActive + "}";
    }
}
