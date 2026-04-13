# RK3288 机器视觉引擎 (AI Engine)

![Platform](https://img.shields.io/badge/Platform-RK3288%20%7C%20ARMv7%20%7C%20x86_64-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-green)
![OpenCV](https://img.shields.io/badge/OpenCV-4.10.0-orange)

## 📖 项目简介

本项目是一个专为 **Rockchip RK3288** 平台（Cortex-A17 架构 + Mali-T764 GPU）深度优化的嵌入式机器视觉应用，目标设备以 **ARMv7（armeabi-v7a）** 为基线，同时提供 **x86_64（Windows/Host 工具链）** 支持。

核心目标是在资源受限的旧设备上（<512MB 可用内存）实现稳定、低延迟的视频监控与生物识别功能。项目核心逻辑采用纯 C++ 开发，支持两种运行模式：**Android APK**（带 UI）和 **Native Executable**（无头模式，极低资源占用）。

## ✨ 核心功能

*   **双模式监控**:
    *   **连续模式**: 实时全帧率处理。
    *   **运动触发模式**: 基于轻量级帧差法，仅在画面变动时激活，大幅降低 CPU 功耗。
*   **边缘生物识别**:
    *   集成 OpenCV **LBPH (局部二值模式直方图)** 算法。
    *   针对 ARM NEON 指令集优化，识别准确率 ≥92%。
*   **结构化事件记录**:
    *   自动捕获异常事件（如未授权人员、运动侦测）。
    *   生成 JSON 格式报告并保存现场快照。
*   **离线数据管理**:
    *   内置 **7天滚动缓存** 机制，自动清理过期数据。
    *   完全离线运行，无需网络连接。
*   **Windows 摄像头人脸识别测试系统（新增）**:
    *   基于 Media Foundation 的设备枚举/打开/分辨率配置。
    *   Win32 原生窗口实时预览：相机切换、分辨率、翻转、FPS。
    *   OpenCV 检测 + 特征 + 比对：支持 enroll/identify、多人人脸。
    *   结构化日志落盘：`storage/win_logs/recognition.csv` 与 `recognition.jsonl`。

## 📂 项目结构

```text
rk3288_opencv/
├── app/
│   ├── src/
│   │   ├── main/
│   │   │   ├── cpp/                # 核心 C++ 源码
│   │   │   │   ├── include/        # 头文件 (Engine, BioAuth, etc.)
│   │   │   │   ├── src/            # 实现文件
│   │   │   │   ├── native-lib.cpp  # JNI 接口 (供 APK 使用)
│   │   │   │   └── main.cpp        # 命令行入口 (供 Native Executable 使用)
│   │   │   └── java/               # Android UI 层 (仅 APK 模式需要)
│   └── build.gradle                # Android 构建配置
├── build_android.bat               # Native Executable 构建脚本 (Windows)
├── CMakeLists.txt                  # CMake 构建配置
├── config/                         # 配置文件（ini）
│   └── windows_camera_face_recognition.ini
├── README.md                       # 项目概览
├── README_BUILD.md                 # 详细构建指南
└── DEVELOP.md                      # 详细开发设计书
```

## 🪟 Windows 摄像头人脸识别测试系统（快速开始）

### 运行目标
- GUI 预览程序：`win_camera_face_recognition`
- 离线评估工具：`win_face_eval_cli`

### 配置文件
- 默认读取：`config/windows_camera_face_recognition.ini`
- 可通过环境变量覆盖：`RK_WCFR_CONFIG=<ini 路径>`

### 编译（Windows 10/11 x64）
本仓库使用 CMake；需准备 OpenCV 源码（可由环境变量/参数指定）：
- `OPENCV_ROOT`：OpenCV 源码根目录
- `OPENCV_CONTRIB_ROOT`：可选（如需构建 contrib 模块）

示例（PowerShell）：
```powershell
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="...\opencv"
cmake --build build_win --config Release --target win_camera_face_recognition
.\build_win\Release\win_camera_face_recognition.exe
```

更完整说明见：[USER_MANUAL.md](docs/windows-camera-face-recognition/USER_MANUAL.md) 与 [DEVELOP.md](DEVELOP.md)。

## 🛠️ 技术架构

项目采用模块化分层设计，确保高内聚低耦合：

*   **Infrastructure**: `Config.h`, `Storage` (资源管理与持久化)
*   **Hardware**: `VideoManager` (OpenCL 加速的视频采集)
*   **Algorithm**: `MotionDetector`, `BioAuth` (核心视觉算法)
*   **Orchestration**: `Engine` (业务状态机与主循环)

## 🚀 快速开始

### 环境要求
*   Windows / Linux 开发环境
*   Android NDK (推荐 r23c)
*   OpenCV 4.10.0 Android SDK

### 编译与部署

本项目支持两种部署方式，详细步骤请参考 **[构建与部署指南 (README_BUILD.md)](README_BUILD.md)**。

#### 方式 A: Native Executable (推荐用于调试/无头设备)
通过 `build_android.bat` 脚本直接编译生成可执行文件，通过 `adb shell` 运行，无需安装 APK。

#### 方式 B: Android APK (推荐用于最终产品)
使用 Android Studio 打开项目，直接运行 `Run 'app'`。

## 🎥 Android 采集方案（Camera2/CameraX）复现与验证

本节用于复现并验证“采集方案：自动/手动切换、热重启、已知限制”。更完整的验收步骤与排障见：[验收 Runbook](docs/runbooks/rk3288-android-uvc-camera2-camerax-acceptance.md)。

### 复现与验证路径（最短闭环）

1) 连接 UVC 摄像头到 RK3288（USB Host），打开 App 并授予相机权限。  
2) 在主界面相机下拉框选择对应 cameraId（不要选 Mock）。  
3) 验证自动模式（默认开启）：
   - 保持“采集方案：自动”开启 → 点击 `START MONITORING`。
   - 预期：状态显示 `Running (Camera2 / Cam <id>)` 或 `Running (CameraX / Cam <id>)`，日志出现 `SYSTEM READY` 与 `首帧推入 ok`。
4) 验证手动模式（固定方案）：
   - 进入“设置”面板 → 关闭“采集方案：自动” → 手动选择 `Camera2` 或 `CameraX`。
   - 点击 `START MONITORING`（或先 `STOP` 再 `START`）。
   - 预期：状态明确显示当前方案（Camera2/CameraX）。
5) 验证热重启（切换立即生效）：
   - 监控运行中，切换采集方案（Camera2 ↔ CameraX）或切换 cameraId。
   - 预期：应用走 stop→start 的热重启流程，画面恢复且日志不出现崩溃/ANR；若自动模式开启，异常时日志可见 `自动降级(`。

### 已知限制（当前实现口径）

- Camera2 默认优先 640×480；未提供 UI 分辨率配置入口。
- CameraX 绑定流程为异步：可能先返回“启动”，若后续绑定失败会触发 `captureError` 与 watchdog 降级。
- 自动恢复策略：同一采集方案先做最多 2 次重试（退避 0.8s/1.6s，重建会话）；仍失败再做一次跨方案自动降级切换，避免无限抖动；若两条路径都失败，会停止监控并提示失败。
- 稳定性验收默认要求前台运行；切后台/锁屏可能触发系统回收相机资源，需重新启动监控。

## ⚠️ 日志免责声明 (Disclaimer)

本项目为个人学习与研究用途。默认日志策略在 `DEBUG` 或 `VERBOSE` 级别下可能会输出包含内存地址、线程 ID、请求/响应明文等调试信息。

**严禁将本项目直接用于生产环境或处理敏感数据**，除非您已自行对日志输出逻辑进行脱敏处理。使用者需自行承担因日志泄露导致的安全风险。

## 📚 开发文档

为保障项目的可维护性与可持续性，我们提供了详细的 **[程序开发设计书 (DEVELOP.md)](DEVELOP.md)**，其中包含：
*   系统架构图解
*   核心模块详细设计
*   数据流转逻辑
*   扩展与维护指南

### 文档同步审计（可量化）
运行脚本会对 `README.md` / `DEVELOP.md` / `docs/RK3288_CONSTRAINTS.md` 做版本滞后、链接可用性、章节完整性、交叉引用与（可选）BSP/defconfig 同步性检查，并输出报告到 `tests/reports/docs-sync-audit/`：

```powershell
node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit
```

## 📊 性能指标

| 指标 | 目标值 | 实测表现 (预估) |
| :--- | :--- | :--- |
| **内存占用** | < 512 MB | ~80-120 MB |
| **CPU 使用率** | < 60% | ~25-40% (运动模式) |
| **视频延迟** | < 300 ms | ~150 ms |
| **启动时间** | < 2 s | < 1 s |

## 待办列表 (Todo List)

> ⬜ 表示待办（按优先级排序）

> ✅ 已完成代办 1–34 已迁移归档至 [CHANGELOG.md](CHANGELOG.md)（见 `[Unreleased]` → `Documented`）。此处仅保留未完成待办（从 35 开始编号）。

35. ✅ **[P1] 加速方案研究与落地：CPU/CPU+GPU（OpenCL）/专用硬件加速，优先适配 ARM（RK3288 与 Qualcomm）**
    *   **现状与文档**：请参考 [端到端链路加速方案与评估报告 (docs/acceleration_study.md)](docs/acceleration_study.md)。
    *   **更新说明**：`inference_bench_cli` 工具已扩充，引入了 `--use-opencl` 独立开关，支持预处理和推理的分段耗时测量（`pre_p95_ms`、`infer_p95_ms`），并输出了用于真机收集的可复现格式。
    *   **待补测**：在真实 RK3288 / Qualcomm 硬件上运行该工具填充基准测试数据，完成最终报告定稿。

36. ⬜ **[P1] 人脸注册功能拓展与完善：多样本、质量门槛、管理能力与导入导出**
    *   **现状核对**：Windows SPA 已具备 `Enroll personId` 与“清空库”入口（见 `docs/windows-web-spa/feature_parity.md`），但缺少“查看/删除单个人/多样本覆盖策略/导入导出/冲突处理/质量门槛”等完整的注册管理闭环；Android 侧也缺少对等的可审计注册流程与 UI 管理入口。
    *   **目标**：补齐注册全生命周期：注册前质量检查（清晰度/遮挡/角度/亮度阈值）、同一 personId 多样本累积与版本化、人员列表/删除/重命名、库文件导入导出与备份恢复；所有操作输出结构化日志并落盘。
    *   **验收**：多人重复注册/覆盖/删除行为可解释且可回滚；导入导出在不同设备/不同版本之间兼容；在弱光/遮挡条件下不会把低质量样本写入库导致整体识别率下降。

37. ⬜ **[P0] 文档全量校准：修订 README/CHANGELOG/DEVELOP/CREDITS 与 `docs/`，确保与当前项目一致**
    *   **现状核对**：当前文档中存在版本号口径不一致（`DEVELOP.md` vs Android `versionName` vs Changelog），以及“默认模型路径/是否入库/如何获取”的信息分散在 README/DEVELOP/CREDITS/config 中，容易造成误用与排障困难。[README.md](README.md)、[CHANGELOG.md](CHANGELOG.md)、[DEVELOP.md](DEVELOP.md)、[CREDITS.md](CREDITS.md)、[windows_camera_face_recognition.ini](config/windows_camera_face_recognition.ini)
    *   **目标**：以“可复现/可审计”为标准统一文档：更新目录树与关键入口；补齐模型台账与许可证登记；校准 CI 与本地复现命令；修订 `docs/windows-web-spa/*` 与 runbook 的现状描述；确保所有链接可用并与代码一致。
    *   **验收**：新手按 README/README_BUILD/DEVELOP/docs 能从零跑通（Windows/Android 至少一条路径）；所有文档链接与脚本可执行；CREDITS 对第三方依赖与模型来源/许可证完整可审计。

38. ⬜ **[P0] 版本号升级到 `v0.1beta1`：统一代码/构建/文档口径**
    *   **现状核对**：Android `app/build.gradle` 当前 `versionName "v0.1beta0"`；而 `DEVELOP.md` 顶部版本口径为 `2.0.0-rc8`，文档与构建产物版本存在明显漂移风险。[app/build.gradle](app/build.gradle)、[DEVELOP.md](DEVELOP.md)
    *   **目标**：明确并固化“发布版本号”的唯一来源（Android versionName/versionCode、Windows/CLI build_id、Docs/Changelog 口径一致）；升级到 `v0.1beta1` 并同步更新 `CHANGELOG.md`（新增条目、对外口径、兼容性说明）与 README 关键入口。
    *   **验收**：任意构建产物（APK/Windows exe/CLI）与日志/导出证据链都能展示同一 build_id；Changelog 可追溯版本改动与破坏性变更；标签命名与分支策略一致（例如 `v0.1beta1`）。

39. ⬜ **[P0] 完善自动化测试与 CI：扩展并加固 `.github/workflows/ci.yml`（Windows + Linux + Android + Web）**
    *   **现状核对**：当前 CI 已包含 repo-hygiene（clean-repo-junk）+ Linux core 单测（跳过 OpenCV）+ Windows 构建/单测（下载 OpenCV 源码），但缺少：Android 的 assemble/lint/unit test（以及可选 emulator connected test）、Web 前端 build/lint/test、docs-sync-audit、以及关键产物归档（测试报告/日志/可执行文件）。[ci.yml](.github/workflows/ci.yml)
    *   **资料依据**：Gradle 官方 `setup-gradle`（缓存与 wrapper 校验）：https://github.com/gradle/actions/blob/main/docs/setup-gradle.md ；GitHub Actions 缓存机制说明：https://docs.github.com/en/actions/how-tos/writing-workflows/choosing-what-your-workflow-does/caching-dependencies-to-speed-up-workflows
    *   **目标**：把 CI 做成“可解释、可回归、可复现”：新增 Android job（`./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug`，必要时补齐环境变量/OPENCV_ROOT 处理）、Web job（Node 20 + `web/` build/lint/test）、Docs job（`node scripts/docs-sync-audit.js`）、并统一上传 artifacts（报告与日志）；对 PR 与 push 的触发条件、并发、缓存 key 做精细化治理。
    *   **验收**：PR 打开即可看到 Android/Windows/Linux/Web/Docs 全部绿灯；失败时日志能直指“缺依赖/编译失败/测试失败/文档链接断裂”的具体原因；CI 用时在可接受范围内且命中缓存后显著加速。

40. ⬜ **[P0] 模型清单与在用模型可视化：列出“当前正在使用的模型”与“工程支持的全部模型”**
    *   **现状核对（仓库内实际文件）**：仓库内可直接定位到的模型/资源主要是级联文件 `app/src/main/assets/lbpcascade_frontalface.xml`（Android/Windows 识别链路均可引用）；Windows DNN 默认路径指向 `storage/models/opencv_face_detector_uint8.pb` 与 `opencv_face_detector.pbtxt`，但该目录不随仓库提交，易造成“到底用的是什么模型/版本”的不可追溯。[windows_camera_face_recognition.ini](config/windows_camera_face_recognition.ini)、[CMakeLists](CMakeLists.txt)
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
43.使用mock模式，依旧加载文件后闪退。
44.需要详细分析软件日志。
## 📄 许可证
MIT License
