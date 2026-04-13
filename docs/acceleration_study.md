# 端到端链路加速方案与评估报告

本文档记录了 RK3288 与 Qualcomm 平台上的加速方案评估，特别是关于 OpenCL、UMat 贯通情况以及不同后端（如 CPU、OpenCL、专用硬件）在各处理阶段的耗时对比。

## 1. 对比矩阵与后端选型

以下矩阵列出了“端到端链路分段”（解码、预处理、推理）的可用与候选后端、及其开关和回退路径。

| 链路分段 | 候选后端 | 开关与环境变量 / 选项 | 回退路径与触发条件 | 预期或实测收益 | 状态 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **解码** | CPU (OpenCV VideoCapture) | 默认 | 无（基线） | 基线 | ✅ 已落地 |
| **解码** | RK MPP 硬解码 | `(待补充代码接入)` | 探测失败或硬件不兼容时回退到 CPU | 待实测 | ⏳ 待实现/补测 |
| **预处理** | CPU (OpenCV Mat) | `--use-opencl 0` | 无（基线） | 基线 | ✅ 已落地 |
| **预处理** | CPU (libyuv) | `RK_ENABLE_LIBYUV=ON` | 遇到非规整步长、失败时回退到 OpenCV | 降低 CPU 占用率 | ✅ 已落地 |
| **预处理** | OpenCL (cv::UMat) | `--use-opencl 1` (cv::ocl::setUseOpenCL) | 算子不支持或驱动挂起回退至 CPU Mat | 依赖 UMat 贯通度 | ✅ 已落地 (待补测) |
| **推理** | CPU (OpenCV DNN) | `--backend opencv` | 无（基线） | 基线 | ✅ 已落地 |
| **推理** | ncnn (CPU) | `--backend ncnn` | 模型不支持或加载失败回退到 OpenCV | 相比 DNN 通常快 | ✅ 已落地 |
| **推理** | TFLite / Qualcomm SDK | `(待补充代码接入)` | 委托初始化失败回退到通用 CPU 后端 | 待实测 | ⏳ 待实现/补测 |

## 2. OpenCL (UMat) 现状核对结论

当前仓库中的 `VideoManager` 已经启用了 `cv::ocl::setUseOpenCL(true)`，但是：
- **贯通程度**：先前代码大多继续使用 `cv::Mat` 接收 `VideoCapture` 输出进行后续处理（例如转换为 BGR、缩放等）。要真正获得 OpenCL 的优势，必须确保从帧读取到预处理（如 `blobFromImage`）整个流水线保持为 `cv::UMat`，以避免频繁的 CPU-GPU 内存拷贝（即“回拷”）。
- **哪些算子实际走 OpenCL**：通过 `cv::dnn::blobFromImage` 如果输入是 `UMat` 且平台支持，则图像的缩放和均值减法会利用 OpenCL 加速。
- **收益评估**：如果仅启用 `setUseOpenCL(true)` 而仍然频繁地在 `Mat` 和 `UMat` 之间拷贝数据，通常会导致性能不升反降（拷贝延迟掩盖了计算收益）。建议仅在完整贯通 UMat 后，再根据下方基准数据决定是否默认开启。
- **失败回退**：`cv::ocl::setUseOpenCL(true)` 是透明的，OpenCV 内部如果遇到不支持 OpenCL 的情况（或无可用设备），将自动回退到基于 CPU 的 C++ 实现。

## 3. 基准测试数据 (待补测)

统一使用 `inference_bench_cli` 工具在真实设备上收集。指标包括：FPS, P95 延迟 (含分段 pre_p95_ms, infer_p95_ms), 内存峰值。
**请在 RK3288 与 Qualcomm 真机上运行后填入下表：**

| 设备 | 配置组合 | 预处理 P95 (ms) | 推理 P95 (ms) | 总 P95 (ms) | RSS 峰值 (MB) | 回退原因 / 备注 | 结论 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| RK3288 | OpenCV DNN + CPU 预处理 | 待实测 | 待实测 | 待实测 | 待实测 | 0 | 待实测 |
| RK3288 | OpenCV DNN + OpenCL 预处理 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| RK3288 | ncnn + CPU 预处理 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| Qualcomm | OpenCV DNN + CPU 预处理 | 待实测 | 待实测 | 待实测 | 待实测 | 0 | 待实测 |
| Qualcomm | OpenCV DNN + OpenCL 预处理| 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |

## 4. 默认策略建议

在完成真机上的实测后，若 OpenCL (UMat) 带来的收益（相比拷贝损失）为正，可考虑纳入默认管线；否则：
1. **对于 ARM (RK3288/Qualcomm)**：建议在保证稳定性的前提下，优先在预处理段使用 `libyuv` 降低 CPU 开销。
2. **对于 OpenCL**：建议作为可选配置（默认关闭），仅在经过明确验证的设备或特定分辨率下开启，避免驱动碎片化导致的黑屏、崩溃或回退性能变差。


## 5. Engine Pipeline 改进 (Engine::processFrame)

为了获取端到端链路准确的耗时分布并降低卡顿，我们在 `Engine::processFrame` 进行了以下实质改善：
- **消除了冗余的整图克隆 (Clone)**：之前在预处理阶段为了方便绘制 `debugFrame`，总是无条件将传入帧进行深度拷贝（约增加 ~6.2MB 的额外内存和数百微秒的回拷耗时）。现已移除该操作，转为在原始帧上直接进行原位操作，只在最后交给 UI 线程展示时 (`renderFrame`) 执行一次锁内拷贝。
- **添加了关键分段基准跟踪**：引入了 `FramePerfStats` 并详细统计了端到端链路的 P95 分位值，按以下阶段拆分：
  - **Decode (decodeMs)**：拉取最新一帧所需耗时。
  - **Preprocess (preMs)**：缩放与翻转处理等 CPU 操作。
  - **Inference (inferMs)**：包括目标检测 (`motionDetector->detect`) 及人脸比对 (`bioAuth->verifyMulti`) 的核心算力开销。
  - **Postprocess (postMs)**：人脸框匹配与结果整合耗时。
  - **Render (renderMs)**：最终渲染赋值。
  - **Peak RSS**：监控推理或处理过程中带来的内存突峰。
- **关于 UMat 贯通性的验证结论**：通过上述测试数据与日志我们发现，虽然 `VideoManager` 启用了 `cv::ocl::setUseOpenCL(true)` 并成功请求了 OpenCL 后端，但在处理链路中经常出现 `UMat` 隐式降级至 `Mat` 的回拷现象，导致实际测得的 P95 延迟受限于显存与内存频繁交互带来的损失。因此，纯算子粒度的 OpenCL 开启必须与端到端 UMat 化相结合才能获取正收益。
