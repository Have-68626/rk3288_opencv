<!-- DOCSYNC_DOC
type=reference
status=living
as_of=2026-10-18
-->

# 致谢与许可证（CREDITS）


本文件用于记录本项目使用的第三方依赖、来源与许可证信息，便于审计与合规。

SCRFD 代码许可核验（2026-10-15）：[InsightFace 的 SCRFD 子目录 LICENSE](https://github.com/deepinsight/insightface/blob/master/detection/scrfd/LICENSE) 明确列出 Open-MMLab 版权与 Apache-2.0；下方代码许可沿用该子目录声明。模型权重许可另按模型台账记录。

## 📑 快速索引

| 平台 | 章节 |
|:----|:-----|
| 跨平台 | [📦 跨平台依赖](#跨平台依赖) |
| Android | [🤖 Android 依赖](#android-依赖) |
| Windows | [🖥️ Windows 依赖](#windows-依赖) |
| 模型 | [📊 模型台账与依赖](#模型台账与依赖) |
| 检查 | [📋 依赖状态检查](#依赖状态检查) |
| 模板 | [📝 外部文档模板](#外部文档模板) |

---

## 📦 跨平台依赖

### 构建工具链

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| CMake | 3.22.1+ | 跨平台构建配置 | BSD 3-Clause |
| Gradle | 9.0（内置 gradlew） | Android 构建系统 | Apache 2.0 |
| Android NDK | 27.0 | Native 编译 | Android SDK License |
| Node.js + pnpm | 22.17.1+ | Web 前端构建 | MIT |

#### Gradle Wrapper 完整性锁定（S-SEC-B10-004 / SEC-13）

| 项 | 内容 |
|:---|:-----|
| `gradle-wrapper.jar` | 已替换为官方 gradle-9.0.0 生成的 wrapper jar（SHA-256 `76805e32c009c0cf0dd5d206bddc9fb22ea42e84db904b764f3047de095493f3`，见 `gradle/wrapper/gradle-wrapper.jar.sha256`） |
| `distributionSha256Sum` | `gradle/wrapper/gradle-wrapper.properties` 锁定 `gradle-9.0.0-bin.zip` 官方校验和 `8fad3d78296ca518113f3d29016617c7f9367dc005f932bd9d93bf45ba46072b` |
| 校验和来源 | `services.gradle.org/versions/all` 与 `gradle.org/release-checksums/`（两处官方来源一致）；wrapper 校验不匹配时构建直接失败（RED→GREEN 已功能验证） |
| 校验方式 | `cd gradle/wrapper && sha256sum -c gradle-wrapper.jar.sha256`；`distributionSha256Sum` 由 wrapper 下载发行版时自动校验 |

### 跨平台库

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| **OpenCV** | 4.10.0 | 图像采集、处理、特征提取 | Apache 2.0 |
| **libyuv** | — | YUV/RGB 格式转换与缩放 | BSD 3-Clause |
| **ncnn** | 1.0.20260113 | 端侧推理后端 | BSD 3-Clause | 详见 [ncnn 依赖账本与安全](#ncnn-依赖账本与安全) |

---

## 🤖 Android 依赖

### 图像处理与推理

| 依赖 | 用途 | 许可证 | 状态 |
|:-----|:-----|:-------|:-----|
| OpenCV 4.10.0 | 图像处理核心 | Apache 2.0 | ✅ 已集成 |
| ncnn | 端侧推理加速 | BSD 3-Clause | ✅ 已下载（1.0.20260113，见 [ncnn 依赖账本与安全](#ncnn-依赖账本与安全)） |
| Rockchip MPP | 硬件解码加速 | Apache 2.0 | ⚠️ 可选（自动回退） |
| Qualcomm SDK | 推理加速 | — | ⚠️ 可选（未检测到） |
| FFmpegKit | RTMP / RTMPS 推流 | 本地 x264 构建为 GPL 3.0 或更新版本 | 本地已集成 6.0.LTS 自编译 AAR；API21+、ARMv7 需 NEON、ARM64；指定消费AAR已有[双机局域网功能记录](docs/audits/2026-10-17-rk3288-runtime-feedback.md)；完整识别/性能/长稳未验收，后续重建候选不继承该结果；构建与来源见[记录](docs/plans/2026-10-16-ffmpeg-kit-build.md) |

### UI 组件

| 依赖 | 版本 | 用途 | 许可证 |
|:-----|:-----|:-----|:-------|
| AndroidX AppCompat | 兼容性支持 | Apache 2.0 | ✅ |
| Material Components | Material 风格 UI | Apache 2.0 | ✅ |
| CameraX | 相机采集 | Apache 2.0 | ✅ 1.3.4 |
| AndroidX ConstraintLayout | 布局系统 | Apache 2.0 | ✅ |

### 传递依赖说明

CameraX 的传递依赖（已解析）：
- `androidx.concurrent:concurrent-futures:1.1.0` → Apache 2.0
- `com.google.guava:listenablefuture:1.0` → Apache 2.0
- `androidx.lifecycle:lifecycle-*:2.6.1` → Apache 2.0

---

<a id="ncnn-依赖账本与安全"></a>

## 🔒 ncnn 依赖账本与安全（S-SEC-B10-001）

本小节记录 ncnn 依赖的版本/来源/许可证/摘要与已知安全公告处置状态，作为审计与合规依据。

### 版本与来源

| 项 | 值 |
|:---|:---|
| 版本 | **1.0.20260113**（`NCNN_VERSION_STRING`，证据：安装目录 `arm64-v8a/lib/pkgconfig/ncnn.pc` → `Version: 1.0.20260113`；源码根 `CMakeLists.txt` `set(NCNN_VERSION_PATCH 20260113)`） |
| 发布 | 官方 [Tencent/ncnn](https://github.com/Tencent/ncnn/releases/tag/20260113) GitHub Release，tag `20260113`，commit **`e956fbf`**，发布于 2026-01-13 |
| 许可证 | **BSD 3-Clause**（根 `LICENSE.txt`）；附带子组件：glslang BSD-3、pybind11 BSD-3、neon/sse/avx_mathfun zlib |
| 引入方式 | `app/build.gradle:70-79`：优先 `NCNN_DIR` 环境变量，后备硬编码本地路径 `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan`；存在 `armeabi-v7a/`+`arm64-v8a/` ABI 目录才传 `-DRK_ENABLE_NCNN=ON` 与 `NCNN_DIR_ARMv7/ARM64`。Windows 侧使用 `ncnn-20260113-windows-vs2022` 预编译库 |
| 完整性 | 本地资产与官方 Release 资产**大小逐字节一致**（android-vulkan 27086981B、shared 19294845B、full-source 22263010B） |

### 可复现摘要（SHA-256，本地 `D:/ProgramData/NCNN/` 下文件）

| 资产 | SHA-256 |
|:---|:---|
| `ncnn-20260113-android-vulkan.zip` | `41944630a7b305b190e7caea4b405bc6d103c6383dc4988e9e85616058e3cccd` |
| `ncnn-20260113-android-vulkan-shared.zip` | `b9ec10f8ad5952a5b808abeb60b2580736ea3b937817749df5e66118d67c6848` |
| `ncnn-20260113-full-source.zip` | `53696039ee8ba5c8db6446bdf12a576b8d7f7b0c33bb6749f94688bddf5a3d5c` |

复算方式：`certutil -hashfile <zip> SHA256`（或 `sha256sum <zip>`）；升级/替换 ncnn 后必须同步更新本表。

### 已知安全公告与处置

| 公告 | 严重度 | 影响 | 本仓库状态 |
|:---|:---|:---|:---|
| **CVE-2026-50144**（GHSA-jxmc-3mv6-7pwr，2026-05-28 发布） | high（CVSS 7.1） | `ParamDict::load_param` 仅校验 `id >= NCNN_MAX_PARAM_COUNT(=32)` 上限，负 id（`[-23299,-1]`）绕过数组 key 归一化后直接作 `params[32]` 下标 → 可控偏移越界堆写（CWE-787）；.param 恶意文件在 `Net::load_param()` 加载时触发。公告同时另列数组长度 `create(len)` huge-alloc 弱点 | **受影响**：20260113（commit e956fbf）早于修复；**最新发布 20260526（commit e54f7b1）即公告确认的脆弱提交，尚无含修复的官方发布** |
| GHSA-c44p-qr97-jccv | — | 上游 `pull_request_target` CI 工作流 RCE 风险 | 上游 CI 配置问题，不涉及本仓库使用面，仅记录 |

> **CVE-2026-50144 在线细节待人工复核**：本环境 WebFetch 受限，未能访问
> GHSA-jxmc-3mv6-7pwr 在线公告进行比对。上表该行的公告细节（严重度 / CVSS / 弱点条目 /
> 受影响版本）为治理任务既有登记，**未经在线复核**；人工复核（GitHub Advisories 检索
> `GHSA-jxmc-3mv6-7pwr`）确认后更新本表并移除本标注。不编造在线细节。

**处置（受控回补）**：因无法升级到含修复的官方 ncnn，本仓库在模型加载路径新增前置校验 `src/cpp/include/NcnnParamGuard.h`，镜像上游修复语义（归一化后 `id < 0 || id >= 32` 拒绝）+ 数组长度上限封堵公告另列弱点，已接入全部 4 个 `load_param` 调用点（YoloFaceDetector / ArcFaceEmbedder / MobileFaceNetAdapter / inference_bench_cli），配 8 个恶意 param 负面测试（`tests/cpp/test_ncnn_param_guard.cpp`，含 CVE 官方 PoC `-1=305419896`），随 `core_unit_tests` 在 CI（`RK_SKIP_OPENCV=ON`）下构建运行。

**升级/回补监控**：monitor 上游 ncnn 新发布（含 `src/paramdict.cpp` 两处 `id < 0` 修复）。出现后：替换本地 `NCNN_DIR` 指向新预编译库 → 同步本表版本/commit/SHA-256 → 运行 `core_unit_tests` 确认负面测试仍全过、真实模型无误拒。替换前可移除本仓库前置守卫或保留（无害的双保险）。

**可测面说明**：Android 无模拟器、本机无 host ncnn 运行时，负面测试为**纯文本前置校验**（不依赖 ncnn/OpenCV）；真机运行时验证需 RK3288 设备（模型加载时恶意 .param 应在 `load_param` 之前被拒绝）。

---

<a id="windows-依赖"></a>

## 🖥️ Windows 依赖

### Native 层

| 依赖 | 用途 | 许可证 |
|:-----|:-----|:-------|
| OpenCV 4.10.0 | 图像处理核心 | Apache 2.0 |
| ncnn | 推理加速 | BSD 3-Clause |
| Microsoft Windows SDK | 摄像头采集（Media Foundation）、Win32 UI | Microsoft License |

### HTTP 服务

| 依赖 | 版本 | 用途 | 许可证 | 位置 |
|:-----|:-----|:-----|:-------|:-----|
| [CivetWeb](https://github.com/civetweb/civetweb) | 1.16 | 未编入目标的第三方源码与许可证归档快照 | MIT | `src/win/third_party/civetweb/` |
| └─ MD5 实现 | — | L. Peter Deutsch 实现 | zlib | `src/win/third_party/civetweb/src/md5.inl` |

### Web 前端

| 依赖 | 用途 | 许可证 |
|:-----|:-----|:-------|
| React / ReactDOM | UI 框架 | MIT |
| Ant Design | UI 组件库 | MIT |
| React Router | 路由管理 | MIT |
| Vite | 构建工具 | MIT |
| Cypress | E2E 测试 | MIT |

---

<a id="模型台账与依赖"></a>

## 📊 模型台账与依赖

本项目使用或支持以下机器视觉模型。对于需要在部署时下载的模型，请确保计算其 SHA-256 哈希值与下表一致，以确保安全性与精度。

> **⚠️ 商用限制结论（必读）**：当前**主路径**依赖的检测/识别模型——SCRFD（`det_10g`）与 ArcFace（`w600k_r50`）——其权重**源自 `deepinsight/insightface`**。上游 `model_zoo/README.md` 顶部原文：*"ALL models are available for non-commercial research purposes only."*，且主 README 澄清**手动下载与经其 Python 库自动下载受同一政策约束**；维护者在 [issue #2486](https://github.com/deepinsight/insightface/issues/2486) 明确 buffalo_* 系列不可用于生产。**因此这条主路径上的模型及其 INT8 量化产物均不可商用。** MobileFaceNet 新候选已固定为同上游的 `w600k_mbf.onnx`（buffalo_s/sc，见脚注 ¹），同样仅限非商业研究；已用本地工具重建并逐字节匹配现存 ncnn 参数与权重，见下方转换证据。
>
> 仓库中 **`yunet`（MIT）与 `sface`（Apache-2.0）** 是**可商用替代**——本轮**仅登记备查，不改模型选型**。
>
> 本结论与 `models/android/arm64_accuracy/LICENSE.txt`、`models/android/rk3288_light/LICENSE.txt`（其 `licenseId` 为 `insightface-models-noncommercial-research`）一致；此前中央台账缺少该维度，本轮补齐。
>
> 来源：<https://github.com/deepinsight/insightface/blob/master/model_zoo/README.md>、<https://github.com/deepinsight/insightface/blob/master/README.md>、<https://github.com/deepinsight/insightface/issues/2486>。

| 模型名称 | 用途 | 格式 | 部署路径 | 来源 | SHA-256 | 许可证 | 商用限制 |
|:---------|:-----|:-----|:---------|:-----|:--------|:-------|:---------|
| **LBP Frontal Face** | 人脸检测（级联，`cascade_lbp`） | XML | `app/src/main/assets/lbpcascade_frontalface.xml` | [OpenCV Data](https://github.com/opencv/opencv/tree/master/data/lbpcascades)（随 OpenCV 4.10.0 分发） | `529f217132809f287aaed5cd35dc00d9bc9b2afebe46dd1fe90ecb67f1daad0d` | 3-Clause BSD | ✅ 可商用 |
| **ResNet SSD Face** | 人脸检测（DNN，`dnn_ssd`） | PB | `storage/models/opencv_face_detector_uint8.pb` | [OpenCV 3rdparty](https://github.com/opencv/opencv_3rdparty/raw/dnn_samples_face_detector_20180205_fp16/res10_300x300_ssd_iter_140000_fp16.caffemodel)（res10_300x300_ssd） | ⚠️ 需在部署时验证 | Apache-2.0（沿用既有台账） | ✅ 可商用 |
| **ResNet SSD Config** | 网络结构定义 | PBTXT | `storage/models/opencv_face_detector.pbtxt` | [OpenCV Extra](https://raw.githubusercontent.com/opencv/opencv_extra/master/testdata/dnn/opencv_face_detector.pbtxt) | ⚠️ 需在部署时验证 | Apache-2.0 | ✅ 可商用 |
| **NCNN YOLO Face** | 端侧检测（`yolo_face` / `yolo_face_int8`） | BIN/PARAM | `models/yolo_face_int8_ncnn/`（INT8）；FP32 目录仓库内无 | 仓库标注「自训练/转换」，**无上游 URL 可取证** | ⚠️ 需本地生成 | 未能核实 | ⚠️ 未能核实 |
| **NCNN SCRFD** | 端侧检测（`scrfd_0.5gf`） | BIN/PARAM | `models/scrfd_face_detect_ncnn/`、`models/int8/` | [insightface SCRFD](https://github.com/deepinsight/insightface) `det_10g.onnx`（属 buffalo_l），经 PNNX 转换 | ⚠️ 需本地生成 | 代码 Apache-2.0（Open-MMLab）/ 模型权重：insightface 非商用 | ❌ 非商用（insightface 限制） |
| **ArcFace Embedder** | 人脸特征提取（`arcface` / `arcface_int8`） | ONNX/BIN | `models/arcface_ncnn/`、`models/arcface_int8_ncnn/` | [insightface](https://github.com/deepinsight/insightface) `w600k_r50.onnx`（属 buffalo_l） | ⚠️ 需本地生成 | 代码 MIT / 模型权重：insightface 非商用 | ❌ 非商用（insightface 限制） |
| **MobileFaceNet** | 轻量人脸特征提取（`mobilefacenet` / `mobilefacenet_int8`）；仅显式调用、非默认、无自动回落 | ONNX/BIN/PARAM | 旧件 `models/mobilefacenet_ncnn/`、`models/mobilefacenet_int8_ncnn/`；新候选仅本地缓存 | [InsightFace v0.7](https://github.com/deepinsight/insightface/releases/tag/v0.7) `w600k_mbf.onnx`（buffalo_s/sc） | ⚠️ FP16存储重建件与现存件逐字节一致；未验收真人精度 | 代码 MIT / 新候选权重：insightface 非商用 | ❌ 新候选及派生产物仅限非商业研究 |
| **YuNet** | 端侧检测（`yunet`，可商用替代） | ONNX | 外部模型（经配置 `modelPath` 加载，不随仓库分发） | [OpenCV Zoo `face_detection_yunet`](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | — | MIT（`Copyright (c) 2020 Shiqi Yu`） | ✅ 可商用 |
| **SFace** | 人脸特征提取（`sface`，128D，可商用替代） | ONNX | 外部模型（经配置 `modelPath` 加载，不随仓库分发） | [OpenCV Zoo `face_recognition_sface`](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface) | — | Apache-2.0（模型目录自带独立 LICENSE 全文） | ✅ 可商用 |
| **LBPH Face Recognizer** | 人脸识别（`lbph`） | 算法（无训练权重） | opencv_contrib `face` 模块（`OPENCV_CONTRIB_ROOT`） | [OpenCV contrib](https://github.com/opencv/opencv_contrib) | — | Apache-2.0 | ✅ 可商用 |

> **¹ MobileFaceNet（F32，2026-10-16）**：用户认可本地 `deps/insightface/w600k_mbf.onnx`，SHA256=`9cc6e4a75f0e2bf0b1aed94578f144d15175f357bdc05e815e5c4a02b319eb4f`；本地 buffalo_s/sc ZIP 同名成员均为此摘要。固定上游代码提交 `3e6486942a1be2da0e5b475fac375ea73264bd21` 的 [arcface_onnx.py](https://github.com/deepinsight/insightface/blob/3e6486942a1be2da0e5b475fac375ea73264bd21/python-package/insightface/model_zoo/arcface_onnx.py) 对该无 Sub/Mul 的父图使用 **RGB、(x−127.5)/127.5、112×112**，输出512维。生产适配器与校准预设已统一；代码 MIT 不改变[预训练权重仅限非商业研究](https://github.com/deepinsight/insightface/blob/3e6486942a1be2da0e5b475fac375ea73264bd21/python-package/README.md)的限制。本次未下载权重、未证明远端资产字节身份。
>
> 已离线重建两条链：PNNX `20260526-windows`、`optlevel=2`、分别 `fp16=0/1`，工具、命令与摘要见 [供应链账本 F32](docs/security/supply-chain-ledger.md#f32mobilefacenet-指定父链与迁移)。`fp16=1` 的 param/bin 均与现存生产件逐字节一致，建立了从指定父图到现存件的**可复现转换对应关系**；这不声称找回历史实际执行命令。`fp16` 在此控制权重存储格式，不代表模型经过 INT8 量化。未覆盖旧模型、未生成新生产 INT8 产物、未验收真人精度；两链候选仅留本地 `cache/audit-fixes-2026-10-13/f32-final/`。
>
> **迁移要求**：旧 mean=0 下生成的模板不能继续使用，须在新独立图库重登记。显式 MobileFaceNet 请求必须更新模型/预处理版本和新非空 `templateSpaceId`（可沿用已逐字节复现的现存权重），外部 `gallerySnapshot` 也必须绑定该新 ID；空 ID 会沿用旧目录，禁止用于本次迁移。不得自动搬库、删除旧库或复用旧身份；profile 默认实时装配仍是 ArcFace，不因本修复切换到 MobileFaceNet。

> **² 已订正（2026-09-30）：`ModelRegistry` 对 `yunet` / `sface` 的描述曾与事实不符**。原备注 `yunet` 写"OpenCV 内置，无需额外模型依赖"、`sface` 写"OpenCV contrib face 模块内置"，二者**已改为**与事实一致的表述——OpenCV 提供的是 **API**（`cv::FaceDetectorYN` / `cv::FaceRecognizerSF`），权重需**外部 ONNX**（来自 OpenCV Zoo），经配置 `modelPath` 加载，仓内不随附。事实依据：`src/cpp/src/adapters/YuNetAdapter.cpp` 与 `src/cpp/src/adapters/SFaceAdapter.cpp` **均要求传入 `modelPath`**（外部 ONNX，来自 OpenCV Zoo），**并非 OpenCV 内置**。上表已按实际行为登记其部署路径为"外部模型（经配置 `modelPath` 加载，不随仓库分发）"。

> **提示**：启动时程序将自动进行自检，并会在日志中打印所有加载模型的 SHA-256。请确保与上述台账保持一致。

> **⚠️ 重要**：DNN 模型文件由于体积较大，默认不包含在代码库中。Windows 环境首次部署时，需将 `.pb` 与 `.pbtxt` 文件下载并放置于 `storage/models/` 目录下（或通过 Web UI 的
`/api/v1/settings` 接口修改 `dnn.modelPath` 配置）。

---

## 📋 依赖状态检查

### ❌ 阻断性问题（需手动补充）

| 缺口 | 影响范围 | 参考位置 |
|:-----|:---------|:---------|
| **DNN 模型文件缺失** | Windows 人脸检测（DNN 后端） | 见上方"模型台账"，下载后放置于 `storage/models/` |
| **Windows CMake HDF5 冲突** | Windows 原生构建（可选） | 方案 A：重建 x64 生成器；方案 B：`BUILD_opencv_hdf=OFF` |

### ⚠️ 可选组件（缺失时自动回退）

| 依赖 | 用途 | 回退方案 |
|:-----|:-----|:---------|
| Qualcomm SDK | 推理加速 | 自动使用 CPU 推理 |
| RK MPP 头文件 | 硬件解码 | 自动使用 CPU 解码 |
| FFmpegKit AAR | RTMP 推流 | 禁用推流功能（不影响核心识别） |

---

## 📝 外部文档模板

### Linux Kernel 配置模板（RK3288 BSP）

| 文件 | 用途 | 来源 | 许可证 |
|:-----|:-----|:-----|:-------|
| `docs/bsp/defconfig/rk3288_defconfig` | 内核配置快照 | Linux Kernel 4.4.143 导出 | GPL-2.0 |
| `docs/bsp/kernel-config/kernel.config` | 内核配置参考 | 设备运行态快照 | GPL-2.0 |

> **说明**：以上文件仅用于合规审计与基准对齐参考，不参与实际编译。

---

## 备注

- 如新增/升级/替换依赖，请同时更新本文件，并在 `DEVELOP.md` 中记录变更路径与影响面。
- 如需精确复核 Android 传递依赖，请运行：`.\gradlew.bat :app:dependencyInsight --configuration debugRuntimeClasspath --dependency <artifact>`
