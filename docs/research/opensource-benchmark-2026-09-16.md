<!-- DOCSYNC_DOC
type=research
status=snapshot
as_of=2026-09-16
-->

# 开源对标调研（核实版）

本文件核实一份外部生成的《rk3288_opencv 开源对标调研报告（修订版 v2）》，并补充其遗漏的对标项目，回答三个问题：**哪些是重复开发、哪些是独有的、哪些可以借鉴**。

> **证据边界**：所有仓库数据（星标 / 许可证 / 最后推送）由 GitHub API 于 2026-09-16 实测；所有"某项目能做什么"的判断来自其**仓库 README、官方产品页或 issue 原文**，**未实际克隆运行过其中任何一项**。涉及性能数字的均为对方自述口径，仅作量级参考。

---

## 1. 对原报告的核实结论

原报告方向大体正确，但存在**三类系统性偏差**。

### 1.1 星标普遍低估，且未标注废弃状态

| 项目 | 原报告称 | 实测 | 最后推送 |
|---|---|---|---|
| `nihui/opencv-mobile` | ~2k | **3358** | 2026-07-12 |
| `SharpAI/DeepCamera` | ~2k | **3062** | 2026-06-18 |
| `exadel-inc/CompreFace` | ~5k | **8312** | **2024-10-05** |
| `blakeblackshear/frigate` | ~22k | **35920** | 2026-09-15 |
| `bluenviron/mediamtx` | ~12k | **20146** | 2026-09-15 |
| `airockchip/rknn_model_zoo` | ~1k | **2759** | 2025-04-09 |
| `YuGongCharley/MTCNN_facenet_Android` | ~0.02k | 24 | **2020-08-14** |
| `791136190/ncnn_android_face_vehicle` | ~0.06k | 60 | **2020-05-14** |
| `zhu260824/face_ncnn` | — | 82 | **2020-05-11** |

原报告把若干"高相似度"对标列为可借鉴项，但**其中三份已废弃五年以上**（2020 年后再无提交），且报告全文未提及活跃度。**活跃度是选型的一等指标**，仅看相似度会误导。

### 1.2 完全漏掉最相关的活跃项目

| 项目 | 星标 | 最后推送 | 为什么重要 |
|---|---|---|---|
| `FaceAISDK/FaceAISDK_Android` | **1261** | **2026-08-30** | Android 端侧离线人脸检测/识别/1:N 检索 + **活体（动作与静默）** + 人脸图质量检测 + UVC 双目 IR 活体；与本题技术栈高度重合 |
| `seetafaceengine/SeetaFace2` | **2264** | 2024-03-15 | 全栈人脸工具包，**不依赖任何第三方库**，支持 Android ARM，含独立的 `QualityAssessor` 与 `FaceTracker` 模块 |
| `HyperInspire/InspireFace` | 273 | 2026-09-09 | 跨平台 C/C++ 人脸 SDK，Android armv7/arm64 均有预编译库与 Java/JNI 封装，可作"引擎 + JNI 分层"的直接架构对照 |
| `nihui/ncnn-small-board` | 166 | 2023-10-19 | **收录了 RK3288 的公开 benchncnn 成绩**，可作基准校准 |

**`FaceAISDK` 的一条自述尤其值得注意**：其 README 写明"特殊定制硬件如 **RK3288** 等适配需要兼容适配"，并建议设备配置"八核 64 位 2.4GHz 以上"。RK3288 是**四核 A17 1.8GHz**，低于其推荐线。这反过来说明本项目在 RK3288 上跑通并非"别人早已做过"，而是确实踩在更低的硬件线上。

### 1.3 一条核心建议在技术上不成立（本轮最重要的修正）

原报告建议："**评估迁移到 opencv-mobile 稳定基线以降低长期维护成本**"。

实测本项目 `cmake/opencv.cmake:50` 的 `BUILD_LIST` 与 `:161-164` 的链接清单，共 **15 个模块**：

```
core  imgproc  imgcodecs  objdetect  features2d  flann  calib3d  dnn
ml  photo  face(contrib)  video  videoio  highgui  stitching
```

opencv-mobile **只保留 5 个**（`core` / `imgproc` / 定制 `highgui` / `features2d` / `video`），**恰好裁掉本项目需要的 10 个**。它官方明确把 `opencv_dnn` 列入移除名单（理由："mobile 上很慢，请用 ncnn"），`opencv_objdetect` 同样移除。

而 **`objdetect` 自 OpenCV 4.5.4 起依赖 `dnn`**（[opencv/opencv#22544](https://github.com/opencv/opencv/issues/22544)）——因为 `cv::FaceDetectorYN` 与 `cv::FaceRecognizerSF` 被并入 dnn 体系，而本项目的 `yunet` 与 `sface` 两个适配器正是用这两个 API。

**结论**：迁移 opencv-mobile 意味着把裁掉的 10 个模块再加回去，其中 `dnn` 正是体积大头，等于放弃 opencv-mobile 的全部价值。**该建议在当前模块清单下不成立。**

可行的两条路（**尚未决策**，需先量化代价）：

| | 路 A：只借配方 | 路 B：收敛适配器换瘦身 |
|---|---|---|
| 做法 | 保留 15 模块与全部适配器，只借鉴 opencv-mobile 的构建配方（`no-rtti` / `no-zlib` 补丁、stb 版 highgui、选项矩阵） | 砍掉全部依赖 OpenCV-DNN 的适配器（`yunet` / `sface` / `dnn_ssd` / `face`(LBPH)），全走 ncnn；模块清单可大幅收敛 |
| 代价 | 体积收益有限（`dnn` 省不掉） | 放弃上述备选模型；需改适配器、测试与文档 |
| 迁移是否划算 | 否 | 待体积与构建时长实测 |

### 1.4 另一条建议方向正确但缺具体参照

原报告建议"把 Windows 本地服务降级为本地 REST/WebSocket 适配层"。方向对，但未给出可核对的参照。补充：本类项目的 REST 设计已高度趋同（详见 §3.3），可直接对照本项目 `docs/windows-web-spa/openapi.yaml` 做端点命名与语义对齐。

---

## 2. 哪些是重复开发

### 2.1 Android「相机 + JNI + ncnn + OpenCV」胶水层 —— 确认重复

`nihui/ncnn-android-scrfd`（263★）展示的 NDK Camera + ncnn + opencv-mobile 模式确实成熟；**更贴近的是 `FaceAISDK`**（同为 CameraX + 端侧离线识别 + 1:N 检索）。

**但要注意一个不可替代的差异**：这些项目的推理后端是 ncnn / TFLite，跑在主流手机 SoC 上；本项目的约束是 **RK3288 四核 A17 + 2GB RAM + CPU-only + Android 7.1.2**。**胶水层可以借鉴，硬件适配层没有现成的。**

### 2.2 OpenCV 移动端构建 —— 见 §1.3，报告在此过度乐观

### 2.3 「设备—本地服务—Web」三层形态 —— 确认重复，且比原报告说的更普遍

原报告只举了 CompreFace 与 iot-facerecognition。实测这一类至少有 5 个活跃项目，形态高度雷同：

| 项目 | 形态 | 与本题的雷同点 |
|---|---|---|
| CompreFace（8312★） | Docker + REST + UI + 角色管理 | **要求 x86 + AVX**；ARM64 支持有 PR 但**未发布 Docker 镜像**，社区在 RPi5 上构建持续失败 |
| `anhnvme/facedetect`（IRIS） | 本地 REST + Web UI，明示"数据不出本机" | Python/ONNX，Docker Compose 一键起 |
| Raray Vision | FastAPI + Vue3 + JWT | **API 路径同为 `/api/v1/faces/*`**，同为多租户 |
| VisageAI | Next.js + pgvector | **明示 CPU-only、无需 GPU**，边缘优化 + Web 仪表盘 |
| ZepIris | 双微服务 + Milvus + MinIO | 多租户 + 质量/防伪检查 |

**这一层没有技术独特性可言**，属于成熟范式。

### 2.4 评测工具链 —— 非独有

ncnn 自带 `benchncnn`，opencv-mobile 有构建矩阵。自建 `inference_bench_cli` 有工程价值，但"系统评测"本身不是从零发明。

### 2.5 项目自身的内部重复（对标范围之外，但属于同类问题）

调研中被忽略的一类"重复开发"：**本项目内部就存在两套并行的「检测 + 对齐 + 识别」编排**。

| 编排 | 位置 | 使用者 |
|---|---|---|
| `runFaceInferOnce()`（单图） | `FaceInferencePipeline` + `FaceInferStages` | `src/cpp/main.cpp` 的 `--face-infer` CLI、`src/cpp/jni/config.cpp` 的 `nativeInferFaceFromImage`、测试 |
| `RealtimeFacePipeline`（实时） | `src/cpp/src/pipeline/RealtimeFacePipeline.cpp` | `Engine`（仅 `__ANDROID__` 下默认装配） |

**`Engine` 完全不调用 `FaceInferencePipeline`**（`Engine.h`/`Engine.cpp` 中零命中）。二者各自实现同一套 detect → align → embed → search 流程，共用底层的 `FaceAlign` / `ModelRegistry` / `FaceSearch`，但编排层是两份。

这不是"重复造轮子 vs 开源"，而是**项目内部的重复**，评估维护成本时应当计入。

---

## 3. 哪些是独有的

### 3.1 「加速契约」不是独有（原报告判断有误）

原报告称 `requested` / `effective` / `evidence` / `reason` 四元组"真独特（需内部证明）"。核实后**该判断不成立**——至少 5 个独立项目实现了同一模式，表述几乎一致：

| 项目 | 实现 |
|---|---|
| kailash-py `specs/ml-backends.md` | `detect_backend(prefer=...)`，明文写 **"Silent fallback to CPU is BLOCKED"**，未命中即抛 `BackendUnavailable` 并附 `reason` |
| Particula | `FallbackPolicy` / `FallbackBoundary` / `CPUStateAuthority` + **reason 码表**，逐码规定调用方动作 |
| MCOP-Framework `Accelerator.ts` | `AcceleratorProvenance` 含 `requestedDevice` / `verifiedDevice` / `fallback` / `fallbackReason` / `resolvedFrom`，并做"ghost-GPU 检测" |
| scena | `HeadlessBackendSelectionReport` 记录"请求的后端 / 实际选中的后端 / 是否发生回退 / 原始错误" |
| aidekin | `resolveWidgetCapabilities(r, requested)` → `effectiveMode` + `reason` |

kailash-py 给出了同一动机的清晰表述：**"静默回退是'我的训练为什么这么慢'事后复盘的头号原因"**。

**这仍是有价值的工程实践**，但它是被反复独立发明的通用模式。要保留其价值，应转成**可验证规格**（机器可读 manifest + CI 断言），而非仅存在于代码结构里。

**本地核实补充（2026-09-16，代码级）**——本项目的落地程度**强于我的第一轮判断，但仍低于文档自述**：

| 核查项 | 实测 |
|---|---|
| 契约结构体 `AccelContractStatus` 的实例化点 | 仅 1 处文件（`Engine.cpp` 的 `performAccelSelfCheck`），覆盖 ncnn / mpp / qualcomm / opencl / libyuv 五项 |
| **CI 中的契约测试** | **有**。`tests/cpp/test_acceleration_contract.cpp` 编译进 `core_unit_tests`（每次 push 都跑）；`test_realtime_face_pipeline.cpp` 断言 reason 透传（`build_disabled` 不被误归、`missing_model`、`runtime_init_failed`、`unsupported_input`）；`test_engine.cpp` 断言模型缺失时 `getRealtimeReason()=="missing_model"` 且**零轨迹**（即"绝不静默回退 LBPH"被测试锁住） |
| 机器可读产物 | **有三类**：运行期日志行 `ACCEL_SELF_CHECK ...`；`inference_bench_cli` 的 JSON + CSV（逐记录带 `backendRequested/Effective/Reason/Evidence`）；Android `getRuntimeStatus()` 的 JSON |
| `inference_bench_cli` 的覆盖 | **只有 `opencl`**；且自带一套手搓的 `backendRequested/Effective/Reason/Evidence`，与 `AccelContractStatus` 无类型级复用，`backendReason` 还硬编码字面量、不经 `normalizeReasonCode` 校验 |
| 文档自述 | `CLAUDE.md` / `AGENTS.md` / `DEVELOP.md` 均称"每个加速器在 `Engine::performAccelSelfCheck()` **或 `inference_bench_cli`** 中使用四元组"——**mpp / libyuv / ncnn / qualcomm 只存在于 Engine 侧**，CLI 侧不产出它们的契约行 |
| 缺失项 | 无 7 个固定码的**穷尽**往返测试；"禁静默回退"是**逐路径**断言而非全局属性断言；契约本身**无 JSON Schema**；`requested≠effective ⇒ 拒绝`只存在于文档约定，**无代码强制**（`inference_bench_cli` 记录四元组但从不因此判失败） |

另有两项在平台维度上恒为占位：`mpp` 在 Windows 恒输出 `unsupported_platform`；`qualcomm` 因项目内无 `deps/qualcomm_snpe/` 恒为 `missing_dependency`。

**结论**：这是本项目**证据最扎实的一条**——行为已被 CI 测试锁定；但它仍是"**观测层完整、规格层缺失**"，且三份指南对 CLI 覆盖范围的描述不实。要主张其价值，需补 Schema 与穷尽测试，并同步修正文档。

### 3.2 RK3288 CPU-only 约束下的完整工程组合 —— 成立，但性质要说清

证据链：

- `rknn_model_zoo` 的支持矩阵**确不含 RK3288**（RK3562/3566/3568/3576/3588/RV1126B/RV1109/RV1126/RK1808/RK3399PRO）
- 百度人脸离线 SDK Linux ARM 版**确经验证兼容 RK3288**，但那是 **Ubuntu 系统**、闭源、需商务授权——不是 Android
- `SharpAI/DeepCamera` 的 RK3288 支持**需要澄清形态**：其 `docs/RunOnRK3288.md` 显示这是**基于 Termux 的 Linux 用户态方案**——装 `SharpAI-LauncherTermux.apk` → `pkg install openssh` → `./setup_arm32.sh` → **Python 2 + TVM**，还需 `LD_PRELOAD=libatomic.so:libcutils.so` 这类变通，并另外下载 2018 年的开发镜像。**它不是原生 Android 应用，且该路径已多年未更新**，只能作架构参考

**准确表述**：未检索到可直接替代的成熟开源套件这一判断基本成立，但性质是——**没人这么做，而不是做不到**。稀缺的不是技术，而是"在 RK3288 这一档硬件上把整条链路跑通并工程化"这件事本身。

**但"工程纪律"的实际强度需按项区分**（本地核实，2026-09-16）：

| 约束 | 实际强度 |
|---|---|
| CPU-only 不静默回退 | **契约层真实强制**——`test_engine.cpp` / `test_realtime_face_pipeline.cpp` 断言模型缺失时显式 `missing_model` 且零轨迹，进 CI |
| APK 不内置模型 | **真实强制两条代码路径**（构建期删除 + APK 条目断言），但断言那条**未接 CI** |
| **512MB 内存预算** | **仅文档**。全仓 `tests/`、`scripts/`、`.github/` 中 **PSS 断言 = 0、内存预算断言 = 0**；`StatsRepository` 的 `MemSampler` 只采样供 UI 显示，不判定不断言。三处"512"命中里有一处（`jni/camera.cpp`）还是单平面拷贝的整数溢出上界，与内存预算无关 |

同理，**"四子系统"在部署拓扑上应表述为 3 个可交付物（APK / `win_local_service.exe` / webroot 静态资源）+ 1 个共享 C++ 源码层**——Web SPA 无独立部署形态，其产物由 Windows 服务托管。

**另有一项应记入"文档漂移"而非"实现缺口"**：`docs/designs/android-enrollment-design.md` 仍声称注册闭环因"逐帧特征源缺失"而不可用（列为 Blocker），但代码链实际已打通（`MainActivity.startNativeEnrollmentCapture` → `NativeFaceBridge` → `onEnrollmentFrame` → `feedEnrollmentSample`），`CHANGELOG.md` 亦有对应记录。

### 3.3 其余"部分成立"项

原报告自行降级为"部分成立"的四条（① 四子系统组合 ③ 八样本注册与模型导入 ⑤ 系统化评测 ⑥ 安全边界组合），核实后同意。补充一点：③ 与 ⑥ 的技术单项在开源里都有参照（`FaceAISDK` 有注册与质量门控，`ZepIris`/VisageAI 有质量与防伪检查），**独特性来自组合与约束，不来自单项**。

---

## 4. 哪些可以借鉴

### 4.1 原报告 8 条中需要修正的

- **原第 2 条（迁移 opencv-mobile）** —— 见 §1.3，改为"借配方不借包"，或先收敛适配器再评估
- **原第 3 条（本地服务降级为 REST 适配层）** —— 方向对；补具体参照：用 Raray Vision / IRIS 的端点设计对照 `docs/windows-web-spa/openapi.yaml`（见 §4.2 参照表）

其余 6 条（复用 ncnn-android 模板、标准媒体服务器、NVR 分层解耦、INT8 可重复实验、加密威胁模型先行、百度基线）成立。

### 4.2 本轮新增的三条

#### ⑨ 用公开的 RK3288 基准校准自己的数字

`nihui/ncnn-small-board`（作者即 ncnn 作者 nihui）收录了 **Tinker Board S R2.0 / RK3288CGW / A17 1.8GHz × 4** 的 benchncnn 成绩。这是**与目标 SoC 完全一致的公开可对照数据**，可直接用于判断本项目自测数字是否合理，以及"测量方法是否可信"。

#### ⑩ RK3288 基准必须在受控条件下测

ncnn issue [#3662](https://github.com/Tencent/ncnn/issues/3662) 记录：有人在**两块 RK3288** 上实测均达不到文档速度，最终发现需同时满足——`scaling_governor` 设为 `performance`（四核全设）+ **供电电压足够**（该案例为 POE 供电不足）。该 issue 还给出一个诊断信号：**"4 线程比 2 线程慢是不正常的"**。

**含义**：基准方法必须包含**供电与 governor 两项控制**，否则数字不可复现、不可比较。

#### ⑪ ncnn INT8 在 armv7 上有已知的正确性陷阱

本项目已做 INT8 量化，并设了"ArcFace 余弦 ≥ 0.90"的精度阈值。**但本地核实显示该阈值不在任何自动防线内**：

| 核查项 | 实测 |
|---|---|
| 阈值位置 | `tests/cpp/ncnn_precision_test.cpp` |
| 该目标是否进 CI | **否**。windows job 的 ctest 过滤器只跑 `-R "unit_tests|core_gtest_tests"` 与 `-R face_infer`，`ncnn_precision_test` 被排除 |
| 仓库自述 | `tests/cpp/test_arcface_contract.cpp` 逐字写明："INT8 vs FP32 余弦 ≥0.90 属本地模型精度验证，走 `ncnn_precision_test`，**CI 只跑结构/模拟**" |
| 量化步骤本身 | CI 中为 `continue-on-error: true` |

即：**阈值存在，但从不在 CI 中执行**。而 ncnn 历史 issue 显示 **armv7 + INT8 存在多起"结果错误 / 检测不到目标"**：

| Issue | 现象 | 根因 / 处置 |
|---|---|---|
| [#2952](https://github.com/Tencent/ncnn/issues/2952) | **RK3288 上 retinaface INT8 无任何人脸框输出**（FP32 正常 60ms，INT8 33ms 但空结果） | 未明确闭环 |
| [#1793](https://github.com/Tencent/ncnn/issues/1793) | ARM 上量化模型输出与 x86 差异巨大 | `opt.use_packing_layout = false` 解决 |
| [#2936](https://github.com/Tencent/ncnn/issues/2936) | armv7 INT8 与 x86 不一致，定位到 `split` 之后 | depthwise int8 arm 计算错误，已修（commit `f6c4952`） |
| [#3224](https://github.com/Tencent/ncnn/issues/3224) | x86 正确、arm 不正确 | x86 AVX2 生成 FMA 导致舍入差异，整数化后放大 |
| [#547](https://github.com/Tencent/ncnn/issues/547) | arm 开 int8 检测不到目标 | 已修（commit `ef36d79`） |

**行动含义**：INT8 验收不能只看"cosine ≥ 0.90"这一个标量，且该标量当前**根本不在 CI 中运行**。必须**逐层对比 armv7 与 x86 的输出**，并显式验证 `use_packing_layout` 在两端的差异。**这是本项目当前真实存在的风险敞口，而非假设风险。**

### 4.3 本仓库内已实现但未接入 CI 的检查（零开发成本）

调研中发现的**性价比最高的一类改进**：以下检查**代码已写好、逻辑已验证**，只是没接进流水线。前三条是纯接线工作。

| 检查 | 位置 | 现状 |
|---|---|---|
| APK 模型断言 `checkApkFaceModels` | `app/build.gradle` | 打开已构建 APK，发现任何 `assets/face_models/` 条目即抛 `GradleException`——**是真断言**，但 `ci.yml` 的 android job 从不调用它 |
| 安全基线门禁 `security-policy-audit.js` | `scripts/` | 未接 CI；CI 只跑 `tests/scripts/*.test.js`（它自己的自测），**不审仓库基线** |
| Web 首屏预算 `bundle:check` | `web/package.json` | 700KB 首屏预算，**真断言**，未接 CI |
| `win_face_database_perf` | CMake target | `ci.yml` 注释显式排除（"约 80s 且环境敏感，不参与 PR 门禁"） |
| `ncnn_precision_test` | CMake target | 仅在 `models/` 变更时跑，且 `continue-on-error: true` |

**另需注意**：`ci.yml` 的 windows job **只缓存 OpenCV 源码、不缓存编译产物**，因此每次 CI 都从零编译 15 个模块；且 `BUILD_LIST` 在 `ci.yml` 与 `cmake/opencv.cmake` 中**两处独立硬编码**，注释自述"必须一致"但**无脚本断言**。

---

## 5. 横向对照：活体检测（缺口已核实）

`FaceAISDK`（动作活体 + 静默活体 + UVC 双目 IR）、`kby-ai`、`Faceplugin`（iBeta level 2）、`VisageAI`、`ZepIris` **全都带活体 / 防伪**，而本项目**没有**。

**核实结论（2026-09-16，代码级确认）**：缺口**真实存在**，不是"实现存在而未文档化"。

| 核查项 | 结果 |
|---|---|
| 源码中的活体/防伪标识（`src/cpp`、`src/java`、`src/win`、`app/src`、`web/src`） | **零命中** |
| `models/` 下的模型 | 仅检测（`scrfd_face_detect_ncnn`、`yolo_face_int8_ncnn`）与识别（`arcface_*`、`mobilefacenet_*`），**无活体模型** |
| `docs/capability-matrix.md` 的能力条目 | 相机预览、检测/识别、八样本注册、加速能力文本、RTMP——**无活体** |
| `--fake-detect` / `--fake-embedding` 参数 | **不是防伪**：`FaceInferStages.cpp` 中它们分别合成居中检测框与假特征，是**测试注入开关** |

**但项目已经调研过它**：`docs/research/appendix-d.md` §6.4 有完整的「活体检测路线对比（工程可交付）」与表 6-3，含 PAD 指标口径（APCER/BPCER）与 **ISO 30107-3** 对齐，并规定了 `LIVE/SPOOF + 置信度` 的输出契约。

即：**设计已调研并规格化，实现从未落地**。这是"已知缺口"而非"疏漏"。

**影响**：在真实门禁场景下，缺少活体的直接后果是**照片即可通过**——这会削弱本项目「安全边界」的价值主张（该主张本就只是"部分成立"）。是否补齐需按目标场景判断，成本估计尚未做。


---

## 6. 引用来源

**一手核实（GitHub API，2026-09-16）**：本文所有星标 / 许可证 / 最后推送数据。

**项目页面**：

- opencv-mobile — <https://github.com/nihui/opencv-mobile>（模块取舍列表、构建方式）
- FaceAISDK — <https://github.com/FaceAISDK/FaceAISDK_Android>
- SeetaFace2 — <https://github.com/seetafaceengine/SeetaFace2>
- InspireFace — <https://github.com/HyperInspire/InspireFace>
- DeepCamera RK3288 — <https://github.com/SharpAI/DeepCamera/blob/master/docs/RunOnRK3288.md>
- CompreFace ARM 议题 — <https://github.com/exadel-inc/CompreFace/issues/610>、<https://github.com/exadel-inc/CompreFace/pull/1018>
- `ncnn-small-board` RK3288 成绩 — <https://github.com/nihui/ncnn-small-board>
- 百度人脸离线 SDK Linux ARM — <https://cloud.baidu.com/product/face/offline-sdk/linuxarm>

**技术依据**：

- `objdetect` 依赖 `dnn` — <https://github.com/opencv/opencv/issues/22544>
- ncnn 基准受供电与 governor 影响 — <https://github.com/Tencent/ncnn/issues/3662>
- ncnn armv7 INT8 正确性问题 — issues #547 / #1793 / #2936 / #2952 / #3224
