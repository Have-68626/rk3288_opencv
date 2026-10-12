<!-- DOCSYNC_DOC
type=research
status=snapshot
as_of=2026-09-16
-->

# 开源代码可集成性评估

本文件回答一个比"有什么开源可借鉴"更硬的工程问题：**这些开源东西到底能不能插进本项目的代码里？** 承接 [opensource-benchmark-2026-09-16.md](opensource-benchmark-2026-09-16.md)（该文回答"有什么"：哪些重复、哪些独有、哪些可借鉴），本文件回答"**能不能装上去**"。

三问：能否**快速升级迭代**现有功能、能否**替代**整个功能、可用的开源代码与本项目**开发语言是否适配**（C/C++/Java 优先，性能优化优先）。

> **证据边界**：本文件所有"某项目能做什么"的判断来自其**仓库 README、源码原文、LICENSE 正文或 issue 原文**，**未实际克隆运行过其中任何一项**。所有性能数字均为**对方自述口径**，仅作量级参考。
>
> **2026-09-18 复核修正**：原句称"本项目真机数据尚未采集
> （见 `2026-09-15-documentation-audit.md` 与 `capability-matrix.md` 缺失项）"——
> 该前置结论已被推翻，且不再回指过时缺失项。真机数据已于 2026-09-17 采集
> （见 [rk3288-device-capability-survey-2026-09-17.md](rk3288-device-capability-survey-2026-09-17.md)），
> 2026-09-18 复核。
> **但原句真正成立的证据边界予以保留**：本文件**未实际克隆运行过任何候选项目**；
> 第三方性能数字均为**对方自述口径**，与本项目真机口径不同，**不可直接套用**。
> 核查方法与逐条来源见 §10，未能核实项集中列于 §9。

> **2026-10-12 决策落地（外部决策，权威；与下方 2026-09-16 快照正文并存时以本节点为准）**：
> 本文件候选方向经决策后**收口**如下——
>
> - **活体检测**（§4.4 / §7.5）**列入待办，未实现**：须先核实权重来源与许可，再做自转原型、RK3288 性能与误判验证；**不并入当前 INT8 主线，不宣称已有防伪能力**。
> - **TurboJPEG（`libjpeg-turbo`，§4.3）** **搁置**：先测现有 JPEG 耗时、确认瓶颈与 ARMv7 实测收益后再启动替换。
> - **`ShiqiYu/libfacedetection`（§4.3 / §7.2）** **裁决=不采纳**：保持「APK 不内置模型」策略与 SCRFD 主线（其模型以 C 数组内嵌 `.so`，与该策略冲突）。
>
> 「搁置」与「不采纳」的区别：搁置=暂不做但保留为将来候选，不采纳=结论性排除（除非有新的实测数据）。

---

## 1. 可行性分级方法

上一轮用"重复 / 独有 / 可借鉴"三分法。本文件改用**四档可行性等级**，因为"可借鉴"这三个字掩盖了成本差异——同一个项目可能既有能直接编译进来的 C++ 库，又有只能抄设计的服务端架构。

| 等级 | 含义 | 判定条件 |
|---|---|---|
| **L1 可直接替换** | 编进同一进程、同语言、许可证可商用、接口能被现有接缝吸收 | C/C++/Java 源码 + 宽松许可证 + 已有 `armeabi-v7a` 产物 |
| **L2 需胶水嵌入** | 源码可编，但需写适配层、改构建，或**自行补 32 位支持** | 同上，但接口不匹配或只出 arm64 |
| **L3 仅作设计参照** | 无法编译进来，只能抄架构、协议、参数 | 语言或交付形态不匹配 |
| **L4 不可用** | 排除 | 许可证传染或**闭源专有**、闭源二进制且无源码可补 |

**本项目要同时兼容 `armeabi-v7a` 与 `arm64-v8a`**，因此"候选只支持 arm64"**不算淘汰**，降级为 L2 的一项待补成本。但**实测这条规则适用范围比预期窄得多**——见 §7，多数候选早已交付 armv7，真正需要"自己补"的只有 faiss 与 MiniFASNet 的源码路线。

**硬性判据**（来自 [RK3288_CONSTRAINTS.md](../RK3288_CONSTRAINTS.md)）：

- ABI 需同时覆盖 `armeabi-v7a`（32 位）与 `arm64-v8a`
- Android 7.1.2 = API 25；**禁止 NPU**（不得引入 rknn/rknpu）、**禁止把 Vulkan 当前提**
- 内存峰值 ≤512MB
- 本项目为 **MIT 许可**（`README.md`），因此 AGPL/GPL 依赖是法律障碍而非风格偏好

---

## 2. 语言适配：语言匹配不等于能嵌入

**这是本文件最需要被记住的一条。** 用户的筛选条件是"C/C++/Java 优先"，但实测表明：**语言匹配只是入场券，真正决定能否插进来的是许可证、交付形态与 ABI 三件事**，而它们挡住了绝大多数候选。

### 2.1 完全匹配（C/C++/Java 为唯一主体）

`seetafaceengine/SeetaFace2`、`nihui/opencv-mobile`、`nihui/ncnn-android-scrfd`、`FaceAISDK/FaceAISDK_Android`、`HyperInspire/InspireFace`、`minivision-ai/Silent-Face-Anti-Spoofing-APK`

### 2.2 语言不匹配（主体非 C/C++/Java）

| 项目 | 主体语言（GitHub 字节占比） | 说明 |
|---|---|---|
| `blakeblackshear/frigate` | TypeScript 4.64MB + Python 4.54MB | **零 C/C++/Java** |
| `SharpAI/DeepCamera` | JavaScript 2.32MB + Python 2.02MB | C++ 部分是内嵌 ncnn 副本，非自研（见 §8） |
| `bluenviron/mediamtx` | Go 2.21MB | 无 Java/C++ 集成面 |
| `exadel-inc/CompreFace` | Java 835KB（最大单项）+ TypeScript 631KB + Python 209KB | Java 虽为最大单项，但 TS+Python 合计反超；且用 Maven 非 Gradle，形态是 Docker 服务 |

### 2.3 语言匹配却被挡住的（本文件的重点）

| 项目 | 语言 | armv7 | 挡在哪 |
|---|---|---|---|
| `FaceAISDK_Android` | Java 92% | ✅ AAR 内含 `jni/armeabi-v7a/libFaceAIModel.so`（25,424B，二进制级核实） | **SDK 为专有商业许可**。其 `Document/LICENSE` 原文：demo 源码是 MIT，但 *"The SDK libraries (io.github.faceaisdk:Android) and binary files are subject to their own commercial license and are NOT covered by the MIT License … a valid commercial license is required for production deployment."* |
| `HyperInspire/InspireFace` | C++ + Java | ✅ 官方双 ABI、专用 CI、兼容性矩阵标 Passed | **仓库无任何 LICENSE 文件**（GitHub license API 返回 404）；README 明写模型 *"solely for academic purposes and explicitly prohibiting commercial applications"* |
| `seetafaceengine/SeetaFace2` | C++ | ✅ 四个 `jni/Application.mk` 均为 `APP_ABI := armeabi-v7a` | BSD-2 代码干净，但**最后 commit 2020-02-18**；模型仅百度网盘/Dropbox 分发；ndk-build 路径缺 `SeetaNet/libs/` 与 `SeetaNet/thirdParty/Android/` |
| `exadel-inc/CompreFace` | 边缘 | ❌ 无任何 Android 形态 | Maven + docker-compose 独立服务，**最后 commit 2023-11-14** |

来源：<https://github.com/FaceAISDK/FaceAISDK_Android/blob/main/Document/LICENSE>、<https://github.com/HyperInspire/InspireFace/blob/master/README.md>、<https://github.com/seetafaceengine/SeetaFace2/blob/master/LICENSE>、<https://github.com/exadel-inc/CompreFace>

### 2.4 不要依赖 GitHub 的许可证徽章

多个仓库的 `spdx_id` 字段为 `NOASSERTION`，但 LICENSE 正文明确：`libfacedetection`（实为 3-clause BSD）、`libjpeg-turbo`（IJG + BSD-3）、`mbedTLS`（Apache-2.0 OR GPL-2.0-or-later）、`libsodium`（ISC）、`SeetaFace2`（BSD-2-Clause）均是。**本文件所有许可证结论均以仓库内 LICENSE 文件正文为准。**

---

## 3. "替代本项目功能"：不存在可整包替代的候选

四类候选分别被**许可证**、**交付形态**、**ABI**、**语言**挡住：能嵌入的被许可证挡住，许可证干净的不可嵌入，两者都不挡的语言不匹配。

**"替代"被否后的可行路径**：不是整包替换，而是**按能力域局部换装**——见 §4。

---

## 4. 可升级迭代项

本节列出的项目**全部只评估、不实施**。分层依据是"落在哪个接缝内"。

### 4.1 本项目已有的两类接缝（决定成本高低）

**接缝 1 —— 工厂/依赖注入接缝：换实现成本低。**

`src/cpp/include/pipeline/RealtimeFacePipeline.h` 定义了 `RealtimePipelineHooks`，含六个注入点：`verifyFiles`、`loadDetector`、`loadEmbedder`、`loadGallery`、`selfCheck`、`benchmark`。其中 `loadGallery` 的返回类型被命名为 **`FaceSearchLinearIndex`**——接口名本身为"非线性索引"留了位置。

**接缝 2 —— 读路径原语只有两个：加阶段成本中等偏高。**

`RealtimeFacePipeline` 的读路径只有 `detect()` 与 `embed()`，**没有第三个**。所以活体检测、人脸质量评估这类**新增能力域**吸收不了，须新增读路径原语，再沿 `Engine` → `src/cpp/jni/native_face_bridge.cpp` → Java 层 → UI 串联；单图路径还要在 `src/cpp/include/FaceInferStages.h` 的七个阶段函数里同步加。

> **有利条件**：`embed()` 的入参是 **`alignedFaceBgr`（已对齐的人脸裁剪）**，而主流轻量静默活体模型消费的正是同形态的对齐裁剪——**管线产出的中间产物形态与活体模型的输入要求天然对齐**，缺的只是 API 面与串联。

### 4.2 接入一个新模型后端 = 三个确定性步骤

| 步骤 | 位置 |
|---|---|
| ① 实现接口 | 检测器 `src/cpp/include/FaceDetector.h`（3 个纯虚：`load` / `detect` / `name`）；嵌入器 `src/cpp/include/Embedder.h`（4 个纯虚 + 1 个有默认实现：`load` / `embed` / `embeddingDim` / `name` / `apiVersion`） |
| ② 注册 | `src/cpp/src/ModelRegistry.cpp` 的 `call_once` lambda 内加一次 `registerDetector` / `registerEmbedder` |
| ③ 加入源文件清单 | `cmake/face_infer.cmake` 的 `RK_ADAPTER_SOURCES`——**该清单被 Android / Windows / CLI / 测试四类 target 共同引用**，改一处即全平台生效 |

可直接照抄的最小模板：检测器 `src/cpp/include/adapters/YuNetAdapter.h`（35 行）+ `src/cpp/src/adapters/YuNetAdapter.cpp`（64 行）；嵌入器 `src/cpp/include/adapters/SFaceAdapter.h` + `src/cpp/src/adapters/SFaceAdapter.cpp`（45 行）。

### 4.3 分级清单

| 能力域 | 候选 | 语言 | 许可证 | armv7 | 落在哪个接缝 |
|---|---|---|---|---|---|
| **活体检测** | `minivision-ai/Silent-Face-Anti-Spoofing-APK` | **C++ (ncnn + OpenCV)** | **Apache-2.0** | ✅ 官方 `abiFilters 'armeabi-v7a','arm64-v8a'` + `-DANDROID_ARM_NEON=TRUE` | 接缝 2（**2026-10-12 决策：列入待办，未实现**） |
| 检测（许可证纠偏） | OpenCV Zoo `face_detection_yunet` | C++/ONNX | **MIT** | ✅ | 接缝 1/3（**已注册为 `yunet`**） |
| 识别（许可证纠偏） | OpenCV Zoo `face_recognition_sface` | C++/ONNX | **Apache-2.0** | ✅ | 接缝 1/3（**已注册为 `sface`**） |
| 检测备选 | `ShiqiYu/libfacedetection` | 纯 C/C++，无第三方依赖 | **3-clause BSD** | ⚠️ 无官方 armv7 基准 | ⚠️ 见下（**2026-10-12 决策：不采纳**） |
| 人脸质量评估 | `BSI-OFIQ/OFIQ-Project` | C/C++ | **MIT** | ✅ 自带 `mobile/build_for_android.sh` | 接缝 2 |
| JPEG 编码 | `libjpeg-turbo`（TurboJPEG API） | C + 汇编 | **IJG + BSD-3** | ✅ 有 32 位 NEON，但**缺慢速整数正向 DCT 与降采样加速** | 接缝外（**2026-10-12 决策：搁置**） |
| 构建体积 | `nihui/opencv-mobile` | C++ | **Apache-2.0** | ✅ 官方构建 | ⚠️ 见下 |

**`libfacedetection` 与"APK 不内置模型"策略冲突**：它的模型以 **C 数组内嵌**于 `src/facedetectcnn-data.cpp`，编进 `.so` 就绕过了 ZIP 导入流程与 `checkApkFaceModels` 断言。另需注意其**官方从未公布 ARMv7 基准**——README 的"1000FPS"是 AVX512 多线程 @128×96 的极端数字，ARM 部分只测了 Raspberry Pi 4 的 Cortex-A72（ARMv8 64 位）：320×240 @4 线程为 32.98ms。**RK3288 必须自行实测。**（**2026-10-12 决策：不采纳**——保持「APK 不内置模型」策略与 SCRFD 主线。）

**opencv-mobile 的完整模块账**：本项目 `cmake/opencv.cmake` 的 `BUILD_LIST` 共 **15 个模块**，opencv-mobile 只保留 5 个、**恰好裁掉本项目需要的 10 个**；且 `objdetect` 自 OpenCV 4.5.4 起依赖 `dnn`（[opencv/opencv#22544](https://github.com/opencv/opencv/issues/22544)），而本项目的 `yunet` 与 `sface` 适配器正是用 `cv::FaceDetectorYN` 与 `cv::FaceRecognizerSF`。体积差距确实存在（opencv-mobile 4.13.0 Android 包 **18.9MB** vs 官方 OpenCV 5.0.0 Android 包 **303MB**），但本项目用不上那条裁剪路径。可行做法是"**借配方不借包**"（借 `no-rtti` / `no-zlib` 补丁与选项矩阵，保留自己的模块清单）。

### 4.4 活体检测：唯一能补上真实能力缺口的候选

> **2026-10-12 决策：列入待办，未实现。** 本节与 §7.5 的路线比较作为**未来实施参考**；须先核实权重来源与许可，再做自转原型与 RK3288 性能/误判验证，**不并入当前 INT8 主线，不宣称已有防伪能力**。

| 项 | 事实 |
|---|---|
| 项目 | `minivision-ai/Silent-Face-Anti-Spoofing-APK` |
| 核心文件 | `engine/src/main/cpp/live/live.cpp`——**直接用 `ncnn::Net` + `ncnn::Extractor`** |
| 模型 | `.bin` / `.param` **已预转换好放在仓库里**：`model_1.bin` ≈1.74MB、`model_2.bin` ≈1.73MB |
| 输入 | **80×80**；`config.json` 中 scale 为 **2.7 / 4.0**（MiniFASNetV2@2.7 + MiniFASNetV1SE@4.0 双模型融合） |

**五条理由**：

1. **零新增推理依赖**——它跑在 **ncnn** 上，本项目已有 ncnn
2. **官方同时覆盖两个 ABI**——无需自行移植
3. **输入形态与既有管线天然对齐**——它要 80×80 人脸裁剪，而 `embed()` 的入参正是 `alignedFaceBgr`
4. **模型已预转换**——pth→caffe→ncnn 的转换工具**未开源**，且带 SE 模块的 V1SE 走 onnx→ncnn **会失败**，因此**预转换权重本身就是该仓库的核心价值**
5. **许可证干净**——Apache-2.0

**已知代价**：该仓库构建配置陈旧（作者在 [issue #14](https://github.com/minivision-ai/Silent-Face-Anti-Spoofing-APK/issues/14) 确认用的是 OpenCV 3.4 + ncnn 20200413），移植需重写 CMake；但 `live.cpp` 只用 `load_param(AAssetManager*)` / `from_pixels` / `extract` 等**稳定 API**，改动量小。

**交叉验证**：独立项目 `mudler/face-detect.cpp`（MIT）采用的是**同一对模型**（其 README 原文 "MiniFASNet anti-spoof ensemble (V2@2.7 + V1SE@4.0, 80x80)"），与上述 `config.json` 完全吻合。

**硬约束**：**走 FP16/FP32，不要 INT8**——ncnn 在 armv7 + INT8 上有已知正确性缺陷，详见 §7.3。

---

## 5. 明确不建议清单

本节的价值在于**避免后续走弯路**：以下各项看上去有吸引力，但结论是不做，理由都经过量化。

| 项 | 结论 | 依据 |
|---|---|---|
| **1:N 检索引入 ANN**（hnswlib / faiss） | **不建议** | 10,000 条 × 512 维的全库余弦检索 = 5.12M 次乘加 ≈ **10 MFLOP**，Cortex-A17 走 NEON 的量级是**几毫秒**，1,000 条底库为**亚毫秒**——ANN 换来的加速在此规模下**不存在**，却要付出索引构建、内存膨胀、参数调优（M / ef_construction / ef_search）的成本。且 **hnswlib issue #516 的标题即 "load successed on arm64-v8a, but faild on armeabi-v7a"**，根因是 32 位下 `size_t` 为 4 字节致索引序列化格式不兼容；**faiss 维护者在 issue #3292 明确 32 位平台 "more problematic"**。**保持线性检索**，仅当底库超过约 10 万条时再议 |
| **加密库替换**（libsodium / mbedTLS / OpenSSL） | **不建议** | **`libsodium` 的 AES-256-GCM 在 RK3288 上根本跑不起来**——官方文档要求 Intel AES-NI 或 **ARM Crypto Extensions**，而 Cortex-A17 二者皆无，且官方明确"无计划支持非硬件加速实现"。RK3288 的独立硬件加密引擎支持 AES 的 ECB/CBC/OFB/CTR/CMAC/XCBC/XTS/**CCM**——**独缺 GCM**，且经内核 crypto API 访问，Android 用户态难用。三个库在此平台提供的 GCM 均为纯软件实现，**不会比现有方案快** |
| **视频传输层替换** | **不建议** | MJPEG/SSE 分帧本身不是瓶颈，**瓶颈是每帧 JPEG 编码**。且 `jacksonliam/mjpg-streamer`（3.2k★）**无任何许可证声明**，法律上属"保留所有权利"，**仅可作参考不可用于产品** |
| `FaceAISDK` / `InspireFace` 集成 | **不建议** | 许可证（专有商业 / 无 LICENSE 且禁止商用），见 §2.3 |
| `frigate` / `mediamtx` / `DeepCamera` | **不建议** | 语言与交付形态双重不匹配；DeepCamera 的 Android 路径仅 aarch64 且需 Python 2 + TVM |
| `SeetaFace2` 集成 | **不建议**（可作 L3 参照） | 停更 6 年 + 模型分发无官方通道 + ndk-build 路径缺件；且 **issue #82 自述 Android 上特征提取耗时超过 10s**。⚠️ 其**模型权重许可证未能核实**——仓库内无针对模型的独立许可声明 |

来源：<https://github.com/nmslib/hnswlib/issues/516>、<https://github.com/facebookresearch/faiss/issues/3292>、<https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/aes-256-gcm>、<https://github.com/seetafaceengine/SeetaFace2/issues/82>

---

## 6. 跨语言重写的收益边界

**问题**：若某项目的某部分实现是最优解或最优工程实践，参考它并将其用 C/C++/Java 重写，性能收益如何？

**核心论点**：**"抄它的做法"与"把它的语言换成 C/C++"是两件收益来源完全不同的事，混为一谈会投入大量工时换来接近零的收益。**

### 6.1 决定性反例

`mudler/face-detect.cpp` 是**从零开始的 C++17 重写版 insightface 流水线**（基于 ggml）。其作者本人报告：**在 CPU 上比 onnxruntime 慢**——

| 环节 | 1 线程 | 8 线程 |
|---|---|---|
| SCRFD 检测 | **0.83x** | **0.69x** |
| ArcFace 嵌入 | **0.61x** | **0.84x** |

**一个 C++ 重写跑到比 Python 包装层更慢。** 原因不复杂：Python 那层从来不是瓶颈，它调用的是 onnxruntime 中高度手工调优的原生内核；而 ggml 需要从头重新挣得那些优化。

### 6.2 按"收益来源"分类的量级

| 收益来源 | 典型量级 | 在本项目是否成立 |
|---|---|---|
| **换更小或更合适的模型**（算法级） | **1.5x – 4x** | ✅ **最大的一块**——如 SCRFD 500m vs 10g、512 维 vs 128 维嵌入 |
| **消除每帧拷贝与分配** | 720p 下约 1–3 ms/帧，对 30fps 预算是 **3–10%** | ✅ 但属**架构缺陷**，与语言无关 |
| **消除跨进程 / FFI 边界** | 1.2x – 2x（取决于序列化量） | ⚠️ 仅当候选是独立服务时 |
| 解释器循环 → 编译代码 | 10x – 100x | ❌ **现在几乎不存在此类代码**——成熟项目底层均为 numpy / cv2 / onnxruntime |
| **语言本身**（两边调用同一批原生内核） | **1.0x – 1.1x** | ❌ **这就是本题的场景** |

> 上表为**量级估计**，非实测数据。

### 6.3 为什么"语言重写"在本项目收益接近零

1. **候选项目里真正吃 CPU 的部分已是原生**——insightface 走 onnxruntime、FaceAISDK 走预编译 `.so`、CompreFace 的 `embedding-calculator` 是 Flask 包 insightface；**Python/JS/Go 那层是胶水**
2. **瓶颈通常在拷贝与分配，不在算术**——而重写不会自动消除拷贝
3. **本项目已依赖 ncnn 与 OpenCV，两者都有 ARM NEON 汇编级优化**——自己重写同功能代码几乎必然比上游慢。SeetaFace2 是另一例证：纯 C++、自研 SeetaNet、不依赖任何第三方库，但 RK3399 上 ResNet50 识别只有 **2.5 FPS**
4. **工具链不是差异项**——Android NDK 对 `armeabi-v7a` 在 API 21+ **默认启用 NEON**

### 6.4 重写真正划算的四种情形

| 情形 | 收益来源 |
|---|---|
| 上游是**纯解释器循环**且无原生库可替代 | 真·语言收益（此类代码已罕见） |
| 上游是**独立进程/服务**，要消掉跨进程通信与序列化 | **边界消除**，不是语言收益 |
| 上游**许可证不允许用其二进制**，但算法公开 | **动机是法律，不是性能** |
| 上游**不支持本项目 ABI** 且只提供汇编或闭源 `.so` | 自行实现 NEON 路径（承 §7） |

**须一并计入的成本**：跨语言重写意味着**维护一份 fork、与上游持续优化脱节**。对本项目这种依赖 ncnn / OpenCV 等重型上游的项目，这笔账几乎总是负的。

**结论**：真正该"参考后重写"的是**算法选择、拷贝消除、参数与门控策略**；不该"参考后重写"的是**推理内核本身**——后者已有 ncnn / OpenCV / ONNX Runtime 的 NEON 汇编，自研是净损失。

**一个符合"该抄"特征的实例**：DeepCamera 的 `face_motions` 面部动作判定逻辑是**用 JavaScript 写的活体状态机**。它符合"该抄"的三个特征——是**逻辑而非计算**（没有原生库在做这件事）、本体很小、且本项目**已透传检测关键点**。但须注意：**它带来的是能力，不是性能**。

---

## 7. 32 位 ARMv7 移植代价

### 7.1 卡点分类

`(a)` 用 NEON intrinsics 但混用 A64 专有指令（需改写）/ `(b)` 依赖只有 arm64 的第三方 / `(c)` 依赖硬件特性（ARMv8 crypto / dotprod / fp16 / i8mm）/ `(d)` 纯属没人构建过 / `(e)` 闭源二进制无源码。

> **一条需要纠正的直觉**：移植成本**不是**主要来自 ARMv8 专有硬件特性。实测中**卡点 `(c)` 一次都没有出现**，没有任何候选真正依赖那批指令；真实卡点全是 **(a) 与 (d)**。而且 **A64-only intrinsic 多有直接的 armv7 等价物**（如 `vfmaq_f32` → `vmlaq_f32`），改写是机械的。

### 7.2 逐项代价

| 候选 | 当前 armv7 状态 | 卡点 | 量级 | 值得做？ |
|---|---|---|---|---|
| `HyperInspire/InspireFace` | 官方 armv7 预编译 + 专用 CI + 兼容性矩阵标 Passed | 无 | **0 小时** | ✅（但许可证禁止商用） |
| `microsoft/onnxruntime` | AAR 四 ABI 含 `armeabi-v7a`，2023→2026 连续交付 | (a) 偶发 MLAS A64 指令 | **0 小时**（预编译） | ✅ 通用后端对照成立 |
| `FaceAISDK` | AAR 内含 `jni/armeabi-v7a/libFaceAIModel.so` | (e) 闭源但已交付 | **0 小时** + 数小时验证 | ⚠️ 许可证专有；自述支持下限 Android 8，API 25 未核实 |
| `nmslib/hnswlib` | 无 NEON，纯标量零改动可移植 | (d) | **0 小时** / 几小时移植 PR #657 | ✅ 但本项目不该用 ANN（见 §5） |
| `seetafaceengine/SeetaFace2` | `-DANDROID_ABI="armeabi-v7a with NEON"` **即其默认**；缺的是 arm64 | (d) | **几小时** | ⚠️ 可移植性无问题，**性能致命**（issue #82 称 >10s） |
| `ShiqiYu/libfacedetection` | demo 双 ABI + `ENABLE_NEON`；官方脚本仅 arm64 | (d) + 少量 (a) 编译标志 | **几小时 – 1 天** | ❌ 不采纳（2026-10-12 决策；见 §4.3） |
| MiniFASNet 静默活体 | 路线 A 有 armv7 预编译 `.so`；路线 B 需自转 | (e) 路线 A / (d) 路线 B | 几小时（A）/ **几天**（B） | ⏸ 待办（2026-10-12 决策：未实现；路线 B 为推荐实施方案） |
| `facebookresearch/faiss` | 无 NEON；SIMD 分支仅 aarch64；32 位数据模型需改 | (d) + **LP64 数据模型** | **几周** | ❌ **不值得**，改用 hnswlib |

### 7.3 三条跨候选结论

**① 唯一"不值得"的是 faiss，且原因不是 SIMD，而是 LP64 数据模型**——`size_t` / `long` / `1L << 40` 这类字面量在 32 位下溢出。**SIMD 是可选的**（标量路径照样编译通过），**数据模型是不可选的**。佐证：fork `luojinlongjjj/android-faiss` 把这些改动全做了一遍，README 仍自述 *"32位并不能正常运行……但并不保证运行一定稳定"*；官方 issue #1059「Faiss for mobile devices」**开了近 7 年无人解决**。

**② "只支持 arm64"是少数情况**——InspireFace、FaceAISDK、onnxruntime、SeetaFace2 都已交付 armv7；hnswlib 零改动可跑；libfacedetection 只差编译标志。真正需要"自己补"的只有 **faiss** 与 **MiniFASNet 的源码路线**。

**③ 参考实现已备好**——onnxruntime 的 PR #25955 / #26161 是"如何把 A64 指令改写成 armv7 等价物"的现成教材（`vfmaq_f32` → `vmlaq_f32`），该改写模式可套用到任何遇到的 A64-only intrinsic。

### 7.4 ncnn 在 armv7 上是一等公民

- `.github/workflows/android.yml` 明确构建 `-DANDROID_ABI="armeabi-v7a" -DANDROID_ARM_NEON=ON`（静态与 shared 各一档）
- `CMakeLists.txt` 有 `option(NCNN_VFPV4 "optimize armv7 platform with vfpv4" ON)`，而**Cortex-A17 有 VFPv4**
- **走 ncnn 路线意味着不写一行 intrinsics**，SIMD 全部下沉给 ncnn 的手写 armv7 NEON 层

**必须记住的坑**：**ncnn 在 armv7 + INT8 上有历史正确性缺陷**——issue [#547](https://github.com/Tencent/ncnn/issues/547)（arm 开 int8 检测不到目标）、[#1793](https://github.com/Tencent/ncnn/issues/1793)（ARM 上量化模型输出与 x86 差异巨大，`opt.use_packing_layout = false` 解决）、[#2936](https://github.com/Tencent/ncnn/issues/2936)（armv7 INT8 与 x86 不一致，定位到 `split` 之后）、[#2952](https://github.com/Tencent/ncnn/issues/2952)（**RK3288 上 retinaface INT8 无任何人脸框输出**，FP32 正常 60ms、INT8 33ms 但空结果）、[#3224](https://github.com/Tencent/ncnn/issues/3224)。**走 FP16/FP32，不要 INT8。**

### 7.5 活体检测的两条实现路线

> **2026-10-12 决策：列入待办，未实现。** 下面的路线比较作为**未来实施参考**；**不并入当前 INT8 主线，不宣称已有防伪能力**。路线 B（自转，拥有代码）仍是推荐方向，但须先完成权重来源与许可核实。

| 路线 | 做法 | 代价 | 风险 |
|---|---|---|---|
| **A** | 直接用 `oldprincess/Android-Silent-Face-Anti-Spoofing` 的 armv7 预编译产物 | 几小时 | **`libengine.so` 的源码不在仓库里**（自述"编译了原项目的 c++ 代码，可以直接复制 jniLibs 中的 so 库"），等于接受闭源引擎，且被 3 个 OpenCV `.so` 绑架 |
| **B（推荐）** | 自己做 pth → onnx/ncnn 转换 + 预处理对齐 + FP16 数值验证 | **几天** | 无结构性阻塞，**且只有这条路线你拥有代码** |

权重来源：`minivision-ai/Silent-Face-Anti-Spoofing`（Apache-2.0）**把权重直接提交在仓库里**（`resources/anti_spoof_models/` 下的 `2.7_80x80_MiniFASNetV2.pth` 与 `4_0_0_80x80_MiniFASNetV1SE.pth`）。转换时**必须对齐的契约**是 `src/model_lib/MiniFASNet.py` 与 `src/anti_spoof_predict.py` 中的预处理 / crop 契约及 **2.7 / 4.0 两个尺度因子**。

---

## 8. 以 DeepCamera 为样本的跨平台平台对照

**问题**：`SharpAI/DeepCamera` 是否是这批项目里最优秀最完善的（语言除外）？**经核实：不是**，且理由比"语言除外"更根本。

### 8.1 决定性核实

对 DeepCamera 文件树做实测统计：

```text
C/C++ 文件总数            : 286
其中位于 3rdparty/ncnn 内 : 200
自研 C/C++ 文件数         :  86
```

那 86 个的构成是：`include/ncnn/*.h`（ncnn 头文件）、`pybind11/*.h`（**另一个 vendored 第三方**）、以及打包进来的 MTCNN 检测器目录（`assets/detection/face_detector/`）。

**即该仓库中全部 C/C++ 都是第三方代码或其搬运**，它自研的是 JavaScript 2.32MB + Python 2.02MB。**因此"语言除外"这个前提本身消解**——若拿掉语言约束，DeepCamera 恰是这批里"C/C++ 工程完善度"最低者之一，**因为它的 C/C++ 不是它的**。

### 8.2 按其他轴也不是第一

| 轴 | 领先者 | DeepCamera |
|---|---|---|
| 星标 | frigate **35,923** > mediamtx **20,155** > CompreFace 8,312 | 3,062 |
| 活跃度 | frigate / mediamtx **2026-09-15 当天仍有提交** | 最后提交 2026-04-21 |
| 可部署产品完成度 | frigate（NVR + Web UI + MQTT/HA）、CompreFace（Docker + REST + UI + 角色管理） | 平台形态 |
| 单点工程质量 | opencv-mobile、SeetaFace2、ncnn-android-scrfd | — |

它**唯一真正领先的一维是平台广度**：设备端 + 服务端 + 桌面端，以及推理后端与硬件的多路自适应。

### 8.3 架构骨架

- **`skills/` 是其插件体系**，分类为：`analysis`、`annotation`、`automation`、`camera-providers`、`channels`、`detection`、`integrations`、`lib`、`segmentation`、`streaming`、`training`、`transformation`
- **`skills/detection/`** 下为：`yolo-detection-2026`、`-coral-tpu-macos`、`-coral-tpu-win-wsl`、`-openvino`——**按"加速器 × OS"组合各打一个包**
- **`skills/camera-providers/`** 下为 `eufy`、`reolink`、`tapo`——**按消费级品牌接相机**，不是按协议
- **`skills/streaming/`** 下为 `go2rtc-cameras`——转调外部 Go 项目
- 源码树内含 Milvus（向量库）与 MinIO（对象存储）——1:N 检索走**外部服务**

### 8.4 可借鉴 vs 应避开的负担

| 项 | 判断 |
|---|---|
| **"把任务与执行它的硬件/运行时组合分开"这一组织思路** | ✅ **可借鉴**——与本项目加速契约的动机一致，但实现层次不同（它是**打包分片**，本项目是**运行时抽象**） |
| 把"多后端"实现为**每种加速器 × OS 各打一个包** | ❌ **负担**——本项目只有**一个目标硬件**，运行时抽象（`requested` / `effective` / `reason`）比打包分片更合适 |
| 按**消费级品牌**接相机 | ❌ **不适用**——本项目走 MF / UVC / CameraX，是设备内相机而非 IoT 网络相机 |
| 把 **Milvus + MinIO 做成服务** | ❌ **负担**——见 §5，本项目规模下线性检索仅数毫秒，引入向量库是净损失 |
| **Python 2 运行时** | ❌ **反面信号**——Python 2 已于 2020 年 EOL |
| 构建产物目录与 574 个 `assets/` 文件在版本树中 | ❌ 仓库卫生反面样本（对比本项目 `scripts/clean-repo-junk.js` 门禁） |

### 8.5 定性

DeepCamera 在这批里**唯一特殊之处是"曾经专门尝试支持 RK3288"**（其 `docs/RunOnRK3288.md` 确实存在），所以每当谈 RK3288 都会撞到它。但那条路是 **Termux Linux 用户态 + Python 2 + TVM 0.5.dev**，配 `LD_PRELOAD=libatomic.so:libcutils.so` 变通与 2018 年的开发镜像，**非原生 Android 且多年未更新**。

**结论**：它是**相关性最高**的项目，不是**质量最高**的项目。"最完善"在这个语境下是错的评价轴——**"完善"通常等于"覆盖场景多"，而覆盖场景多意味着依赖多、运行时重、假设硬件强**；对一个以"2GB RAM / CPU-only / 单一 SoC / APK 不内置模型"为身份的项目，DeepCamera 的每一条"完善"特征都是负担。

---

## 9. 未能核实项

以下字段**没有查到**，不得当作确定结论使用：

1. **FaceAISDK 已发布 AAR 的真实 `minSdk`**——仓库模块写 `minSdk 21`，README 写 "Android [8, 16]"，两者矛盾；AAR 内 `AndroidManifest.xml` 是压缩二进制 XML，未解析出明文值
2. **FaceAISDK 在 Android 7.1.2（API 25）上是否真能运行**——manifest 覆盖 API 25，但自述支持下限为 Android 8
3. **SeetaFace2 四个模型文件的实际体积**——仅有百度网盘 / Dropbox 链接，仓库内无官方 HTTPS 发布通道
4. **SeetaFace2 / SeetaFace6 模型权重的许可证**——代码为 BSD-2-Clause，但模型文件**无独立许可声明**
5. **InspireFace 代码本体的 SPDX**——仓库无 LICENSE 文件，GitHub license API 返回 404（其**模型的"仅学术、禁止商用"**是 README 明文，已确认）
6. **`nihui/ncnn-android-scrfd` 自身示例代码的授权**——仓库无 LICENSE 文件；依赖项 ncnn=BSD-3-Clause、opencv-mobile=Apache-2.0 已核实
7. **`libfacedetection` 的 `highway/` 后端在 armv7 上是否验证过**——Highway 本身支持 `HWY_ARCH_ARM_V7`，但该仓库侧无证据
8. **`BSI-OFIQ/OFIQ-Project` 的依赖许可证**——其 LICENSE.md 自述 *"includes dependencies and files that may be licensed otherwise"*，落地前须逐项审计
9. **`mudler/face-detect.cpp` 的 armv7 可行性**——其 CMakeLists 与 README 均无 ARM / NEON 痕迹
10. **`libfacedetection` 在本项目目标 SoC 上的实测速度**——官方只有 AVX512 与 Raspberry Pi 4（Cortex-A72 / ARMv8）基准，无 ARMv7 数据

---

## 10. 引用来源

**核查方法**：四路并行调研——① 本地集成接缝勘察（含 `graphify` 图查询定向）；② 候选项目语言 / 许可证 / ABI 元数据核实；③ 能力级替换候选（7 个能力域）；④ 32 位 ARMv7 移植代价。联网部分以已鉴权的 `gh` CLI 取元数据、`raw.githubusercontent.com` 读源码原文、**HTTP Range 解析 Maven AAR 的 ZIP 中央目录**做二进制级 ABI 核实。

**一手核实（2026-09-16）**：本文件所有星标 / 许可证 / 最后推送 / AAR 内 ABI 条目 / C-C++ 文件计数。

**项目页面**：

- Silent-Face-Anti-Spoofing-APK — <https://github.com/minivision-ai/Silent-Face-Anti-Spoofing-APK>
- Silent-Face-Anti-Spoofing（权重来源）— <https://github.com/minivision-ai/Silent-Face-Anti-Spoofing>
- Android-Silent-Face-Anti-Spoofing（armv7 预编译产物）— <https://github.com/oldprincess/Android-Silent-Face-Anti-Spoofing>
- FaceAISDK_Android — <https://github.com/FaceAISDK/FaceAISDK_Android>
- InspireFace — <https://github.com/HyperInspire/InspireFace>
- SeetaFace2 — <https://github.com/seetafaceengine/SeetaFace2>
- libfacedetection — <https://github.com/ShiqiYu/libfacedetection>
- opencv-mobile — <https://github.com/nihui/opencv-mobile>
- ncnn-android-scrfd — <https://github.com/nihui/ncnn-android-scrfd>
- OpenCV Zoo（YuNet / SFace / eDifFIQA）— <https://github.com/opencv/opencv_zoo>
- OFIQ-Project — <https://github.com/BSI-OFIQ/OFIQ-Project>
- libjpeg-turbo — <https://github.com/libjpeg-turbo/libjpeg-turbo>
- hnswlib — <https://github.com/nmslib/hnswlib>
- faiss — <https://github.com/facebookresearch/faiss>
- onnxruntime — <https://github.com/microsoft/onnxruntime>
- DeepCamera — <https://github.com/SharpAI/DeepCamera>
- mudler/face-detect.cpp — <https://github.com/mudler/face-detect.cpp>

**技术依据**：

- insightface 模型仅限非商用 — <https://github.com/deepinsight/insightface/blob/master/model_zoo/README.md>、<https://github.com/deepinsight/insightface/issues/2486>
- `objdetect` 依赖 `dnn` — <https://github.com/opencv/opencv/issues/22544>
- ncnn armv7 INT8 正确性缺陷 — issues #547 / #1793 / #2936 / #2952 / #3224
- libsodium AES-GCM 需硬件加速 — <https://libsodium.gitbook.io/doc/secret-key_cryptography/aead/aes-256-gcm>
- hnswlib armeabi-v7a 索引加载失败 — <https://github.com/nmslib/hnswlib/issues/516>
- faiss 32 位平台支持 — <https://github.com/facebookresearch/faiss/issues/3292>
- onnxruntime A64 → armv7 指令改写范式 — PR #25955 / #26161
- Android NDK 对 armeabi-v7a 默认启用 NEON — <https://developer.android.com/ndk/guides/cpu-arm-neon>

**相关文档**：模型许可证台账见 `CREDITS.md`；约束出处见 [RK3288_CONSTRAINTS.md](../RK3288_CONSTRAINTS.md)；上一轮三分类结论见 [opensource-benchmark-2026-09-16.md](opensource-benchmark-2026-09-16.md)。
