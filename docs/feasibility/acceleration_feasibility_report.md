# 端到端加速方案可行性研究报告（CPU / OpenCL / 专用硬件）

## 0. 结论摘要（先看这个）

### 0.1 可行性结论
本项目的“端到端加速”总体 **可行（条件可行、必须分阶段推进）**：
- **短期高可行（建议优先落地）**：基于现有代码基础做“可观测性 + 基准体系 + 低风险 CPU/内存/线程优化 + libyuv 覆盖面增强 + ncnn/OpenCV DNN 后端治理与回退”。这些方向对现有架构兼容性最好，维护成本可控，且更容易获得可复现收益。
- **中期可行但不确定（必须实测、不可默认强开）**：OpenCL/UMat 透明加速。当前仓库只做了 `cv::ocl::setUseOpenCL(true)` 的全局开关，但没有 UMat 管线与“算子命中率/回退原因”证据链；在 RK3288（Mali）上稳定性与收益高度依赖驱动与 OpenCV 覆盖，存在“无收益/变慢/不稳定”的现实风险。该路径只能作为 **AUTO/白名单** 的可选项推进。
- **中长期可行但高风险（建议以 PoC 方式推进）**：RK MPP 硬解码（零拷贝）与 Qualcomm 专用推理后端（NNAPI/QNN/DSP）。它们潜在收益大，但引入外部依赖与平台碎片化明显，维护成本与排障成本显著上升，且必须配套严格的开关/回退/证据落盘机制。

### 0.2 推荐决策（默认策略）
- **默认配置（发布基线）**：CPU-only 可长期可用（推理后端主选 `ncnn CPU`，回退 `OpenCV DNN CPU`；预处理优先 `libyuv`，失败回退 OpenCV）。
- **OpenCL/UMat**：不作为默认正确性前提；仅在能力探测通过、白名单设备、基准收益达标且稳定性不退化时，允许通过 `AUTO` 进入默认配置。
- **MPP / Qualcomm SDK**：作为可插拔后端推进；任何异常必须自动回落到基线，并输出“失败原因码 + 证据”。

### 0.3 决策门槛（Go / No-Go）
进入“实现阶段”的最低门槛：
- 已建立统一基准与报告格式；能在 **RK3288 + 至少 1 台 Qualcomm** 上输出可复现报告。
- 每个加速点都定义了：默认值、启用条件、失败检测、回退路径、失败原因输出（且能落盘）。
- 评审材料齐备并通过签署（见第 7 章）。

> 本报告只做“可行性研究与决策”，不直接改变当前业务默认链路；任何实现必须在利益相关方评审通过后再开始。

---

## 1. 背景与业务需求（来自 README 的待办 35）

需求要点（来自 [README.md:L168-L172](file:///d:/19842/Documents/GitHub/rk3288_opencv/README.md#L168-L172)）：
- 已在 `VideoManager` 启用 OpenCL，但缺少可量化结论：哪些算子走 OpenCL、收益多少、失败如何回退。
- 需形成“端到端链路分段”的加速选型与开关：解码（CPU vs MPP）、预处理（NEON vs OpenCL vs libyuv）、检测/特征（ncnn/OpenCV DNN/TFLite/Qualcomm SDK）。
- 需建立统一基准测量脚本与报告格式：FPS、P95 延迟、功耗/温度（可选）、内存峰值。
- 验收要求：在 RK3288 与至少 1 台 Qualcomm 上可复现；每个开关有回退与失败原因；默认配置稳定性不退化前提下获得可观收益。

---

## 2. 现有架构与实现现状（以仓库真实代码为准）

### 2.1 端到端链路（真实存在的三条链路）
- **Android/RK3288 实时链路**：Java 采集（Camera2/CameraX）→ JNI → C++ `Engine` 做预处理与人脸处理 → 回传 UI/日志。
- **Windows 摄像头识别测试系统**：Media Foundation 采集 → OpenCV 检测/（可选）识别 → 本地 HTTP/SPA 输出与结构化日志。
- **离线/工具链路（现代管线）**：`FaceInferencePipeline`（YOLO 检测 + ArcFace 特征 + 检索 + 阈值策略）已有实现与工具入口，但尚未成为 Android/RK3288 实时默认链路。

关键证据（可审计入口）：
- OpenCL 全局开关：`cv::ocl::setUseOpenCL(true)` 位于 [VideoManager.cpp:L16-L19](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/VideoManager.cpp#L16-L19)
- 外部帧预处理（libyuv 优先、OpenCV 回退）：[Engine.cpp:L138-L265](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/Engine.cpp#L138-L265)
- Android 实时识别主链路仍为 cascade/LBPH： [BioAuth.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/BioAuth.cpp)
- 现代离线管线（含分段计时字段）：[FaceInferencePipeline.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp)

### 2.2 已落地的加速开关/回退点（当前可用）
- **libyuv（预处理）**：编译期开关 `RK_ENABLE_LIBYUV`，实际在 `Engine` 内走 `libyuv::NV21ToRGB24 / I420ToRGB24`，失败回退 OpenCV。
- **ncnn（推理后端，可选）**：编译期开关 `RK_ENABLE_NCNN`，YOLO/ArcFace 已具备 ncnn 与 OpenCV DNN 双路径。
- **回退机制（非性能专用但对稳定性重要）**：VideoManager 支持 primary|backup URL 回退；Engine 外部帧通道失败回退内部采集；Windows 端存在动态跳帧检测以保帧率。

### 2.3 未落地但被文档/待办提及的方向（当前缺口）
- **UMat/OpenCL 透明加速管线**：仓库内未发现 `cv::UMat` 实际使用点；仅存在全局 OpenCL 开关，缺少“命中率/收益/回退”证据链。
- **RK MPP 解码接入**：仓库实现侧未发现 MPP 解码代码；目前视频读取主要走 OpenCV `cv::VideoCapture`。
- **TFLite / Qualcomm 专用推理后端**：仓库实现与构建未发现相关依赖与模块，当前停留在资料引用与方向性描述。

---

## 3. 候选方案可行性评估与关键指标对比矩阵（按链路分段）

> 说明：本节的“收益范围”是经验区间/定性判断，用于指导优先级。最终必须以第 4 章的统一基准输出为准。

### 3.1 对比矩阵（解码 / 预处理 / 推理）

| 段 | 候选方案 | 技术可行性（本仓库落地难度） | 预期收益范围（定性/区间） | 兼容性影响 | 维护成本 | 回退策略（必须） | 主要风险 |
|---|---|---|---|---|---|---|---|
| 解码 | 现状：OpenCV VideoCapture / CameraX 输出 YUV | 低（已存在） | 基线 | 跨平台好，但性能不可控 | 低 | 无（即基线） | CPU 占用可能成为瓶颈 |
| 解码 | RK MPP 硬解码（零拷贝目标） | 高（新增模块/依赖/生命周期治理） | CPU 下降 20%~60%（依赖零拷贝程度） | Android/Linux(RK) 强相关 | 高 | 失败立即回到 VideoCapture/基线 | 驱动/BSP 差异、色彩/stride 问题、排障成本高 |
| 解码 | Android MediaCodec 硬解（文件/网络流） | 中~高（需 Java/NDK 队列与契约） | CPU 下降 15%~50% | Android-only | 中~高 | 失败回退 VideoCapture（或仅 file/stream 场景） | 机型碎片化、格式支持差异 |
| 预处理 | 现状：OpenCV +（可选）libyuv 色转 | 低（已存在） | 基线 | 跨平台好 | 低 | libyuv 失败回退 OpenCV | stride/格式边界导致花屏/崩溃 |
| 预处理 | 扩大 libyuv 覆盖（含 resize/rotate/复用 buffer） | 中 | 1.2×~2.5×（对色转/缩放常有效） | 主要影响 ARM 端 | 中 | 任一算子失败回退 OpenCV | 若拷贝次数不降，收益被吞噬 |
| 预处理 | UMat + OpenCL（透明加速真正落地） | 高（需 UMat 化与命中率治理） | 1.1×~2×（不确定） | 设备差异极大 | 中~高 | 能力探测失败/异常/性能劣化即禁用 | 驱动不稳定、回拷开销导致变慢 |
| 推理 | 基线：cascade/LBPH（实时主链路现状） | 低 | 基线（速度快但精度上限低） | 跨平台好 | 低 | 可作为所有 DNN 失败时兜底 | 精度上限、阈值口径与 DNN 不一致 |
| 推理 | OpenCV DNN（CPU）YOLO/ArcFace | 中（已有组件与入口，缺实时接入与模型治理） | 取决于模型，需轻量化 | 跨平台较好 | 中 | forward 失败回退基线或降级模式 | RK3288 实时性不确定 |
| 推理 | ncnn（CPU，含量化模型） | 中（已有开关与路径，需模型转换与回归） | 1.3×~3×（相对 OpenCV DNN CPU） | 跨平台较好 | 中 | ncnn 失败回退 OpenCV DNN | 模型版本管理、精度漂移 |
| 推理 | Qualcomm 专用后端（NNAPI/QNN/DSP） | 高（新增 SDK/模型/合规） | 1.2×~3×（设备相关） | Qualcomm-only | 高 | 初始化失败回退通用后端 | 合规/再分发限制、收益不可复制 |

### 3.2 “关键指标对比”输出模板（待实测填充）

每个设备（RK3288 与 Qualcomm 各至少 1 台）至少输出以下主表（每行一个配置组合）：

| 设备 | 配置组合（解码/预处理/推理） | FPS(out) | total P95(ms) | infer P95(ms) | CPU(%) | RSS 峰值(MB) | 回退次数/原因 Top1 | 结论（PASS/FAIL/INVALID） |
|---|---|---:|---:|---:|---:|---:|---|---|
| RK3288 | baseline（CPU-only） | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 0 / - | 待实测 |
| RK3288 | libyuv + ncnn | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| RK3288 | OpenCL(AUTO) + … | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |
| Qualcomm | baseline（CPU-only） | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 0 / - | 待实测 |
| Qualcomm | NNAPI/QNN(AUTO) + … | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 | 待实测 |

---

## 4. 基准测量与验收方法（统一口径）

### 4.1 场景覆盖（必须）
- 输入场景：`camera`（真机实时）、`file`（可重复）、`mock`（最小噪声，适合 A/B）
- 分辨率：源分辨率至少覆盖两档（如 720p、1080p）；推理输入至少两档（如 320、640；ArcFace 112）
- 重复：同一配置至少重复 3 次 run；每次 run 记录 raw 与 summary

### 4.2 分段计时点（必须）
每个样本（帧/图片/迭代）记录：
- `decode_ms`、`preprocess_ms`、`infer_ms`、`postprocess_ms`、`total_ms`
并在 summary 中输出 `mean/p50/p95/p99`（验收主用 p95 或 p99）。

### 4.3 “开关是否生效”三段式证据（必须）
任何加速开关都必须同时记录：
1) requested（请求值）  
2) effective（实际生效值，允许回退）  
3) evidence（证据：设备/版本/命中率/回退计数等）

若一个 case 声称启用了加速但 `effective!=requested` 或 evidence 为空，则该 case 标记为 **INVALID（无效对比）**，不得用于“收益结论”。

### 4.4 原始数据与汇总表字段（建议 schema）
建议采用 raw（JSONL/CSV）+ summary（CSV/JSON）双层输出，字段示例：
- raw：`schema_version, run_id, scenario, source_resolution, infer_resolution, decode/pre/infer/post/total_ms, ok, error_code, opencl_requested/effective, libyuv_requested/effective, infer_backend_requested/effective, model_hash12, ...`
- summary：`n, total_p50/p95/p99, stage_p95, fps_in/fps_out, drop_count, error_count, cpu_util, rss_peak, pass_fail, fail_reason, ...`

（完整字段表建议以本报告的模板为准，并作为后续实现阶段的“验收契约”。）

---

## 5. 风险清单与缓解策略（完整版）

> 风险条目格式统一为：触发条件 → 影响 → 检测方法 → 缓解措施 → 回滚/退出策略。  
> 原则：任何“可选加速”只要带来稳定性退化或不可解释的抖动，即使性能更好也不得进入默认配置。

### 5.1 性能类风险

| 风险编号 | 风险描述 | 触发条件 | 影响 | 检测方法（证据） | 缓解措施 | 回滚/退出策略 |
|---|---|---|---|---|---|---|
| R-P01 | OpenCL/UMat 看似开启、实际无收益或变慢 | 算子覆盖不足；频繁 Mat↔UMat 往返；小算子调度开销盖过收益；驱动差异导致回退 | FPS 不升反降；P95 抖动增大；温升/功耗上升 | 同输入 A/B：OpenCL ON/OFF 对比 `total_p95` 与分段 P95；记录 `requested/effective/evidence` 与回退计数 | 仅 `AUTO` 启用；能力探测 + 白名单设备；统计命中率与回退原因；尽量减少回拷与中间结果 | 一键禁用 OpenCL（回到 CPU Mat）；若连续两轮基准无稳定收益，退出该路径 |
| R-P02 | libyuv 优化被 JNI/拷贝/stride 处理抵消 | YUV_420_888 的 stride/pixelStride 导致额外打包；预处理链路仍多次拷贝；最终统一转 BGR 造成额外成本 | 局部耗时下降但总耗时不变；内存峰值上升 | 单独统计 `preprocess_ms` 与 `total_ms`；记录 `preprocess_backend_effective` 与调用计数 | 明确像素格式契约；尽量在 YUV 域完成更多处理；复用 buffer；只在热点通道启用 | 关闭 libyuv，回退 OpenCV `cvtColor`；必要时对问题设备做黑名单 |
| R-P03 | 推理线程/内存策略不当导致吞吐下降 | 线程数>核心数；与采集/渲染抢占；ncnn 配置不当；OpenCV DNN backend/target 选择不当 | 卡顿、掉帧、TTFF 变差 | `inference_bench_cli` 与端到端基准输出 FPS/P95、丢帧与 CPU/RSS；对照不同线程数 | 固化线程上限与优先级；推理降频（stride）；复用内存；把“采集线程优先”写成硬约束 | 强制回退到基线线程配置；必要时降级为“只检测不识别” |
| R-P04 | MPP 硬解码引入后零拷贝未打通，反而更慢 | 解码输出需映射/色转导致多次拷贝；stride/格式协商失败 | CPU 未下降、延迟上升；出现色偏/花屏 | 分段计时：`decode_ms` + 映射/色转耗时；同码流对比 CPU 解码 vs MPP | 先锁定 CPU 基线口径再做可选加速；若无法减少拷贝，限制 MPP 仅用于高码率场景 | 一键切回 CPU 解码；对不兼容码流/设备禁用 MPP |
| R-P05 | Qualcomm 专用后端收益不可复制 | SoC/系统/驱动碎片化；delegate 初始化开销；算子不支持导致回退 | 同版本不同机型表现差异大，验收困难 | 至少 1 台 Qualcomm 纳入矩阵；输出 `effective` 与回退原因分布 | 仅插件式、白名单启用；保留通用后端回退；把能力探测做成硬门禁 | 线上异常立即禁用 delegate，回退通用后端；若两轮收益不稳定，退出该路径 |

### 5.2 稳定性类风险

| 风险编号 | 风险描述 | 触发条件 | 影响 | 检测方法（证据） | 缓解措施 | 回滚/退出策略 |
|---|---|---|---|---|---|---|
| R-S01 | OpenCL 驱动不稳定（崩溃/死锁/泄漏） | 老旧 Mali 驱动；长时间运行；特定算子/尺寸触发驱动 bug | 闪退、黑屏、GPU hang，需要人工重启 | 长稳压测（≥8h，建议 24h+）；失败材料落盘（设备画像+原因码） | 默认不强开；自检失败禁用；熔断：连续失败 N 次自动禁用 | 运行时关闭 OpenCL；版本策略上直接下线该加速路径 |
| R-S02 | libyuv stride/格式边界导致花屏或崩溃 | rowStride/pixelStride 与假设不一致；奇数宽高；buffer 长度不足 | 色彩错误、识别率骤降、native 崩溃 | 输入契约日志：宽高/stride/pixelStride/buffer 长度；异常帧采样落盘 | 发现异常立即走安全回退；把契约写成统一校验函数 | 关闭 libyuv；问题设备黑名单或限定分辨率 |
| R-S03 | 多后端导致输出不一致（精度/阈值口径漂移） | 预处理实现分叉；RGB/BGR/归一化差异；模型版本替换未登记 | 阈值反复重调，线上误报/漏报难解释 | 固定数据集对比：输出一致性（如 embedding cosine/TopK 一致率）；记录 model/preprocess/threshold 版本 | 预处理单一实现+版本化；阈值策略版本化并可回滚 | 回滚到上一版 preprocess/threshold 版本组合；减少后端数量（主选+回退各 1） |
| R-S04 | MPP/硬解缓冲区生命周期与线程安全风险 | 异步 put/get 处理不当；buffer 未释放；跨线程映射访问 | 内存持续增长、视频卡死、不可恢复 | 长稳压测观察 RSS 趋势；记录错误码与重建次数 | 明确所有权与线程边界；失败可恢复：连续失败触发重建解码器 | 关闭硬解回退 CPU；对不可恢复码流拒绝并提示 |

### 5.3 质量/维护成本类风险

| 风险编号 | 风险描述 | 触发条件 | 影响 | 检测方法（证据） | 缓解措施 | 回滚/退出策略 |
|---|---|---|---|---|---|---|
| R-Q01 | 开关组合爆炸导致回归成本失控 | 解码×预处理×推理多维叠加 | 测试矩阵不可控，问题难复现 | 发布时输出“默认组合+支持组合”清单；非默认只白名单测试 | 默认唯一组合；编译期开关（是否包含）+运行时开关（是否启用）分层 | 非基线组合出现 P0 立即移出默认，仅保留实验入口 |
| R-Q02 | 依赖 FetchContent/上游漂移导致不可复现 | 构建期联网拉取；上游变更；镜像不可用 | 同版本不同时间构建产物不一致 | 记录依赖版本/commit；CI 校验依赖一致性 | 锁定 tag/commit；必要时内网镜像；交付版本要求离线可构建 | 关闭 FetchContent 改为预装依赖；或减少可选依赖面 |
| R-Q03 | 证据链缺失导致线上性能问题不可定位 | 只有 FPS/平均值，无分段耗时与回退原因 | 定位周期长、只能试错 | 检查 raw/summary 是否包含分段字段与原因码分布 | 强制落结构化分段指标与回退原因；失败材料落 `ErrorLog/` | 指标未完善前，新加速不得进入默认配置 |

### 5.4 合规/许可证类风险

| 风险编号 | 风险描述 | 触发条件 | 影响 | 检测方法（证据） | 缓解措施 | 回滚/退出策略 |
|---|---|---|---|---|---|---|
| R-L01 | 模型来源/许可证不清 | 模型来自不明来源或限制商用/再分发 | 无法通过法务审查，交付受阻 | 建立模型台账：来源/用途/版本/hash/许可证 | 未审计模型不得进入默认配置；启动自检校验 hash | 替换为已审计模型；无法满足则降级为仅检测模式 |
| R-L02 | 厂商 SDK 再分发条款限制（Qualcomm 等） | EULA 限制二次分发或闭源组件来源不清 | 开源/交付受限，合同风险 | 法务逐条审查 EULA；明确交付边界 | 插件式交付：默认不随通用包分发；客户侧自行获取/安装 | 退出该 SDK 路径，回退通用后端 |
| R-L03 | 新增依赖未登记到 CREDITS | 引入新库但未更新 CREDITS | 审计缺失，合规阻断 | 发布门禁：依赖变更必须更新 CREDITS | 将“依赖登记”作为强制步骤 | 未登记不得发布；必要时回退合并 |

### 5.5 依赖与交付边界（建议）
- **libyuv / ncnn**：属于“可选开源依赖”，建议只在满足可复现构建（版本锁定/来源可审计）的前提下纳入默认构建；任何升级必须出基线对比报告。
- **RK MPP / RGA（若引入）**：属于“平台强绑定依赖”，建议以独立模块形式接入，并默认 `OFF/AUTO`；需要在报告中明确 BSP/内核版本前置条件。
- **Qualcomm 专用运行时（若引入）**：建议以“插件式后端”交付（默认不随通用包分发），并把许可证/再分发限制写入交付说明与 CREDITS 策略。

---

## 6. 推荐决策与路线图（摘要版）

推荐路线图遵循三条硬约束：
1) 先可观测（能看到分段耗时与回退原因）  
2) 再可回退（每个开关独立回滚）  
3) 最后再引入高不确定性加速（OpenCL/专用运行时）  

详细版本（含阶段验收与退出条件、Go/No-Go 门槛、评审签署流程）已整理在 [DEVELOP.md:6.9](file:///d:/19842/Documents/GitHub/rk3288_opencv/DEVELOP.md#L2035-L2206)。

---

## 7. 评审与签署（必须）

进入任何“实现阶段”（修改默认链路/引入新后端/引入新依赖）前，必须至少完成并签署：
- 加速点决策表（默认值、启用条件、白名单、回退条件、原因码示例）
- 基线对比报告（RK3288 + Qualcomm 各至少 1 台；raw + summary + 结论）
- 稳定性/Soak 报告（至少 8h；崩溃/ANR/内存趋势/相机重启）
- 回滚演练记录（如何一键回到基线；触发阈值；证据截图/日志需打码）
- 风险清单与已知问题（含监控阈值与缓解）
- 依赖与许可证更新策略（任何新增第三方必须更新 CREDITS）

---

## 8. 附录：关键文件索引（便于快速定位）

- OpenCL 全局开关： [VideoManager.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/VideoManager.cpp#L16-L19)
- YUV→BGR 与 libyuv 回退： [Engine.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/Engine.cpp#L138-L265)
- Android 实时识别（cascade/LBPH）： [BioAuth.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/BioAuth.cpp)
- 现代推理管线（含分段计时字段）： [FaceInferencePipeline.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp)
- 统一推理基准工具入口（参考）： [inference_bench_cli.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/tools/inference_bench_cli.cpp)
