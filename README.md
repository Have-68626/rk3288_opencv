# RK3288 机器视觉引擎 (AI Engine)

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

```text
rk3288_opencv/
├── app/                  # Android 应用目录
│   ├── src/main/java/    # Android UI 代码
│   └── build.gradle      # Android 构建配置
├── config/               # 配置文件
│   └── windows_camera_face_recognition.ini
├── docs/                 # 开发设计文档与运维手册
│   ├── runbooks/         # 测试验收与排障流程
│   └── windows-web-spa/  # Web SPA 架构与功能对照
├── src/                  # 核心源码
│   ├── cpp/              # C++ 核心算法、工具与头文件
│   │   ├── src/          # C++ 源文件
│   │   ├── native-lib.cpp # JNI 入口
│   │   ├── main.cpp      # Native CLI 主循环入口
│   │   └── tools/inference_bench_cli.cpp # 推理基准测试工具
│   ├── java/             # 核心 Java 层源码
│   └── win/              # Windows 特定代码
│       ├── app/
│       │   ├── win_local_service_main.cpp # Windows 本地服务入口
│       │   └── webroot/  # Windows 静态资源落盘目录
│       └── src/          # Windows 侧代码
├── web/                  # Web SPA 前端源码
└── tests/                # 测试脚本、配置与输出目录
    ├── metrics/          # 基准 / 性能报告输出目录
    └── reports/          # CI 报告与审查日志输出目录
```

## 🛠️ 依赖列表与状态检查

> 📋 **完整的依赖状态检查清单（包含已满足、缺失、可选）请参阅 [CREDITS.md#依赖状态检查清单](CREDITS.md#依赖状态检查清单)。**

### 已满足（可开始构建）
- ✅ Windows 开发环境（VS 2022）
- ✅ Android NDK 27.0 及以上
- ✅ OpenCV 4.10.0（位于 `D:\ProgramData\OpenCV\`）
- ✅ CMake 3.22.1 及以上
- ✅ Gradle 9.0（内置 `gradlew.bat`）
- ✅ Node.js 22.x 及以上（含 pnpm 10.x）

### ❌ 缺失（仅影响特定功能）
- **DNN 模型文件**（影响 Windows DNN 人脸检测）：需下载 `.pb` + `.pbtxt` 至 `storage/models/` 或通过 Web UI 配置
  - 详见 [Model_Inventory.md](Model_Inventory.md) 与 [config/windows_camera_face_recognition.ini](config/windows_camera_face_recognition.ini)

### ⚠️ 可选（自动回退，不阻断核心功能）
- **RK MPP**（硬件解码加速）：已下载至 `D:\ProgramData\rkmpp`，需配置 CMake 环境后启用
- **Qualcomm SDK**（推理加速）：缺失时自动回退 CPU 推理
- **FFmpegKit AAR**（RTMP 推流）：仅 Android 远程推流需要，核心识别不需要

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

## 待办列表 (Todo List)

> ⬜ 表示待办（按优先级排序）

> ✅ 已完成代办 1–34 及 37–38 已迁移归档至 [CHANGELOG.md](CHANGELOG.md)（见 `[Unreleased]` / `[0.1.1-beta.1]` → `Documented`）。此处仅保留未完成待办。

35. ⬜ **[P1] 加速方案研究与落地：CPU/CPU+GPU（OpenCL）/专用硬件加速，优先适配 ARM（RK3288 与 Qualcomm）**
    *   ✅ **已完成项**：已有文档与 bench 工具（详见 [端到端链路加速方案与评估报告 (docs/acceleration_study.md)](docs/acceleration_study.md)）；`inference_bench_cli` 已支持 `--use-opencl` 独立开关与分段 P95 输出。
    *   ⬜ **待完成项（不含真机实测）**：补齐开关 `requested`/`effective`/`evidence` 的证据链输出；主链路 OpenCL 作为进程全局开关（process-wide）存在驱动碎片化风险，默认策略将调整为保守的可控开关并输出生效证据；专用硬件后端（MPP/Qualcomm SDK）方向已纳入评估矩阵，但仓库代码尚未落地。
    *   ⬜ **真机实测项**：后续在 RK3288/Qualcomm 真机运行 `inference_bench_cli` 填表定稿（**注：本次不做**）。

36. ⬜ **[P1] 人脸注册功能拓展与完善：多样本、质量门槛、管理能力与导入导出**
    *   **现状核对**：Windows SPA 已具备 `Enroll personId` 与“清空库”入口（见 `docs/windows-web-spa/feature_parity.md`），但缺少“查看/删除单个人/多样本覆盖策略/导入导出/冲突处理/质量门槛”等完整的注册管理闭环；Android 侧也缺少对等的可审计注册流程与 UI 管理入口。由于当前模型基于 OpenCV DNN，仅支持轻量注册与演示。
    *   **目标**：补齐注册全生命周期：注册前质量检查（清晰度/遮挡/角度/亮度阈值）、同一 personId 多样本累积与版本化、人员列表/删除/重命名、库文件导入导出与备份恢复；所有操作输出结构化日志并落盘。
    *   **验收**：多人重复注册/覆盖/删除行为可解释且可回滚；导入导出在不同设备/不同版本之间兼容；在弱光/遮挡条件下不会把低质量样本写入库导致整体识别率下降。

39. ⬜ **[P0] 完善自动化测试与 CI：扩展并加固 `.github/workflows/ci.yml`（Windows + Linux + Android + Web）**
    *   **现状核对**：当前 CI 已包含 repo-hygiene（执行 `node scripts/clean-repo-junk.js scan --ci`）+ Linux core 单测（跳过 OpenCV）+ Windows 构建/单测（下载 OpenCV 源码），但缺少：Android 的 assemble/lint/unit test（以及可选 emulator connected test）、Web 前端 build/lint/test、docs-sync-audit（执行 `node scripts/docs-sync-audit.js`）、以及关键产物归档（测试报告/日志/可执行文件）。[ci.yml](.github/workflows/ci.yml)
    *   **资料依据**：Gradle 官方 `setup-gradle`（缓存与 wrapper 校验）：https://github.com/gradle/actions/blob/main/docs/setup-gradle.md ；GitHub Actions 缓存机制说明：https://docs.github.com/en/actions/how-tos/writing-workflows/choosing-what-your-workflow-does/caching-dependencies-to-speed-up-workflows
    *   **目标**：把 CI 做成“可解释、可回归、可复现”：新增 Android job（`./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`，必要时补齐环境变量/OPENCV_ROOT 处理）、Web job（Node 20 + `web/` build/lint/test）、Docs job（`node scripts/docs-sync-audit.js`）、并统一上传 artifacts（报告与日志）；对 PR 与 push 的触发条件、并发、缓存 key 做精细化治理。
    *   **验收**：PR 打开即可看到 Android/Windows/Linux/Web/Docs 全部绿灯；失败时日志能直指“缺依赖/编译失败/测试失败/文档链接断裂”的具体原因；CI 用时在可接受范围内且命中缓存后显著加速。

40. ⬜ **[P0] 模型清单与在用模型可视化：列出“当前正在使用的模型”与“工程支持的全部模型”**
    *   **现状核对（仓库内实际文件）**：仓库内可直接定位到的模型/资源主要是级联文件 `app/src/main/assets/lbpcascade_frontalface.xml`（Android/Windows 识别链路均可引用）；Windows DNN 默认路径指向 `storage/models/opencv_face_detector_uint8.pb` 与 `opencv_face_detector.pbtxt`，但该目录不随仓库提交（需由部署环境提供），易造成“到底用的是什么模型/版本”的不可追溯。[windows_camera_face_recognition.ini](config/windows_camera_face_recognition.ini)、[CMakeLists](CMakeLists.txt)
    *   **现状核对（代码支持但依赖外部交付）**：工程内已具备“可切换后端/模型路径”的能力（YOLO 人脸检测：OpenCV DNN 或 ncnn；ArcFace 特征：OpenCV DNN 或 ncnn；并包含 `modelVersion/preprocessVersion` 字段用于版本化），但缺少统一的“模型登记表/加载自检/运行时查询 API/日志口径”。[YoloFaceDetector](src/cpp/src/YoloFaceDetector.cpp)、[ArcFaceEmbedder](src/cpp/src/ArcFaceEmbedder.cpp)、[FaceInferencePipeline](src/cpp/src/FaceInferencePipeline.cpp)
    *   **目标**：输出一份“模型台账（Model Inventory）”：按功能（人脸检测/人脸识别/物体检测/物体识别）列出模型名称、路径来源（仓库/部署侧/环境变量）、格式、输入输出、版本号、hash、许可证、运行后端；并提供运行时查询入口（例如 Windows `/api/v1/settings` 扩展或新增 `/api/v1/models`）与启动自检日志（缺失/维度不匹配/模型损坏）。
    *   **验收**：用户无需读代码即可确定“当前在用模型 + 版本 + hash”；模型文件缺失或被替换时启动自检能明确报错并给出修复指引；CREDITS 里能追溯每个模型来源与许可证。

41. ⬜ **[P0] 人员管理与权限管理（注册系统升级）：人员信息/权限/安全/导入导出一体化**
    *   **现状核对**：Windows 侧当前“注册”本质为 `personId -> embedding 均值 + 样本数` 的轻量库（`FaceDatabase`/`FaceRecognizer`），UI 仅暴露“注册/清空库”（`/api/v1/actions/enroll`、`/api/v1/actions/db/clear`）；缺少人员信息字段（姓名/工号/角色/有效期等）、权限模型（门禁点/时间段/设备范围）、审计与可撤销操作。[FaceDatabase](src/win/include/rk_win/FaceDatabase.h)、[FaceRecognizer](src/win/src/FaceRecognizer.cpp)、[HttpFacesServer](src/win/src/HttpFacesServer.cpp)、[PreviewPage](web/src/app/pages/PreviewPage.tsx)
    *   **现状核对（安全）**：Windows 已有 DPAPI 保护密钥/配置的实现基础（`WinCrypto.cpp`），Android 已有基于 Keystore 的 AES-GCM 加密模板落盘（`FeatureTemplateEncryptedStore.java`），但尚未形成“人员数据/模板/权限/导入导出”统一的安全与合规口径。[WinCrypto](src/win/src/WinCrypto.cpp)、[FeatureTemplateEncryptedStore](src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java)
    *   **资料依据**：Windows DPAPI（CryptProtectData/CryptUnprotectData）官方文档：https://learn.microsoft.com/en-us/windows/win32/api/dpapi/nf-dpapi-cryptprotectdata ；Android 加密与 Keystore 建议：https://developer.android.com/guide/topics/security/cryptography
    *   **目标**：建立人员实体（personId + profile + role/permissions + 状态机），支持增删改查、批量导入导出（加密 ZIP/签名可选）、权限校验与审计日志；定义最小化存储字段与脱敏策略；导入导出兼容版本演进（schema version）并可回滚。
    *   **验收**：可在 UI 中完成“新增/禁用/删除/导入/导出/权限变更”全流程；导入导出文件被篡改/版本不兼容时可识别并拒绝；默认不把敏感字段明文落盘/落日志。

42.预览画面依旧存在应用返回桌面后，再打开应用后画面卡死的问题，热重启没有解决问题。
43.使用 mock 模式，依旧加载文件后闪退。
44.需要详细分析软件日志。
## 📄 许可证
MIT License
