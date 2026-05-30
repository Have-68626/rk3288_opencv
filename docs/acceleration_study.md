# 加速链路现状与验收口径

最后更新：2026-05-30

本文档作为当前仓库加速链路的定稿说明，目标是把“代码已经具备的能力”和“仍需真机验收的性能结论”明确分开。

本文档只确认以下两类事实：
- 当前代码中已经落地的加速开关、回退路径、证据输出和配置语义。
- 仓库内已有的历史证据与主机侧验证结果。

本文档**不**把尚未在 RK3288 真机上完成的性能收益写成既成事实。凡是涉及 `P95` 提升、CPU 降幅、默认策略升级，仍以真实设备基准结果为准。

## 1. 统一 contract 与证据链

当前仓库已经统一了 acceleration contract，核心字段为：

| 路径 | requested 来源 | effective / evidence 输出位置 | 说明 |
| :--- | :--- | :--- | :--- |
| `opencl` | Android: `RK_USE_OPENCL`；CLI: `--use-opencl`；Windows: `acceleration.enableOpenCL` | `Engine::performAccelSelfCheck()` / `inference_bench_cli` | 默认保守关闭；输出 `requested/effective/reason/evidence` |
| `libyuv` | Android: `RK_USE_LIBYUV`；Windows: `acceleration.enableLibyuv` | `Engine::performAccelSelfCheck()` | 仅在外部帧颜色转换路径按运行时开关启用 |
| `mpp` | Android: `RK_USE_MPP`；Windows: `acceleration.enableMpp` | `Engine::performAccelSelfCheck()` | 非 RK/Windows 主机上明确标记 `unsupported_platform` |
| `qualcomm` | Android: `RK_USE_QUALCOMM`；Windows: `acceleration.enableQualcomm` | `Engine::performAccelSelfCheck()` / `inference_bench_cli` | 当前为占位后端，统一回退并输出原因 |
| `detector_backend` | Windows: `model.detectorBackend` | Windows INI/JSON 配置归一化；CLI 结果输出 backend requested/effective | 允许 `opencv_dnn` / `ncnn` / `qualcomm` 语义统一 |
| `recognition_backend` | Windows: `model.recognitionBackend` | Windows INI/JSON 配置归一化 | 与检测后端独立配置 |
| `detection_throttle` | Android UI / native runtime | `Engine` 日志输出 | 输出模式与 interval 的 effective 结果 |
| `recognition_throttle` | Android UI / native runtime | `Engine` 日志输出 | 输出模式与 interval 的 effective 结果 |

固定 reason code 已统一到：
- `ok`
- `build_disabled`
- `unsupported_platform`
- `missing_dependency`
- `missing_model`
- `runtime_init_failed`
- `unsupported_input`

## 2. 当前实现状态

### 2.1 解码

| 链路分段 | 后端 | 当前状态 | 回退路径与触发条件 | 验证状态 |
| :--- | :--- | :--- | :--- | :--- |
| 解码 | OpenCV `VideoCapture` | ✅ 已落地 | 基线实现 | 已验证 |
| 解码 | RK MPP | ✅ 已接线 | 仅用于本地 mock 文件；初始化失败、平台不支持、依赖缺失时回退到 `VideoCapture` | 代码闭环已验证；RK3288 真机性能未验收 |

说明：
- `VideoManager` 已具备 MPP 与 `VideoCapture` 的受控双路径，不再是文档中的“待补充代码接入”状态。
- Windows / 主机侧不把 MPP 视为已生效能力，而是通过 `unsupported_platform` 或 `build_disabled` 明确解释回退原因。

### 2.2 预处理

| 链路分段 | 后端 | 当前状态 | 回退路径与触发条件 | 验证状态 |
| :--- | :--- | :--- | :--- | :--- |
| 预处理 | OpenCV `Mat` | ✅ 已落地 | 基线实现 | 已验证 |
| 预处理 | `libyuv` | ✅ 已落地 | 仅在外部帧 `NV21` / `YUV_420_888` 转 BGR 路径启用；遇到 stride / 输入不满足条件时回退到 OpenCV | 代码闭环已验证；真机收益未验收 |
| 预处理 | OpenCL / `UMat` | ⚠️ 仅部分链路可测 | `inference_bench_cli` 有 `UMat + blobFromImage` 测试路径；主 Android 实时链路尚未实现端到端 `UMat` 贯通 | 仅具备可观测性，不具备默认开启依据 |

说明：
- `libyuv` 现在是**编译期开关 + 运行时开关**双重控制，不再只是 `RK_ENABLE_LIBYUV=ON` 的静态能力描述。
- OpenCL 已经完成“保守默认关闭 + requested/effective/evidence 输出”，但 Android 主链路并没有完成完整 `UMat` 化，因此不能把它写成“已完成可用加速”。

### 2.3 推理

| 链路分段 | 后端 | 当前状态 | 回退路径与触发条件 | 验证状态 |
| :--- | :--- | :--- | :--- | :--- |
| 推理 | OpenCV DNN | ✅ 已落地 | 基线实现 | 已验证 |
| 推理 | ncnn | ✅ 已落地 | `.param/.bin` 路径优先选 ncnn；加载失败或构建不支持时回退到 OpenCV DNN | 代码闭环已验证；RK3288 真机性能未验收 |
| 推理 | Qualcomm 专用后端 | ⚠️ 占位接线 | 初始化失败或依赖缺失时回退到 CPU / OpenCV DNN | 仅配置与证据链闭环，未形成可用性能路径 |

说明：
- `YoloFaceAdapter` 和 `ArcFaceAdapter` 都已经具备 ncnn 优先与 OpenCV DNN 回退逻辑。
- `inference_bench_cli` 已输出：
  - `backend_requested`
  - `backend_effective`
  - `backend_reason`
  - `backend_evidence`

### 2.4 调度减载

| 路径 | 当前状态 | 输出证据 | 验证状态 |
| :--- | :--- | :--- | :--- |
| `detection_throttle` | ✅ 已落地 | `ACCEL_SELF_CHECK` 风格日志，包含 mode 与 `interval_ms` | 已验证 |
| `recognition_throttle` | ✅ 已落地 | `ACCEL_SELF_CHECK` 风格日志，包含 mode 与 `interval_ms` | 已验证 |

说明：
- `InferenceThrottle` 已经从“只有框架”进入“请求值、实际值、间隔值都能落日志”的状态。

## 3. 已确认结论

### 3.1 当前代码与配置层面的结论

1. 加速开关的语义已经统一，Android / CLI / Windows 不再各说各话。
2. 每条关键加速路径都具备 `requested`、`effective`、`reason`、`evidence` 这条证据链。
3. Windows 本轮完成的是**配置语义与回退说明对齐**，不是 RK3288 专属加速能力移植。
4. MPP、ncnn、libyuv、throttle 都已从“文档设想”进入“代码可执行、可回退、可解释”的状态。

### 3.2 仓库内历史证据的结论

依据 [docs/analysis/evidence_20170115_analysis.md](docs/analysis/evidence_20170115_analysis.md) 这份历史设备证据，可以确认：

- 该次 RK3288 采集样本中：
  - `OpenCL` 不可用（`effective=0`）
  - `RK_HAVE_MPP=0`
  - `RK_HAVE_QUALCOMM=0`
- 当时系统处于典型的 `CPU-only` 运行状态。
- 这份证据能够证明“历史上曾经没有任何硬件加速实际生效”，但**不能**直接证明当前版本在目标真机上的最终性能结论。

### 3.3 目前不能写成既成事实的结论

以下内容当前仍不能在仓库文档中写成“已达成”：

- RK3288 上 ncnn 使 YOLO + ArcFace 推理 `P95` 降低 30%+
- RK3288 上 MPP 使帧分析总 `P95` 降低 50%+
- OpenCL 在目标设备上值得作为默认路径开启
- Qualcomm 专用后端已形成稳定可交付的推理收益

这些都需要真实设备基准产物，而当前仓库尚未包含对应定版结果。

## 4. 定稿后的默认策略

在没有新的真机基准结果前，默认策略应解释为：

1. **发布基线**
   - 解码：`VideoCapture`
   - 预处理：OpenCV，`libyuv` 作为受控可选路径
   - 推理：`OpenCV DNN` 基线，`ncnn` 作为明确可回退的可选后端
2. **OpenCL**
   - 默认关闭
   - 只在明确探测到收益、且没有稳定性回退的设备上考虑升级策略
3. **MPP**
   - 只在 RK 平台、本地 mock 文件链路中启用
   - 任何异常立即回退 `VideoCapture`
4. **Qualcomm**
   - 当前仅保留统一配置与回退语义，不作为默认性能路径

## 5. 真机验收口径

后续如果要把本文件从“代码与证据定稿”升级为“性能结论定稿”，必须补齐同一输入集下的固定矩阵：

| 设备 | 配置组合 | 预处理 P95 | 推理 P95 | 总 P95 | RSS 峰值 | 结论门槛 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| RK3288 | `opencv_dnn` baseline | 必填 | 必填 | 必填 | 必填 | 作为对照组 |
| RK3288 | `throttle only` | 必填 | 必填 | 必填 | 必填 | 验证减载收益 |
| RK3288 | `ncnn` | 必填 | 必填 | 必填 | 必填 | 推理 `P95` 降幅 ≥ 30% |
| RK3288 | `ncnn + throttle` | 必填 | 必填 | 必填 | 必填 | 组合收益可解释 |
| RK3288 | `ncnn + MPP` | 必填 | 必填 | 必填 | 必填 | 解码收益可解释 |
| RK3288 | `ncnn + MPP + libyuv` | 必填 | 必填 | 必填 | 必填 | 端到端收益可解释 |

统一要求：
- 产物必须来自 `inference_bench_cli`、运行日志或等价的结构化报告。
- 每个组合都必须附带 `requested/effective/reason/evidence`。
- 若某条路径未生效，必须给出固定原因码，而不是只写“不可用”。

在这些结果落库之前，本文档的“定稿”含义仅限于：**当前代码状态、配置语义、回退路径和证据口径已经定稿。**
