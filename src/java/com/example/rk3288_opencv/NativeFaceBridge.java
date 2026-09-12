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
     * 校验并<b>暂存</b>模型运行时 profile（manifest v1 JSON 文本）——只做 manifest 格式/ABI/路径/SHA
     * 字段格式校验，<b>不加载推理管线、不校验模型文件存在性、不跑自检</b>。
     *
     * <p>因此本方法的成功（{@code validated:true}）<b>不得</b>被解释为「已加载生效」；真实生效由
     * {@link #activateRuntime(String, String, String, String)} 的 {@code effective}/{@code reason}
     * 决定（经 {@link #getRuntimeStatus()} 暴露）。模型文件的存在性/LFS/SHA 由 Java 侧
     * {@code FaceModelManager.verifyModelFiles} 与 activateRuntime 的管线 reload 承担。
     *
     * @param manifestJson manifest v1 的 JSON 文本（不是文件路径；文件读取与 SHA 校验
     *                     由 Java/资产层 {@code verifyFaceModels} 负责，JNI 不做文件 IO）
     * @param currentAbi   当前设备 ABI，必须出现在 manifest 的 {@code supportedAbis} 中
     * @return 成功 {@code {"ok":true,"validated":true,"staged":true,"effective":false,
     *         "profileId":...,"modelVersion":...,"embeddingDim":...}}；失败返回错误 JSON
     */
    static native String loadProfile(String manifestJson, String currentAbi);

    /**
     * 激活实时管线：把 manifest 与模型基目录绑定到原生实时管线（SCRFD 检测 + ArcFace 识别），
     * 使运行时从「仅预览 missing_model」切换到真实检测/识别。
     *
     * <p>必须在本方法成功（返回 {@code ok:true}）之前先经 {@link #loadProfile(String, String)}
     * 校验 manifest；本方法会再解析一次并触发 native 侧装配/自检/基准。若 Engine 尚未创建，
     * native 侧只登记待激活参数，待 Engine 创建后自动补激活（返回 {@code pending:true}）。
     *
     * @param manifestJson manifest v1 的 JSON 文本
     * @param baseDir      模型文件基目录（manifest 引用的相对路径相对此目录解析）
     * @param currentAbi   当前设备 ABI，必须出现在 manifest 的 {@code supportedAbis} 中
     * @param galleryDir   图库目录（当前图库来自内存注册，此参数作为契约位保留）
     * @return 成功 {@code {"ok":true,"pending":bool,"effective":bool,"reason":"..","generation":N}}；
     *         失败返回错误 JSON
     */
    static native String activateRuntime(String manifestJson, String baseDir,
                                         String currentAbi, String galleryDir);

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
     * 运行时聚合状态（profile / gallery / enrollment / 实时管线生效态）。
     *
     * <p>字段语义区分：{@code profileValidated}（兼容别名 {@code profileLoaded}）仅表示 manifest
     * 已校验/暂存；{@code effective} 才表示推理管线已加载生效，{@code reason} 为最近一次实时路径
     * 契约码（未生效时如 {@code pending}/{@code missing_model}）。
     *
     * @return {@code {"ok":true,"profileValidated":...,"profileId":...,"embeddingDim":...,
     *         "galleryCount":N,"enrollmentState":"...","effective":...,"reason":"..."}}
     */
    static native String getRuntimeStatus();
}
