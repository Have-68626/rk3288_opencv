## 附录 E. 性能优化与故障排障研究

# 性能优化与故障排障研究

本研究报告涵盖性能优化的路线图、决策门槛以及高级故障排查指南。

### 6.9.1 默认基线路径（Baseline Path）

默认基线的定位不是“最慢”，而是“最可控、最易定位、最易回滚”。基线必须满足：单一入口、可复现命令、稳定的指标输出、明确的失败原因。

**默认基线（必须长期可用）**：
- 推理后端：`ncnn (CPU)` 为主选；`OpenCV DNN (CPU)` 为备选回退（用于对照与兜底）。
- 通用计算：OpenCL/UMat 视为“可选加速”，不作为正确性/稳定性前提。
- 线程与资源：固定线程池、显式内存复用策略；禁止把“GPU 可用”当作假设。
- 失败处理：任何加速路径失败（初始化/运行时/性能抖动/异常码）必须 **自动回落到基线**，并输出“失败原因码 + 证据日志 + 回落结果”。

**现状差距提示（需要在路线图第 1 阶段补齐）**：
- 当前 `VideoManager` 构造函数直接调用 `cv::ocl::setUseOpenCL(true)`（见 `src/cpp/src/VideoManager.cpp`），属于“默认启用可选加速”的行为；若遇到驱动不稳定/算子覆盖不可控，会导致“收益不确定、故障难定位、无法精确回滚”。该行为应纳入“加速开关 + 自检 + 允许列表”体系后再进入默认配置。

### 6.9.2 推荐阶段顺序（先可观测与基准，再逐步引入加速）

阶段顺序遵循三条硬约束：
- 先把“看得见”做好：没有可观测与基准，就无法判断收益、也无法解释失败原因。
- 先把“能回退”做好：每一个加速开关都必须能独立回滚，并能给出可读的失败原因。
- 先做“低风险收益”：优先改 CPU/内存/线程等确定性优化，再引入 GPU/专用运行时等高不确定性加速。

#### 阶段 0：可观测与基准统一（必须先做）

**目标**：建立跨平台一致的指标与报告，让后续每个加速开关都有“收益/退化/失败原因”的证据链。

**范围**：
- 指标口径统一：延迟（P50/P95/P99）、吞吐（FPS/帧间隔）、资源（CPU/RSS/显存 best-effort）、稳定性（崩溃/ANR/相机重启次数）、识别指标（Top1、FAR/FRR 或至少 Top1+误识统计）。
- 报告落盘统一：输出到 `tests/metrics/` 与 `ErrorLog/`（失败材料必须落盘，便于复现与审计）。
- 设备集最小集：RK3288 1 台 + Qualcomm 1 台（真实设备），并明确“摄像头模组/分辨率/FPS/光照条件”作为基准前置条件。

**验收指标（必须同时满足）**：
- 任一设备可一键跑出“基线报告”（含原始 CSV/JSON + 汇总 Markdown），且报告字段稳定。
- 失败时能输出明确原因（例如：模型缺失、相机打开失败、OpenCL 初始化失败、权限拒绝等）并落盘到 `ErrorLog/`。

**退出条件（触发即停止进入下一阶段）**：
- 指标字段不稳定/无法复现（同一 commit 不同结果差异无解释）。
- 失败无法落盘或原因不可定位（只能看到“失败/崩溃”而没有证据）。

#### 阶段 1：加速开关 + 回退 + 失败原因码（必须先做）

**目标**：把所有“可能的加速点”包装成可控开关，并保证每个开关都有独立回退与失败原因。

**最小交付口径（建议）**：
- 开关形态：`AUTO / ON / OFF` 三态；`AUTO` 只在“自检通过 + 白名单设备 + 稳定性门槛通过”时启用。
- 回退语义：单点失败只回退该点，不影响其他点；多点失败必须回到“默认基线”。
- 原因码：为每类失败定义稳定枚举（例如 `ACCEL_OCL_UNAVAILABLE`、`ACCEL_OCL_CRASH_GUARD`、`ACCEL_NNAPI_INIT_FAIL`、`ACCEL_DNN_BACKEND_UNSUPPORTED`），并在日志/报告里出现。

**验收指标（必须同时满足）**：
- 任意开关关闭后，行为与基线一致（功能与指标可对照）。
- 任意开关开启失败时，自动回落且输出原因码；不会出现“部分启用但口径不明”的灰区状态。

**退出条件**：
- 开关无法回滚（需要重装/改代码/改模型才能恢复）。
- 失败原因码不稳定或无法映射到可执行修复路径。

#### 阶段 2：低风险收益（CPU/内存/线程/流水线）

**目标**：在不引入新依赖、不改变推理后端的情况下，获得确定性收益（通常是延迟与抖动改善）。

**优先方向**：
- 帧处理背压与丢帧策略：避免“算法线程阻塞导致全链路抖动”。
- 复用 Buffer/减少拷贝：降低峰值内存与 GC/分配抖动。
- 热身与缓存：把模型加载/首次推理抖动从基准采样中隔离并记录。

**验收指标（必须同时满足）**：
- 稳定性不退化：72h soak（或至少 8h）无崩溃/无 OOM；相机异常恢复次数不超过基线。
- 性能有收益：关键链路 P95 延迟下降 ≥ 10%（或 CPU 降幅 ≥ 10%），且识别指标不下降（Top1 下降 ≤ 0.2% 或 0）。

**退出条件**：
- 任何“收益”以显著稳定性退化为代价（崩溃/泄漏/相机重启增多）。
- 识别口径漂移（阈值/预处理版本变更但未同步标注与报告）。

#### 阶段 3：通用 GPU 加速（OpenCL/UMat 等，可选且可回滚）

**目标**：将 OpenCL 作为“可选加速”，只在验证过的设备上通过 `AUTO` 进入默认配置，以换取可观收益并保持可回退。

**范围建议（先易后难）**：
- 先只加速预处理/resize/色彩转换等“数值稳定且可对照”的算子，再考虑更复杂的算子链。
- 需要明确“实际走 OpenCL 的算子覆盖率与命中率”，并记录回退次数与原因。

**验收指标（必须同时满足）**：
- Qualcomm 与 RK3288 各至少 1 台设备上：P95 延迟下降 ≥ 15% 或 CPU 降幅 ≥ 15%，且崩溃/异常率不高于基线。
- 任意 OpenCL 相关异常（初始化失败、运行时错误、疑似驱动问题）必须触发自动降级到 CPU，并在报告中可见（计数+原因码）。

**退出条件**：
- OpenCL 使得稳定性退化（哪怕收益显著也禁止进入默认配置，只能停留在“可选试验开关”）。
- 命中率不可控（大量算子仍走 CPU，收益不稳定且无法解释）。

#### 阶段 4：Qualcomm 专用加速（NNAPI / QNN / DSP，按项目边界选择）

**目标**：在 Qualcomm 设备上获得“可观且可解释”的推理收益，并保持与 RK3288 的基线可比。

**策略**：
- 任何 Qualcomm 专用运行时必须作为“插件式后端”引入，不得破坏 RK3288 的基线闭环。
- 只允许在 `AUTO` 且满足门槛时进入默认配置；否则一律回到 `ncnn (CPU)` 或 `OpenCV DNN (CPU)`。

**验收指标（必须同时满足）**：
- Qualcomm 设备：推理 P95 延迟下降 ≥ 25%（或吞吐提升 ≥ 25%），且温升/降频不导致 30 分钟后退化。
- RK3288 不受影响：同一版本的基线仍可跑通并产出可对照报告。
- 失败原因可解释：初始化失败/算子不支持/精度漂移/委托回退等都有原因码与建议修复路径。

**退出条件**：
- 引入新依赖导致构建/许可/合规无法闭环（例如许可证不可审计、二进制来源不清）。
- 精度/阈值漂移无法被量化与回滚（“变快但不准”）。

#### 阶段 5：默认配置优化与发布（把“已验证收益”推入默认）

**目标**：把已通过门槛的加速点以“白名单 + 自检 + 失败回退”的方式进入默认配置，同时保持一键回滚能力。

**验收指标（必须同时满足）**：
- 默认配置在 RK3288 与 Qualcomm 上均不低于基线稳定性（崩溃/泄漏/相机重启不增加）。
- 默认配置至少在 1 类设备上带来可观收益（按 6.9.3 的门槛定义）。

**退出条件**：
- 无法做到“默认可观收益”且不退化稳定性：则默认应保持基线，仅允许用户手动开启加速。

### 6.9.3 Go/No-Go 门槛（默认门槛，建议写死在发布门禁）

门槛分为“全局红线”和“阶段绿线”。红线用于保护稳定性与可回滚性；绿线用于判断“值得进入默认配置”。

**全局红线（任一触发即 No-Go）**：
- 无法回滚：任何加速开关关闭后不能恢复到基线行为与指标口径。
- 稳定性退化：崩溃/ANR/相机不可恢复错误率高于基线（哪怕性能提升也 No-Go）。
- 失败不可解释：出现“启用后偶发失败但无原因码/无证据落盘”的情况。
- 精度不可控：识别指标退化超过阈值，或阈值/预处理版本变更未同步报告与标注。

**默认绿线（满足才允许进入默认配置）**：
- RK3288：关键链路 P95 延迟下降 ≥ 10% 或 CPU 降幅 ≥ 10%，且 8h+ 稳定性不退化。
- Qualcomm：关键链路 P95 延迟下降 ≥ 20% 或吞吐提升 ≥ 20%，且 30 分钟持续运行无明显退化（温升/降频造成的回落需记录）。
- 回退与原因：加速失败时能回落到基线，并统计回退次数与原因码（回退不应导致业务不可用）。

### 6.9.4 评审签署流程（从提案到默认上线）

建议将评审拆成四道门：设计门、实现门、性能门、发布门，确保“先证据、后默认、可回滚”。

1) 方案提案评审（Design Review，必须）
- 输入：加速点列表、开关/回退设计、失败原因码表、测试矩阵（RK3288+Qualcomm）、风险清单与回滚方案。
- 输出：批准进入开发（或退回补齐口径/证据链）。

2) 实现评审（PR Review，必须）
- 输入：代码变更、单测/集测、日志与指标字段变更说明、对旧口径兼容策略。
- 输出：允许合并到 `master`（或要求补齐测试与文档）。

3) 性能与稳定性评审（Perf/Soak Review，必须）
- 输入：基线对比报告（P50/P95/P99、CPU/RAM、回退计数、错误原因分布）、至少 1 台 RK3288 + 1 台 Qualcomm 的实测证据。
- 输出：是否允许进入 `AUTO` 默认白名单（或仅保留为实验开关）。

4) 发布 Go/No-Go 评审（Release Readiness，必须）
- 输入：发布说明（build_id、配置变更、默认开关状态）、回滚演练记录、已知问题与监控告警阈值。
- 输出：签署上线范围（灰度比例/白名单设备/回滚触发条件）。

### 6.9.5 利益相关方清单与必须提交的评审材料

**利益相关方（建议最小集合）**：
- 平台负责人：RK3288 设备负责人、Qualcomm 设备负责人（各 1 名）
- 算法负责人：模型/阈值/精度口径签署
- 客户端负责人：Android/Native 负责人（开关/回退/稳定性签署）
- 测试负责人：测试矩阵、稳定性与回归口径签署
- 安全与合规负责人：第三方依赖、许可证、数据最小化与日志脱敏签署
- 发布负责人：灰度/回滚/应急预案签署

**必须提交的评审材料（缺一不可）**：
- 《加速点决策表》：每个开关的默认状态（AUTO/ON/OFF）、可用设备白名单、回退条件、失败原因码示例与修复路径。
- 《基线对比报告》：至少包含 RK3288 与 Qualcomm 各 1 台的基线 vs 加速对比（原始数据 + 汇总结论）。
- 《稳定性/Soak 报告》：运行时长、崩溃/ANR、内存/句柄趋势、相机重启次数与恢复情况。
- 《回滚方案与演练记录》：如何一键回到基线、回滚触发阈值、演练截图/日志（敏感信息打码）。
- 《风险清单与已知问题》：驱动/机型差异、精度漂移风险、资源占用风险，以及对应的监控与缓解策略。
- 《依赖与许可证更新》：任何新增第三方依赖必须同步更新 `CREDITS.md`，并说明来源与许可证。

---

## 7. 高级主题与故障排查 (Advanced & Troubleshooting)

### 7.1 性能优化与安全加固检查点
- [ ] **安全加固**: 检查构建日志是否包含 `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-Wl,-z,relro,-z,now`。
- [ ] **内存泄露**: 使用 AddressSanitizer (`-fsanitize=address`) 运行测试。
- [ ] **热点分析**: 使用 `perf` 或 `simpleperf` 分析 CPU 占用。
- [ ] **GPU 利用率**: 确保 OpenCV 开启了 OpenCL (`cv::ocl::setUseOpenCL(true)`)。

### 7.2 常见问题 (FAQ)
*   **Q: 为什么 V4L2 采集到的颜色不对？**
    *   A: 检查 `pixelformat` 设置 (YUYV vs MJPEG)，以及 YUV 转 RGB 的公式是否匹配。
*   **Q: ncnn/OpenCV DNN 初始化失败？**
    *   A: 优先检查模型文件路径是否正确、模型格式是否匹配（ONNX / ncnn param+bin），以及输入/输出 blob 名称是否与模型一致；若为 Android 端，请确认 ABI 与依赖库齐全。
*   **Q: DRM 显示黑屏？**
    *   A: 确保没有其他显示服务 (如 X11, Wayland, SurfaceFlinger) 占用 `/dev/dri/card0`。

### 7.3 变更日志 (Changelog)
详见 [CHANGELOG.md](CHANGELOG.md)。

---

## 8. 验收标准 (Acceptance Criteria)

---

## 9. 图表索引

### 9.1 图索引
- **图 3-1** 系统分层架构
- [图 5-1 CameraX 生命周期与资源边界](appendix-c.md#fig-5-1)
- [图 5-2 CameraX 打开-预览-分析-释放](appendix-c.md#fig-5-2)
- [图 5-3 CameraX 拍照（ImageCapture）完整时序](appendix-c.md#fig-5-3)
- [图 5-4 CameraX 录像（VideoCapture + Recorder）完整时序](appendix-c.md#fig-5-4)

### 9.2 表索引
- **表 1-1** 术语对照表
- **表 2-1** 技术栈选型对照表
- **表 4-1** V4L2 采集流程
- **表 4-2** RK MPP 解码流程
- **表 4-3** ncnn / OpenCV DNN 推理闭环
- **表 4-4** DRM/KMS 双缓冲显示流程
- **表 4-5** CameraX + Native 分析流程
- [表 5-1 摄像头 API/库对比表](appendix-c.md#tbl-5-1)
- [表 5-2 常见第三方封装库对比](appendix-c.md#tbl-5-2)
- [表 5-3 Fotoapparat 与 CAMKit 对比（工程审计口径）](appendix-c.md#tbl-5-3)
- [表 5-4 Android 13+ 权限与后台限制对照](appendix-c.md#tbl-5-4)
- [表 5-5 常见拍照/录像输出格式](appendix-c.md#tbl-5-5)
- [表 5-6 广角/长焦/TOF/红外：枚举方法与启发式判定（工程口径）](appendix-c.md#tbl-5-6)
- [表 5-7 常用“能力查询”字段速查（闪光/对焦/曝光补偿等）](appendix-c.md#tbl-5-7)
- [表 6-1 离线端侧与云端方案对比表](appendix-d.md#tbl-6-1)
- [表 6-2 基准结果记录格式（CSV 字段定义）](appendix-d.md#tbl-6-2)
- [表 6-3 活体检测路线对比](appendix-d.md#tbl-6-3)
- [表 6-4 集成清单与交付物](appendix-d.md#tbl-6-4)
- [表 6-5 PAD 指标字段与解释](appendix-d.md#tbl-6-5)
- [表 6-6 人脸方案对比（ML Kit / MediaPipe / ArcFace / Dlib / 百度 / 优图）](appendix-d.md#tbl-6-6)
- [表 6-7 云厂商对比（阿里 / AWS / Azure）](appendix-d.md#tbl-6-7)
- [表 6-8 特征维度/模板大小/阈值/延迟对比表（统一口径模板，需以实测填充）](appendix-d.md#tbl-6-8)
- [表 6-9 特征加密文件格式（建议固定，便于兼容与迁移）](appendix-d.md#tbl-6-9)
- [表 6-10 特征加密存储异常映射与处理策略（建议直接照表实现）](appendix-d.md#tbl-6-10)
- [表 6-11 CI 门禁阈值（建议默认值，可按产品定义调整）](appendix-d.md#tbl-6-11)

---

## 10. 风险清单

| 风险 | 触发条件 | 影响 | 缓解策略 |
| :--- | :--- | :--- | :--- |
| 摄像头适配碎片化 | 设备 HAL 差异、硬件级别不足 | 预览/拍照不可用或质量差 | 优先 CameraX，必要时降级/黑名单；产出设备能力报告 |
| 权限与后台限制 | 用户拒绝/永久拒绝、Android 10+ 后台限制 | 无法打开摄像头或异常释放 | 权限 Gate 模板 + 设置页回流处理；onStop 强制释放 |
| 泄漏与内存上涨 | 未关闭 ImageProxy、未清理 Analyzer | OOM、抖动、帧阻塞 | 背压策略 + 强制 close；基准脚本持续压测 |
| 人脸识别口径漂移 | 阈值/数据集/统计口径不一致 | 指标不可比，验收争议 | 固化字段定义与记录格式；版本化数据集与配置 |
| 合规风险 | 生物特征采集/传输/存储无说明 | 合规审计失败 | 端侧优先、最小化原则、加密存储；第三方许可证入库更新 CREDITS.md |

---

## 11. 参考文献

1. Android Developers: CameraX 文档  
   <https://developer.android.com/media/camera/camerax>
2. Android Developers: Camera2 API  
   <https://developer.android.com/reference/android/hardware/camera2/package-summary>
3. Android Developers: 请求应用权限（运行时权限）  
   <https://developer.android.com/training/permissions/requesting>
4. Android Developers: 后台执行限制（与相机后台限制相关）  
   <https://developer.android.com/about/versions/oreo/background>
5. ISO/IEC 30107-3:2017 Biometric presentation attack detection（PAD）测试与评估  
   (需至 ISO 官网搜索标准号查阅)
6. Android Developers: CameraX VideoCapture（录像管线）  
   <https://developer.android.com/media/camera/camerax/video-capture>
7. Android Developers: Android 13 行为变更（权限/通知等）  
   <https://developer.android.com/about/versions/13/behavior-changes-13>
8. Android Developers: Android 14 行为变更（前台服务类型等）  
   <https://developer.android.com/about/versions/14/behavior-changes-14>
9. Android Developers: 前台服务概览（Foreground Services）  
   <https://developer.android.com/develop/background-work/services/foreground-services>
10. Android Developers: MediaStore（媒体保存与读取）  
    <https://developer.android.com/training/data-storage/shared/media>
11. Android Developers: CameraCharacteristics（能力枚举关键字段）  
    <https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics>
12. NIST FRVT（人脸识别基准评测项目，指标口径参考）  
    <https://www.nist.gov/programs-projects/face-recognition-vendor-test-frvt>
13. Google ML Kit: Face Detection  
    <https://developers.google.com/ml-kit/vision/face-detection>
14. MediaPipe Tasks: Vision（Face Detector / Face Landmarker）  
    <https://ai.google.dev/edge/mediapipe/solutions/guide#vision>
15. ArcSoft（虹软）人脸识别 SDK（ArcFace）  
    （访问受限，请在搜索引擎中查找“虹软 ArcFace 官网”）
16. 百度智能云：人脸识别  
    <https://ai.baidu.com/tech/face>
17. 腾讯云：人脸识别/优图（按产品线）  
    <https://cloud.tencent.com/product/facerecognition>
18. Dlib 官方项目  
    <https://dlib.net/>
19. 阿里云：视觉智能开放平台（Facebody）  
    <https://help.aliyun.com/product/135949.html>
20. AWS Rekognition  
    <https://aws.amazon.com/rekognition/>
21. Azure AI Face  
    <https://learn.microsoft.com/en-us/azure/ai-services/face/overview-identity>
22. Deng, J. et al.: ArcFace: Additive Angular Margin Loss for Deep Face Recognition (CVPR 2019)  
    <https://arxiv.org/abs/1801.07698>
23. InsightFace（开源实现集合，ArcFace 等）  
    <https://github.com/deepinsight/insightface>
24. Android Developers: Logical multi-camera（Camera2 多摄与 physicalCameraIds 相关）  
    <https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics#physicalCameraIds>
25. Android Developers: 深度输出与相关格式（DEPTH16/DEPTH_POINT_CLOUD）  
    <https://developer.android.com/reference/android/graphics/ImageFormat>
26. Android Developers: CaptureRequest（曝光/对焦/闪光等请求字段）  
    <https://developer.android.com/reference/android/hardware/camera2/CaptureRequest>
27. Android Developers: CameraMetadata（常量与枚举值定义）  
    <https://developer.android.com/reference/android/hardware/camera2/CameraMetadata>
28. ISO/IEC 19794-5: Face image data（人脸图像与模板相关数据交换标准，按需对齐）  
    (需至 ISO 官网搜索标准号查阅)
29. Android Developers: Android Keystore 系统（密钥生成与存储）  
    <https://developer.android.com/privacy-and-security/keystore>
30. Android Developers: KeyGenParameterSpec（Keystore 对称密钥参数）  
    <https://developer.android.com/reference/android/security/keystore/KeyGenParameterSpec>
31. Android Developers: Cipher / AES-GCM（JCA API 与使用约束）  
    <https://developer.android.com/reference/javax/crypto/Cipher>
32. Robolectric 官方文档  
    <https://robolectric.org/>
33. AndroidX Test: Espresso  
    <https://developer.android.com/training/testing/espresso>
34. AndroidX CameraX: Testing（camera-testing）  
    <https://developer.android.com/media/camera/camerax/architecture>
35. GitHub Actions: Workflow syntax for GitHub Actions  
    <https://docs.github.com/actions/writing-workflows/workflow-syntax-for-github-actions>
36. reactivecircus/android-emulator-runner（GitHub Actions Android 模拟器运行器）  
    <https://github.com/ReactiveCircus/android-emulator-runner>
