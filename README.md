# RK3288 机器视觉引擎 (AI Engine)

**更新日期**: 2026-05-18

![Platform](https://img.shields.io/badge/Platform-RK3288%20%7C%20ARMv7%20%7C%20x86_64-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-green)
![OpenCV](https://img.shields.io/badge/OpenCV-4.10.0-orange)

## 📖 项目简介与运行模式

本项目是一个跨平台机器视觉应用，支持在资源受限设备上进行稳定的视频监控与边缘生物识别，包含以下部分：
*   **Android App (Gradle)**: 位于 `app/`，提供带 UI 的 Android 监控界面与业务。
*   **通用 C++ 核心与工具 (CMake/CLI)**: 位于 `src/cpp/`，实现跨平台视觉算法与无头 CLI。
*   **Windows 摄像头人脸识别系统**: 位于 `src/win/`，提供本地服务、设备打开与识别链路，以及静态资源的 HTTP 分发服务。
*   **Web SPA 源码 (Vite/React)**: 位于 `web/`，为 Windows 本地系统提供基于浏览器的前端页面。

## 📂 项目结构

> 📋 **详细目录结构与模块说明请参阅 [DEVELOP.md](DEVELOP.md)。**

主要目录：
- `app/` — Android 应用（UI + 业务）
- `src/cpp/` — C++ 核心算法与工具
- `src/win/` — Windows 本地服务
- `web/` — Web SPA 前端
- `docs/` — 开发设计文档与运维手册

## 🛠️ 依赖列表与状态检查

> 📋 **完整的依赖状态检查清单（包含已满足、缺失、可选）请参阅 [CREDITS.md](CREDITS.md)。**

关键依赖：
- **必需**：OpenCV 4.10.0、Android NDK 27.0、CMake 3.22.1、Gradle 9.0、Node.js 22.x
- **可选**：RK MPP（硬件加速）、Qualcomm SDK、DNN 模型文件
- **详见**：[CREDITS.md](CREDITS.md) 中的模型台账与依赖清单

## 🚀 快速开始

### 构建命令与测试命令 (Windows CMake)
本仓库使用 CMake，请使用 Visual Studio 生成器进行构建（需预配置 OpenCV）：
```powershell
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="...\opencv"
cmake --build build_win --config Release --target win_unit_tests
cmake --build build_win --config Release --target win_face_eval_cli
cmake --build build_win --config Release --target win_face_bench_cli
cmake --build build_win --config Release --target win_local_service
ctest --test-dir build_win -C Release
```

### 构建命令与测试命令 (Android Gradle)
通过 Gradle Wrapper 编译与测试：
```powershell
.\gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

### 构建命令与测试命令 (Web Vite)
Web 前端通过 `pnpm` 构建。执行构建后，产物会被输出，并放入 `src/win/app/webroot`：
```powershell
pnpm -C web install
pnpm -C web lint
pnpm -C web build
```

## ⚙️ 配置与接口入口

### Windows 核心配置 (JSON)
- **事实来源 (Source of Truth)**：`%APPDATA%\rk_wcfr\config.json`
- **修改方式**：推荐通过 Web UI (`http://127.0.0.1:<port>`) 操作，或调用 `PUT /api/v1/settings` 接口。
- **兼容性迁移配置**：`config/windows_camera_face_recognition.ini`（仅在 JSON 缺失时用于初次生成，后续修改无效）。
- 本地 HTTP 默认监听仅 `127.0.0.1` 保障安全，实现位于 `src/win/src/HttpFacesServer.cpp`。

### Windows Web SPA 配置说明
- 配置文档位于：`docs/windows-web-spa/config.md`
- JSON Schema 位于：`docs/windows-web-spa/config.schema.json`

### 模型台账与下载
本项目依赖的 DNN 模型等大文件默认不入库。关于所需模型的下载地址、存放路径与许可证说明，请查阅 [CREDITS.md](CREDITS.md) 中的**模型台账**部分。

## 📊 验证与报告输出路径

基准测试和验证报告生成后均存储于被版本控制忽略的专属目录：
- 基准与性能报告目录：`tests/metrics/`
- CI 报告与验证日志目录：`tests/reports/`

可以使用以下命令审计文档同步状态：
```powershell
node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit
```

## 🔧 脚本工具

> 位于 `scripts/` 目录，提供构建、测试、验证等自动化工具。

| 脚本 | 用途 | 使用说明 |
| :--- | :--- | :--- |
| `clean-repo-junk.js` | 清理仓库垃圾文件 | `node scripts/clean-repo-junk.js scan --ci` |
| `docs-sync-audit.js` | 审计文档同步状态 | `node scripts/docs-sync-audit.js --out-dir tests/reports` |
| `verify_opencv_host.bat` | 验证 OpenCV 主机环境 | 设置 `OPENCV_ROOT` 后执行 |
| `verify_faces_test_set01.bat` | 人脸检测参数验证 | 依赖 `tests/test_set01/` 测试集 |
| `run-web-e2e.ps1` | Web SPA 端到端测试 | Cypress E2E + 覆盖率报告 |
| `bench_camera_adb.ps1` / `.sh` | Android 摄像头基准测试 | 通过 ADB 连接设备测试 |

## 待办列表 (Todo List)

> ⬜ 表示待办（按优先级排序）

### 1. ⬜ **[P0] 核心稳定性治理 (Stability & Bug Fixes)**
- **现状核对**：
  - [Android] 前后台切换后预览画面高概率卡死（热重启无效）。
  - [Mock] 加载文件模式存在异常闪退风险。代码审查与业界 Mock 策略对照（参见 `ErrorLog/cleanup/mock研究.txt`）发现现有实现虽覆盖三阶段但各有缺口：
    - ① **调用前预检**（Calling）：已实现 `wantMock` 状态机与文件选择器（[MainActivity.java](src/java/com/example/rk3288_opencv/MainActivity.java) `handleMockFileSelection`）→ 缺少文件格式/完整性快速校验，损坏文件与超大文件需等到加载阶段才被发现。
    - ② **加载时防护**（Loading）：已实现分块读取 1MB 块 + 50MB 大小上限 + OOM catch（[VideoManager.cpp](src/cpp/src/VideoManager.cpp) `open(filePath)` 静态图片加载）→ 缺少总体加载超时机制，大文件在网络流场景下可能阻塞主线程。
    - ③ **解析后验证**（Parsing）：已实现 `imdecode` 异常捕获 + `bad_alloc` 保护 + 视频文件回退（[VideoManager.cpp](src/cpp/src/VideoManager.cpp) `captureLoop` 中 isMockMode 分支）→ 缺少首帧格式/分辨率/帧率预检，损坏视频文件在 `cap.read()` 时抛出异常。
    - ④ **测试覆盖**：MonitoringCoordinatorTest 覆盖了部分状态机逻辑（`tests/unit/java/.../MonitoringCoordinatorTest.java`），但缺少面向损坏文件的 fixture 测试。
  - [Engine] `handleAbnormalEvent` 在两个监控会话中频繁触发约 55 次，形成"处理慢→触发异常→CPU 消耗→处理更慢"的恶性循环。参见 [证据日志分析](docs/analysis/evidence_20170115_analysis.md)。
- **目标**：重构渲染 Surface 生命周期管理，实现"感知式"重建；按三阶段补齐 Mock 文件加载防护；调查 `handleAbnormalEvent` 触发条件，区分真正的异常与可忽略警告。
- **验收**：前后台切换 50 次无黑屏；Mock 加载损坏/超规格文件在调用阶段即快速拒绝而非加载后崩溃；`handleAbnormalEvent` 触发频率下降 90% 以上。

### 2. ⬜ **[P0] 链路加速方案落地 (Acceleration)**
- **现状核对**：
  - [CPU-only] 日志分析确认 **所有硬件加速路径均不可用**：Qualcomm SDK 回退（RK_HAVE_QUALCOMM=0）、MPP 硬件解码回退（RK_HAVE_MPP=0）、OpenCL 不支持。Yolo/ArcFace 推理、帧预处理完全由 CPU 承担，这是性能瓶颈的根本原因。参见 [诊断报告](docs/analysis/evidence_20170115_analysis.md)。
  - [延迟尖峰] CPU-only 下帧分析最大延迟达 37.6ms，25fps（40ms 间隔）余量仅 2.4ms，极易丢帧。
  - [加速全景调查] 端到端管线各阶段 CPU 占比：YOLO 检测推理 ~40%（10-20ms）、ArcFace 特征提取 ~30%（8-15ms）、帧预处理 ~10%（2-5ms）、Mock 视频解码 ~15%（5-15ms）、人脸对齐 ~3%（1-2ms）、1:N 搜索 ~1%（<1ms）。参见 [加速方案评估报告](docs/acceleration_study.md)。
- **加速机会矩阵**：

  | 优先级 | 加速项 | 当前耗时 | 加速手段 | 预期收益 | 实现难度 | 项目已有支持 |
  |:------:|:-------|:--------:|:---------|:--------:|:--------:|:-----------|
  | **P0** | 推理调度优化 | 10-20ms | 调整 detect stride 帧跳过策略 | 推理负载降 50% | 低 | ✅ InferenceThrottle 框架已有 |
  | **P0** | **YOLO ncnn 推理** | 10-20ms | ncnn (NEON) 替代 OpenCV DNN | 延迟降 30-50% | 中 | ✅ 工厂方法 `CreateNcnnYoloFaceDetector()` 已有 |
  | **P0** | **ArcFace ncnn 推理** | 8-15ms | ncnn (NEON) 替代 OpenCV DNN | 延迟降 30-50% | 中 | ✅ `BackendType::Ncnn` 枚举与 `NcnnState` 已有 |
  | P1 | Mock 视频 MPP 硬解码 | 5-15ms | MPP 硬件解码（`MppDecoder` 已完成） | CPU 占用降至 ~0% | 中 | ✅ `MppDecoder` 类已创建并集成 |
  | P1 | 帧预处理 libyuv | 2-5ms | libyuv NEON 加速 resize/颜色转换 | 降 2-4ms | 低 | ✅ `RK_ENABLE_LIBYUV=ON` |
  | P1 | 模型 INT8 量化 | 10-20ms | ncnn INT8 量化模型推理 | 加速 2-3x | 高 | ❌ 需量化工具链 |
  | P2 | 1:N 搜索 NEON 加速 | <1ms | ARM NEON vmlaq_f32 点积 | 加速 5-10x | 低 | ✅ `FaceSearchLinearIndex` 已增加 NEON/SIMD 路径，x86 自动回退 |

- **目标**：按优先级推进加速项落地；启用 MPP 硬件解码；补齐加速开关的 `requested`/`effective`/`evidence` 证据输出；在 RK3288 真机完成实测填表并定稿加速策略。
- **验收**：日志中明确记录"为何启用/为何回退加速"；启用 ncnn 后 YOLO+ArcFace 推理 P95 延迟降低 30% 以上；MPP 启用后帧分析 P95 延迟降低 50% 以上；证据日志分析结论闭环。

### 3. ⬜ **[P0] AI 模型与管线管控 (Model & AI Pipeline)**
- **现状核对**：模型台账已建立但在代码中为静态描述；缺乏运行时模型查询接口；检测参数（minSize 等）未持久化。
- **目标**：实现运行时模型查询 API (`/api/v1/models`)；支持检测参数的 JSON 持久化；补齐 YOLO 检测后端（ncnn）的热切换与版本校验能力。
- **验收**：通过 Web UI 实时查看当前加载模型的 hash 与后端状态；参数修改后重启服务不丢失。

### 4. ⬜ **[P0] 自动化测试与 CI 加固 (CI/CD)**
- **现状核对**：当前 CI 缺少 Android 构建/单测、Web 前端测试及文档同步审计自动化。
- **目标**：扩展 [ci.yml](.github/workflows/ci.yml)，集成 Android job、Web job 及 `node scripts/docs-sync-audit.js` 强制发布门禁；统一归档测试报告与关键产物。
- **验收**：PR 打开后自动触发全平台闭环验证，失败时精准定位问题点；CI 命中缓存后显著加速。

### 5. ⬜ **[P1] 人员注册与权限系统 (Personnel & Enrollment)**
- **现状核对**：仅支持单样本均值注册；缺少质量检查、人员属性（姓名/工号）、权限校验及安全的导入导出。
- **目标**：建立人员实体模型（personId + profile + 状态机）；增加注册前质量门槛（亮度/角度/清晰度）；支持加密的批量导入导出（DPAPI/Keystore）。
- **验收**：UI 支持完整的增删改查与权限配置；导入导出文件防篡改且兼容版本演进。

### 6. ⬜ **[P1] 工程规范与治理 (Engineering Excellence)**
- **现状核对**：存在遗留废弃接口；开发流程文档化程度尚需补齐；`initEngine()` 中存在 Engine 初始化双重调用，浪费约 20ms 启动时间。
- **目标**：执行废弃代码"零容忍"清理；补齐基础框架、核心算法与开发流程的研究结论；消除 Engine 初始化冗余调用。
- **验收**：代码库无未引用的旧版接口；开发者可根据文档一键复现全链路开发/调试环境；Engine 初始化日志中仅出现一次 `initialize` 调用。

### 7. ⬜ **[P2] 多模型集成与切换策略 (Model Diversity)**
- **现状核对**：当前项目使用 YOLO Face（检测）+ ArcFace（特征提取），OpenCV DNN/ncnn 双后端。CREDITS.md 中登记了 5 个模型的台账，但缺少替代模型集成和运行时切换能力。
- **模型对比参考**：

  | 管线 | 用途 | 当前模型 | 可替代模型 | 替代优势 | 切换代价 |
  |:----|:----|:--------|:----------|:--------|:--------|
  | Android/CLI | 检测 | YOLO Face (~7M) | **SCRFD-0.5GF** | 精度更高(WiderFace 96.1%)，参数量更低 | 需重新适配 ncnn 接口 |
  | Android/CLI | 检测 | YOLO Face (~7M) | **YuNet** (OpenCV ~0.3M) | 极轻量，OpenCV 内置 | 需用 `FaceDetectorYN` API |
  | Android/CLI | 检测 | YOLO Face (~7M) | **RetinaFace** (InsightFace 500MF) | 精度最高，生态完善 | 需新增 ONNX→ncnn 转换 |
  | Android/CLI | 识别 | ArcFace 512D (FP32) | **MobileFaceNet** 128D | 推理快 2-3 倍，适合 RK3288 | 需训练/转换 ncnn 模型 |
  | Android/CLI | 识别 | ArcFace 512D (FP32) | **SFace** 128D | CPU 上最快识别模型 | 需重新适配推理接口 |
  | Android/CLI | 双模型 | YOLO + ArcFace (FP32) | **INT8 量化版** | 推理加速 2-3x，内存减半 | 需 ncnn2int8 量化工具链 |
  | Windows | 检测 | LBP Cascade | **ResNet SSD Face** (已内置) | DNN 检测精度显著高于 Cascade | 配置 `dnn.enable=true`，无需代码改动 |
  | Windows | 识别 | LBPH | **ArcFace ONNX** | 精度从 <90% 提升至 99.8% | 需集成 ONNX Runtime 推理 |

- **RK3288 部署建议分级**：
  - **追求精度**：YOLO Face (ncnn FP32) + ArcFace (ncnn FP32) — 推理约 15ms + 8ms
  - **追求速度**：SCRFD-0.5GF (ncnn) + MobileFaceNet (ncnn) — 推理约 5ms + 3ms
  - **追求极致**：任选模型 + ncnn INT8 量化 — 再加速 2-3x
- **目标**：建立模型注册与运行时切换框架，支持通过配置切换模型后端；补齐 INT8 量化工具链；补充 SCRFD/MobileFaceNet 等轻量模型的 ncnn 适配。
- **验收**：可通过 JSON 配置 `model.detection` / `model.recognition` 切换模型后端；INT8 量化模型在 RK3288 上推理 P95 延迟降低 50% 以上。
## 📄 许可证
MIT License
