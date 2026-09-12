package com.example.rk3288_opencv;

import java.nio.ByteBuffer;

/**
 * NativeFaceBridge —— 人脸运行时 / 注册域 JNI 桥（任务13）。
 *
 * <p>本类只声明 {@code native} 方法，真正的实现与动态注册位于
 * {@code src/cpp/jni/native_face_bridge.cpp}（{@code registerFaceBridgeMethods}），
 * 并在 {@code src/cpp/jni/registry.cpp} 中按类名绑定到本类（JniMethodRegistry 动态注册，
 * 非 JNI mangled 符号）。因此新增/修改本类 native 声明时必须同步更新 C++ 注册表，
 * 否则 {@code scripts/jni-sync-audit.js} 会报 MISMATCH，运行时抛 UnsatisfiedLinkError。
 *
 * <p>方法签名约定：
 * <ul>
 *   <li>除 {@link #takeEnrollmentTemplate()} 外，均返回 JSON 字符串，
 *       成功为 {@code {"ok":true,...}}，失败为
 *       {@code {"ok":false,"error":{"code":"...","message":"..."}}}；任何非法入参都返回失败，
 *       不抛异常、不崩溃。</li>
 *   <li>embedding 一律以 <b>direct</b> {@link ByteBuffer}（小端 float32）传入，
 *       长度必须精确为 {@code 512 * 4 = 2048} 字节；JNI 侧用
 *       {@code GetDirectBufferAddress/Capacity} 双重校验，非 direct 或长度不符直接拒绝。</li>
 * </ul>
 *
 * <p>安全约束：模板与原始 embedding 向量<b>严禁写入日志</b>，本类与对应 JNI 实现均不例外。
 */
final class NativeFaceBridge {
    static {
        System.loadLibrary("native-lib");
    }

    /** 512 维 ArcFace/MobileFaceNet 模板，单精度浮点，字节数恒定。 */
    static final int EMBEDDING_DIM = 512;
    /** 单个 embedding 的字节数（float32）。 */
    static final int EMBEDDING_BYTES = EMBEDDING_DIM * 4;
    /** 内存人脸库容量上限（JNI 边界硬约束，超出即失败）。 */
    static final int GALLERY_CAPACITY = 1000;

    private NativeFaceBridge() {
    }

    /**
     * 加载模型运行时 profile（manifest v1 JSON 文本），解析成功后缓存在 native 侧。
     *
     * @param manifestJson manifest v1 的 JSON 文本（不是文件路径；文件读取与 SHA 校验
     *                     由 Java/资产层 {@code verifyFaceModels} 负责，JNI 不做文件 IO）
     * @param currentAbi   当前设备 ABI，必须出现在 manifest 的 {@code supportedAbis} 中
     * @return 成功 {@code {"ok":true,"profileId":...,"modelVersion":...,"embeddingDim":...}}；失败返回错误 JSON
     */
    static native String loadProfile(String manifestJson, String currentAbi);

    /**
     * 用一批 (personId, embedding) 原子替换内存人脸库。
     *
     * <p>边界：{@code personIds.length == embeddings.length}；每项 embedding 必须为 direct
     * ByteBuffer 且长度精确 2048 字节；条目数 ≤ {@link #GALLERY_CAPACITY}；personId 非空。
     * 全部校验通过后才替换（否则保留旧库）。
     *
     * @return 成功 {@code {"ok":true,"count":N,"dim":512}}；失败返回错误 JSON
     */
    static native String replaceGallery(String[] personIds, ByteBuffer[] embeddings);

    /**
     * 开始一次注册会话：暂存 personId 与该人的 512 维 embedding（仅驻内存，不落日志、不落盘），
     * 供 {@link #takeEnrollmentTemplate()} 一次性取走。
     *
     * @param embedding 必须为 direct ByteBuffer，长度精确 2048 字节
     * @return 成功 {@code {"ok":true,"state":"ENROLLING","staged":true}}；失败返回错误 JSON
     */
    static native String beginEnrollment(String personId, ByteBuffer embedding);

    /**
     * 取消进行中的注册会话并清零暂存模板。
     *
     * @return {@code {"ok":true,"state":"IDLE","cleared":true|false}}
     */
    static native String cancelEnrollment();

    /**
     * 查询注册会话状态（不含任何模板内容）。
     *
     * @return {@code {"ok":true,"state":"IDLE|ENROLLING","staged":bool,"personId":"..."}}
     */
    static native String getEnrollmentStatus();

    /**
     * 取走并清空暂存的注册模板（FaceTemplate 序列化字节：magic + header + float32 payload）。
     * 一次性语义：成功取走后会话回到 IDLE，重复调用返回 null。
     *
     * @return 序列化模板字节；无暂存模板时返回 {@code null}
     */
    static native byte[] takeEnrollmentTemplate();

    /**
     * 运行时聚合状态（profile / gallery / enrollment）。
     *
     * @return {@code {"ok":true,"profileLoaded":...,"galleryCount":N,"enrollmentState":"..."}}
     */
    static native String getRuntimeStatus();
}
