# RK3288 机器视觉引擎 (AI Engine)

**更新日期**: 2026-06-23

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
| `docs-sync-audit.js` | 审计文档同步状态 | `node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit --link-cache tests/reports/docs-sync-audit/link-cache.json` |
| `verify_opencv_host.bat` | 验证 OpenCV 主机环境 | 设置 `OPENCV_ROOT` 后执行 |
| `verify_faces_test_set01.bat` | 人脸检测参数验证 | 依赖 `tests/test_set01/` 测试集 |
| `run-web-e2e.ps1` | Web SPA 端到端测试 | Cypress E2E + 覆盖率报告 |
| `bench_camera_adb.ps1` / `.sh` | Android 摄像头基准测试 | 通过 ADB 连接设备测试 |
| `stability_switch_50_adb.ps1` | Android 前后台稳定性验收（50 次） | 输出到 `tests/reports/stability/<timestamp>/` |

## 待办列表 (Todo List)

> ✅ 已完成 / 🟡 部分完成 / ❌ 未开始 / ⬜ 待办

### 1. ✅ **[P0] 核心稳定性治理 (Stability & Bug Fixes)**

| 子项 | 状态 | 备注 |
|:----|:----:|:-----|
| Mock 调用前预检 | ✅ 已完成 | `VideoManager::preflightMockInput()` — `MOCK_MAGIC_INVALID` / `MOCK_FILE_INCOMPLETE` / `MOCK_OVERSIZE` 拒绝码 |
| Mock 加载时防护 | ✅ 已完成 | 分块 1MB 读取 + 50MB 上限 + OOM catch (`VideoManager.cpp`) |
| Mock 解析后验证 | ✅ 已完成 | 首帧格式/分辨率/帧率预检 (`VideoManager.cpp:captureLoop`) |
| Mock fixture 测试 | ✅ 已完成 | `tests/cpp/test_mock_preflight_guards.cpp` — 3 tests PASS；fixtures: `tests/fixtures/mock/corrupt_magic.jpg`, `incomplete.jpg` |
| Android 前后台 50 次切换 | 🟡 脚本就绪，未实机验收 | `scripts/stability_switch_50_adb.ps1` 已实现；`tests/reports/stability/` 无报告输出 |
| `handleAbnormalEvent` 治理 | 🟡 代码完成，缺基线对比 | 会话级统计 + 自适应冷却已实现；缺前后同输入集对比数据 |

### 2. ✅ **[P0] 链路加速方案落地 (Acceleration)**

**代码实现全部完成**（7/7 项），**缺 RK3288 真机实测数据**。

| 优先级 | 加速项 | 代码状态 | 真机测量 | 备注 |
|:------:|:-------|:--------:|:--------:|:-----|
| **P0** | 推理调度优化 | ✅ `InferenceThrottle` (Off/Manual/Auto) + `detectStride_` (1-8) | ❌ | |
| **P0** | **YOLO ncnn 推理** | ✅ `CreateNcnnYoloFaceDetector()` + `YoloFaceAdapter` | ❌ | |
| **P0** | **ArcFace ncnn 推理** | ✅ `ArcFaceEmbedderConfig::BackendType::Ncnn` | ❌ | |
| P1 | Mock 视频 MPP 硬解码 | ✅ `MppDecoder` 已集成 (`VideoManager.cpp`) | ❌ | Android 专用 (`!ANDROID` 时跳过) |
| P1 | 帧预处理 libyuv | ✅ `RK_ENABLE_LIBYUV=ON`, `toBgrFromNv21()` | ❌ | |
| P1 | **模型 INT8 量化** | ✅ `scripts/quantize_ncnn_int8.py` + 3 模型 + 精度 ≥ 0.90 | ❌ | 参见下方 §INT8 |
| P2 | 1:N 搜索 NEON | ✅ `FaceSearchLinearIndex` NEON/SIMD + x86 回退 | ❌ | |
| — | 加速证据日志输出 | ✅ `Engine::performAccelSelfCheck()` 全加速器覆盖 | ❌ | 日志中已记录启用/回退原因 |

> ⚠️ **关键缺口**：所有加速项均已完成代码实现，但 RK3288 真机实测数据缺失。需在 RK3288 上执行 `inference_bench_cli` 及 `stability_switch_50_adb.ps1`，将结果填入
DEVELOP.md 附录 B 加速机会矩阵。

### 3. 🟡 **[P0] AI 模型与管线管控 (Model & AI Pipeline)**

| 子项 | 状态 | 备注 |
|:----|:----:|:-----|
| `/api/v1/models` 查询端点 | ✅ 已完成 | `HttpFacesServer.cpp` — 返回 `supportedModels` + `activeModels` (hash/backend/status/isInUse) |
| `/api/v1/models/reload` 重载 | ✅ 已完成 | 支持单模型按 ID 重载 |
| 检测参数 JSON 持久化 | ✅ 已完成 | `ModelConfig` (detection/recognition/backend/int8Enabled) 读写 `WinJsonConfig.cpp` |
| 模型 hash 与状态追踪 | ✅ 已完成 | `FramePipeline::ModelSnapshot` — hash/configuredPath/resolvedPath/status/backend |
| Web UI 实时查看 | ✅ 已完成 | SettingsPage 后端 Tab 新增"模型状态 (Model Status)"面板，展示摘要/活跃模型/状态/重载 |

### 4. ✅ **[P0] 自动化测试与 CI 加固 (CI/CD)**

**6 个 CI job 全部运行中**（`.github/workflows/ci.yml`）：

| Job | 触发条件 | 内容 |
|:----|:--------:|:-----|
| repo-hygiene | PR/push → master | `node scripts/clean-repo-junk.js scan --ci` |
| unit-tests | PR/push → master | Linux CMake + Ninja, `RK_SKIP_OPENCV=ON`, `core_unit_tests` + `src/win/src/*.cpp` 交叉编译检查 |
| docs-audit | PR/push → master | `node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit --link-cache tests/reports/docs-sync-audit/link-cache.json` |
| web | PR/push → master | `pnpm install` → `pnpm lint` → `pnpm build` → `pnpm e2e:run:coverage` (非阻塞) |
| android | PR/push → master | `gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug` |
| windows | push only (含 "Windows" 关键词或 workflow_dispatch) | 完整 OpenCV 源码构建 + ctest + INT8 量化 + 精度测试 |

### 5. ❌ **[P1] 人员注册与权限系统 (Personnel & Enrollment)**

| 子项 | 状态 | 备注 |
|:----|:----:|:-----|
| 单样本均值注册 | 🟡 基础实现 | `FaceRecognizer::enrollFromFrame()` 已存在 |
| 注册前质量门槛（亮度/角度/清晰度） | ❌ 未实现 | 无 pre-enroll 质量检查 |
| 人员实体模型（personId + profile + 状态机） | ❌ 未实现 | `personId` 仅用字符串 |
| 权限校验 | ❌ 未实现 | 无角色/权限系统 |
| 加密批量导入导出 | ❌ 未实现 | 无导入导出机制 |

### 6. 🟡 **[P1] 工程规范与治理 (Engineering Excellence)**

| 子项 | 状态 | 备注 |
|:----|:----:|:-----|
| `initEngine()` 双重调用治理 | 🟡 部分完成 | C++ 侧 ✅（`Engine.h` `atomic<bool>` exchange 守卫），Java 侧 🟡（`engineInitialized` 已加，9 个调用点入口未防护） |
| 废弃代码清理 | ✅ 已完成 | `native-lib-stub.cpp` 删除 11 个废弃 JNI 存根，`native-lib.cpp` 删除无 Java 声明的 `nativePushFrameNV21`/`NV21Bytes` |
| 开发流程文档 | ✅ 已完成 | `DEVELOP.md` 已补充构建变体/测试框架/构建要点/INT8 量化/加速契约，覆盖构建→测试→CI 全链路 |
| `Engine::initialize()` 调用次数验证 | 🟡 未验证 | 缺少运行时日志断言验证 |

### 7. ✅ **[P2] 多模型集成与切换策略 (Model Diversity)**

| 模型/能力 | 状态 | 备注 |
|:----------|:----:|:-----|
| YOLO Face (detect, FP32) | ✅ 已完成 | ncnn + OpenCV 双后端 |
| SCRFD-0.5GF (detect) | ✅ 已完成 | 注册为 `scrfd_0.5gf`，`models/scrfd_face_detect_ncnn/` 含 16 变体 |
| ArcFace 512D (recognize, FP32) | ✅ 已完成 | 注册为 `arcface`，bin 约 87MB |
| MobileFaceNet 128D (recognize, FP32) | ✅ 已完成 | 注册为 `mobilefacenet` |
| **INT8 量化版（3 模型）** | ✅ 已完成 | 脚本 `scripts/quantize_ncnn_int8.py` + 8 测试 + ArcFace cosine ≥ 0.90 |
| 运行时配置切换 | ✅ 已完成 | `ModelConfig::detection`/`recognition`/`int8Enabled` → `FaceInferStages` |
| 模型注册框架 | ✅ 已完成 | `ModelRegistry` — 按文件存在条件注册 |
| **YuNet (OpenCV)** | ✅ 已完成 | `YuNetAdapter` 封装 `cv::FaceDetectorYN`，注册为 `yunet` |
| **SFace 128D** | ✅ 已完成 | `SFaceAdapter` 封装 `cv::FaceRecognizerSF`，注册为 `sface` |
| **SCRFD/RetinaFace (det_10g)** | ✅ 已完成 | `RetinaFaceAdapter` FPN 多尺度解码 + NMS，注册为 `retinaface_scrfd` |
| **Windows ArcFace ONNX** | ✅ 已完成 | `ArcFaceWinRecognizer` 实现 `IRecognizer` 接口，`FramePipeline` 通过 `cfg_.model.recognition` 分支集成 |
| RK3288 INT8 加速实测 | ❌ 缺失 | 代码完成，缺真机 P95 延迟数据 |

> **RK3288 部署建议分级**（基于估算，待实测验证）：
> - **追求精度**：YOLO Face (ncnn FP32) + ArcFace (ncnn FP32)
> - **追求速度**：SCRFD-0.5GF (ncnn) + MobileFaceNet (ncnn)
> - **追求极致**：任选模型 + INT8 量化

## 📄 许可证
MIT License

### 密码学声明

本项目使用 AES-256-GCM（通过 Android KeyStore / Windows BCrypt）进行人脸特征模板加密。
密码学功能仅用于本地数据保护，不涉及通信加密或数字签名。

## 项目治理计划

> 基于 2026-07-04 全项目审计报告 ([AUDIT_REPORT.md](AUDIT_REPORT.md))，按模块分批次渐进治理。
> 设计文档: [governance-plan-design.md](docs/superpowers/specs/2026-07-04-governance-plan-design.md)

### 进度总览

| 批次 | 模块 | 评分(前→后) | 状态 | 进度 |
|:----:|:-----|:-----------:|:----:|:----:|
| Batch 0 | 治理看板初始化 | — | ✅ 完成 | 1/1 |
| Batch 1 | 法律 + 构建基础设施 | 2.3→3.5 | ✅ 完成 | 8/8 |
| Batch 2 | C++ 核心引擎 | 2.8→3.5 | ⬜ 待启动 | 0/7 |
| Batch 3 | Windows 本地服务 | 2.65→3.5 | ⬜ 待启动 | 0/8 |
| Batch 4 | Web SPA + Android | 3.7/3.1→4.0/3.8 | ⬜ 待启动 | 0/8 |
| Batch 5 | 测试覆盖 | 3.0→4.0 | ⬜ 待启动 | 0/5 |
| 持续 | 文档合规 | 3.0→4.0 | ⬜ 穿插 | 0/7 |

### Batch 1 子任务清单

- [x] 1.1 添加 LICENSE 文件
- [x] 1.2 修复 CMake 编译宏泄漏
- [x] 1.3 CORE_SOURCES 变量抽象
- [x] 1.4 rk_core 静态库创建
- [x] 1.5 修复 CI Windows PR 门控
- [x] 1.6 OpenCV 链接变量提取
- [x] 1.7 配置 CI ccache + Gradle 缓存
- [x] 1.8 出口合规声明
