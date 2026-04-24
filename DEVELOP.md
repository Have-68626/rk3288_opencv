# RK3288 AI Engine - 开发指南 (Development Guide)

**版本**: v0.1beta1
**日期**: 2026-04-16  
**状态**: 草案 (Draft)  

---

## 1. 概述 (Overview)

### 1.1 背景与目标 (Background & Objectives)
本项目旨在为 **Rockchip RK3288** 平台（ARM Cortex-A17 四核、Mali-T764 GPU；目标设备不含 NPU，仅 CPU+GPU）提供一套高性能、低资源的机器视觉解决方案。
核心目标是**在有限的算力下（< 60% CPU, < 512MB RAM）实现实时的视频监控（720p@30fps）与生物识别（< 100ms 延迟）**。

目标设备画像与硬约束入口：`docs/RK3288_CONSTRAINTS.md`。

### 1.2 术语对照表 (Terminology)
<a id="tbl-1-1"></a>
#### 表 1-1 术语对照表
| 中文术语 | 英文术语 | 说明 |
| :--- | :--- | :--- |
| 视频采集 | Video Capture | 从摄像头获取原始图像帧的过程 (V4L2) |
| 硬件编解码 | Hardware Codec | 利用专用硬件 (MPP/VPU) 进行视频压缩/解压 |
| 神经网络处理单元 | NPU | Neural Processing Unit, 瑞芯微平台的 AI 加速器 (RKNN) |
| 直接渲染管理器 | DRM | Direct Rendering Manager, Linux 内核显示子系统 |
| 内核模式设置 | KMS | Kernel Mode Setting, 负责显示控制器配置 |
| 零拷贝 | Zero-Copy | 避免数据在 CPU/GPU 内存间不必要的复制 |

### 1.3 目录结构与 GitHub 映射
```text
rk3288_opencv/
├── app/                  # Android APK 源码 (Java/Kotlin + C++) [GitHub]
├── src/                  # 核心源码目录 [GitHub]
│   ├── java/             # Android Java 源码（通过 app sourceSets 引用）
│   ├── cpp/              # Native 核心实现（JNI/引擎/视频/存储等）
│   └── win/              # Windows 本地服务与业务逻辑（无头/HTTP/硬件调度）
├── web/                  # Web SPA 前端源码 (React/Vite) [GitHub]
├── config/               # 配置文件与 Schema [GitHub]
├── docs/                 # 项目文档 [GitHub]
│   ├── windows-web-spa/  # Web SPA 架构与 API 说明
│   └── runbooks/         # 测试验收与排障流程
├── scripts/              # 构建与验证脚本 [GitHub]
├── tests/                # 测试脚本、配置与输出目录 [GitHub]
├── CMakeLists.txt        # Native 构建脚本 (CMake 3.18.1+) [GitHub]
├── DEVELOP.md            # 本开发文档 [GitHub]
├── README.md             # 项目主页 [GitHub]
├── CREDITS.md            # 致谢与许可证 [GitHub]
└── CHANGELOG.md          # 变更日志 [GitHub]
```

---

## 2. 环境准备 (Environment Setup)

### 2.1 快速开始 (Quick Start)
本仓库当前开发/部署主路径为：
- Windows：主开发环境（构建、运行、验证、工具脚本）
- Android：APK 部署到 RK3288 设备（`armeabi-v7a`）
- Linux：仅用于 CI/CD（快速发现编译/测试/脚本问题），不作为部署目标

> 重要提示（边界说明）：
> - 仓库已启用的 CI 工作流见：`.github/workflows/ci.yml`，主要用于“仓库卫生扫描 + 最小单测编译运行”。
> - 本文档中出现的“CI 模板/示例”会明确标注为模板，不代表仓库已经启用同名工作流或脚本。

#### 2.1.1 Windows 端最小准备（建议）
1) 安装 Visual Studio 2022（勾选“使用 C++ 的桌面开发”）
2) 安装 Android Studio（可选：仅当你需要构建/部署 APK）
3) 安装 Node.js 20（可选：仅当你需要运行 `scripts/*.js` 的审计/清理工具）

#### 2.1.2 Windows 快速构建（最小闭环，不依赖 OpenCV）
在仓库根目录打开 PowerShell，执行：

```powershell
$CMAKE_EXE = "cmake"
$CTEST_EXE = "ctest"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  $sdk = $env:ANDROID_SDK_ROOT
  $c1 = Join-Path $sdk "cmake\\4.1.2\\bin\\cmake.exe"
  $c2 = Join-Path $sdk "cmake\\3.22.1\\bin\\cmake.exe"
  $t1 = Join-Path $sdk "cmake\\4.1.2\\bin\\ctest.exe"
  $t2 = Join-Path $sdk "cmake\\3.22.1\\bin\\ctest.exe"
  if (Test-Path $c1) { $CMAKE_EXE = $c1; $CTEST_EXE = $t1 }
  elseif (Test-Path $c2) { $CMAKE_EXE = $c2; $CTEST_EXE = $t2 }
  else { throw "未找到 cmake/ctest：请安装 CMake，或安装 Android Studio 并设置 ANDROID_SDK_ROOT" }
}

& $CMAKE_EXE -S . -B build_smoke -G "Visual Studio 17 2022" -A x64 -DRK_SKIP_OPENCV=ON -DRK_BUILD_CORE_UNIT_TESTS=ON
& $CMAKE_EXE --build build_smoke --config Release --target core_unit_tests
& $CTEST_EXE --test-dir build_smoke -C Release --output-on-failure
```

#### 2.1.3 Android APK 快速构建（可选）
在仓库根目录打开 PowerShell，执行：

```powershell
.\gradlew.bat --no-daemon clean assembleDebug testDebugUnitTest lintDebug
```

### 2.2 技术栈选型 (Tech Stack Selection)
请根据项目需求勾选以下配置（**默认推荐加粗**）：

<a id="tbl-2-1"></a>
#### 表 2-1 技术栈选型对照表
| 组件 | 选项 A (保守/稳定) | 选项 B (激进/高性能) | 用户选择 | 选择后果说明 |
| :--- | :--- | :--- | :--- | :--- |
| **构建系统** | **CMake 3.18.1+** | CMake 3.22+ | [A] 3.18.1 | 高版本 CMake 支持更好的依赖管理，但老旧服务器可能不支持。 |
| **C++ 标准** | **C++17** | C++20 | [A] 17 | C++20 提供协程等新特性，但需较新版本的编译器 (GCC 10+)。 |
| **OpenCV** | **3.4.16** | 4.10.0 | [B] 4.10.0 | 3.x 兼容性好，旧代码迁移成本低；4.x 支持 DNN 模块更完善。 |
| **推理后端** | OpenCV DNN (CPU) | **ncnn（CPU / 可选 GPU）** | [B] | CPU 推理通用性强但慢；ncnn 端侧部署成熟、体积小，GPU 加速不作为强依赖前提。 |
| **视频后端** | **V4L2 (原生)** | GStreamer | [x] | V4L2 延迟最低且可控；GStreamer 功能丰富但引入额外依赖。 |
| **Web 前端** | - | **React 18 + TS + AntD 5** | [B] | Windows 端采用 Web SPA 提供本地 UI 控制台，抛弃老旧 Win32 UI。 |
| **显示后端** | Android Surface | **DRM/KMS (Linux)** | [A] | Android 适合 APP 开发；DRM 适合嵌入式纯 Linux 极速显示。 |

> **注**: 对于 Android 平台，推荐使用 **CameraX + Native 混合架构** (见 4.5 节)，兼顾预览流畅性与算法性能。

### 2.3 Windows（本机）开发环境记录

本节用于记录 Windows 主机侧常变信息（路径/版本/环境变量），并标注修改入口，便于一键复现与迁移。

#### 2.3.1 已安装软件与路径
- Android Studio：`D:\Program Files (x86)\AndroidStudio`
- Android Studio SDK：`D:\ProgramData\AndroidStudioSDK`
- Visual Studio：VS 2022
- Visual Studio Code：`D:\Program Files (x86)\vscode`

#### 2.3.2 推荐环境变量（避免脚本硬编码）
- `ANDROID_SDK_ROOT`：指向 Android Studio SDK 根目录（例：`D:\ProgramData\AndroidStudioSDK`）
- `ANDROID_NDK_HOME`：指向 NDK 根目录（可选；脚本可从 SDK 自动定位）
- `OPENCV_ROOT`：指向 OpenCV 源码根目录（建议版本：4.10.0；修改入口：CMake 变量/环境变量）
- `OPENCV_CONTRIB_ROOT`：指向 OpenCV Contrib 源码根目录（建议版本：4.10.0；可选）
- `RK_WCFR_CONFIG`：Windows 配置文件的默认存储路径（建议：`%APPDATA%\rk_wcfr\config.json`）。旧版 INI 仅作初始迁移使用。
- `NCNN_DIR`：指向 NCNN 库的 CMake 配置目录（可选；例：`D:\ProgramData\NCNN\ncnn-20260113-windows-vs2022\x64\lib\cmake\ncnn`）
- `RK_MPP_HOME`：指向 RK MPP 库根目录（✅ 已设置为 `D:\ProgramData\rkmpp\mpp-1.0.11`）
- *注：配置项（如模型路径、HTTP 端口等）均应通过 Web UI (`PUT /api/v1/settings`) 修改，不再推荐通过环境变量覆盖。*

#### 2.3.3 **依赖状态检查清单（重要）**

> 📋 **完整清单：** 详见 [CREDITS.md#依赖状态检查清单](../CREDITS.md#依赖状态检查清单)

| 状态 | 依赖 | 位置 | 修复方法 |
|--|--|--|--|
| ✅ 已满足 | OpenCV 4.10.0 | `D:\ProgramData\OpenCV\opencv-4.10.0` | — |
| ✅ 已满足 | NCNN (Windows & Android) | `D:\ProgramData\NCNN\ncnn-20260113-*` | — |
| ✅ 已满足 | Android NDK 27.0 | `D:\ProgramData\AndroidStudioSDK\ndk\27.0.12077973` | — |
| ✅ 已满足 | LBP Cascade | `app/src/main/assets/lbpcascade_frontalface.xml` | — |
| ❌ 缺失 | **DNN 模型文件** | `storage/models/opencv_face_detector_*.pb[txt]` | 下载至 `storage/models/` 或通过 Web UI 配置 |
| ❌ 缺失 | **Windows HDF5 架构** | `build/CMakeCache.txt` | 重建 x64 目录或 `-DBUILD_opencv_hdf=OFF` |
| ⚠️ 可选 | **RK MPP** | `D:\ProgramData\rkmpp\mpp-1.0.11` | ✅ 已自动配置（见下方说明） |
| ⚠️ 可选 | **Qualcomm SDK** | 不存在 | 需从官方获取（可自动回退 CPU） |
| ⚠️ 可选 | **FFmpegKit AAR** | `app/libs/ffmpeg-kit.aar` | 需下载或禁用 RTMP 推流 |

**当前影响：**
- ✅ **Android 侧**：构建成功（`./gradlew.bat :app:assembleDebug`），所有核心功能可用
- ✅ **Web 侧**：构建成功（`pnpm -C web build`）
- ❌ **Windows DNN**：人脸检测需要 `.pb`/`.pbtxt` 文件，缺失时无法使用
- ❌ **Windows 原生 CLI**：HDF5 架构冲突会导致链接失败（可跳过或修复）

#### 2.3.4 变动项修改入口（路径与工具链）
- Host 验证脚本：`scripts/verify_opencv_host.bat`
- Android 交叉编译脚本：`scripts/build_android.bat`

#### 2.3.5 相机链路审计（audit-camera-pipeline）复现与变更入口
- Spec：`.trae/specs/audit-camera-pipeline/`
- 审计报告：`docs/audit/camera_pipeline_audit_report.md`
- 回归测试计划：`docs/audit/camera_pipeline_regression_test_plan.md`
- 关键变更入口：
  - `src/java/com/example/rk3288_opencv/MainActivity.java`
  - `src/java/com/example/rk3288_opencv/PermissionStateMachine.java`
  - `app/src/main/AndroidManifest.xml`
  - `src/cpp/native-lib.cpp`
- Windows 命令行复现（在仓库根目录运行）：
  - `.\gradlew.bat assembleDebug`
  - `.\gradlew.bat testDebugUnitTest`
  - `.\gradlew.bat lintDebug`

#### 2.3.5 Windows 本地服务系统（Web SPA 架构）复现与变更入口
- Spec：`.trae/specs/refactor-win-preview-system/`
- Web SPA 架构文档：`docs/windows-web-spa/`
- 配置文件：
  - 核心配置（Source of Truth）：`%APPDATA%\rk_wcfr\config.json`
  - 初始迁移参考：`config/windows_camera_face_recognition.ini`
- 关键代码入口：
  - 本地主服务入口：`src/win/app/win_local_service_main.cpp`
  - JSON 配置管理：`src/win/src/WinJsonConfig.cpp`
  - 采集（Media Foundation）：`src/win/src/MfCamera.cpp`
  - 异常事件日志：`src/win/src/EventLogger.cpp`
  - DNN 检测：`src/win/src/DnnSsdFaceDetector.cpp`
  - 叠加绘制：`src/win/src/OverlayRenderer.cpp`
  - 输出端口（HTTP/SSE + 静态托管 + OpenAPI，使用 CivetWeb；只监听 127.0.0.1）：`src/win/src/HttpFacesServer.cpp`
  - 外部推送（POST）：`src/win/src/HttpFacesPoster.cpp`
  - 结构化日志：`src/win/src/StructuredLogger.cpp`
  - 离线评估：`src/win/tools/win_face_eval_cli.cpp`
  - 单元测试：`tests/win/`
- 本地 HTTP 最小联调入口：
  - 静态页面（Web UI）：`http://127.0.0.1:<port>/`（由 Vite 构建，从 `src/win/app/webroot` 托管）
  - OpenAPI：`http://127.0.0.1:<port>/openapi.json`
  - REST API：`GET /api/health`、`GET /api/faces`、`PUT /api/v1/settings` 等
- Windows 构建（仓库根目录）：
  - `cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="...\opencv"`
  - `cmake --build build_win --config Release --target win_local_service win_unit_tests win_face_eval_cli`
  - `ctest --test-dir build_win -C Release`

#### 2.3.6 Windows 本地服务 + 浏览器 SPA（推荐路径）
- Spec：`.trae/specs/migrate-win32-to-web-spa/`
- 二进制（CMake target）：
  - `win_local_service`：本地服务进程（无 Win32 UI），只监听 `127.0.0.1`
  - `win_camera_face_recognition`：旧版 Win32 UI（仅回滚用途；默认不构建）
- 配置文件（支持热重载与回滚）：
  - `%APPDATA%\rk_wcfr\config.json`
  - 密钥文件（DPAPI 保护）：`%APPDATA%\rk_wcfr\config.key.dpapi`
  - 备份文件：`%APPDATA%\rk_wcfr\config.json.bak`
- Web 前端：
  - 源码：`web/`
  - 构建产物输出目录：`src/win/app/webroot/`（Vite 直接输出；CMake POST_BUILD 会复制到 exe 同级 `webroot/`）
  - 访问入口：`http://127.0.0.1:<port>/`（SPA）与 `http://127.0.0.1:<port>/api/v1/*`（REST）

##### 2.3.5.1 显示相关配置项（ini）
修改入口：`config/windows_camera_face_recognition.ini` 的 `[display]` 段（或通过 UI“应用显示”持久化写回 ini）
- `output_index`：目标显示器索引（按 DXGI 输出枚举顺序）
- `width/height`：目标分辨率（0 表示随窗口/默认）
- `refresh_numerator/refresh_denominator`：目标刷新率（用于系统模式切换）
- `vsync`：1/0
- `swapchain_buffers`：2/3
- `fullscreen`：1/0
- `allow_system_mode_switch`：1/0（默认 0；仅在用户显式开启后才调用独占全屏/系统模式切换）
- `enable_srgb`：1/0
- `gamma`：例如 2.2
- `color_temp_k`：例如 6500
- `aa_samples`：1/2/4/8（按能力检测可用项）
- `aniso_level`：1/2/4/8/16

##### 2.3.5.2 稳定性/压测/基准复现命令（Windows）
二进制：`.\build_host\bin\Release\win_camera_face_recognition.exe`
- 稳定性测试模式（≥5 组分辨率+刷新率自动切换）：
  - `.\build_host\bin\Release\win_camera_face_recognition.exe --stability_test --stability_per_mode_sec 15`
- 72h 压测模式（输出内存/句柄/帧时间/异常事件日志）：
  - `.\build_host\bin\Release\win_camera_face_recognition.exe --soak_test --soak_hours 72`
- 性能基准采集（输出帧时间 P50/P95/P99 + 显存占用 best-effort）：
  - `.\build_host\bin\Release\win_camera_face_recognition.exe --perf_report --perf_seconds 60`

- A/B/C 测试模式（自动输出报告到 `docs/windows-camera-face-recognition/`）：
  - A：`.\build_host\bin\Release\win_camera_face_recognition.exe --test_a`
  - B：`.\build_host\bin\Release\win_camera_face_recognition.exe --test_b --test_b_repeats 10`
  - C：`.\build_host\bin\Release\win_camera_face_recognition.exe --test_c --test_c_seconds 3600`

##### 2.3.5.3 文档同步审计（README/DEVELOP/约束文档）
输出目录：`tests/reports/docs-sync-audit/`（JSON + Markdown 报告）

- 运行审计（默认）：
  - `node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit`
- 可选：尝试调用 markdownlint-cli2（需要联网下载，失败会在报告中体现）：
  - `node scripts/docs-sync-audit.js --markdownlint-cli2 --out-dir tests/reports/docs-sync-audit`
- BSP/内核同步：若要启用“约束文档 vs BSP Release Note / defconfig”的强校验，需要在仓库内提供以下输入（缺失会被报告为缺陷）：
  - BSP Release Note（最新）：`docs/bsp/BSP_RELEASE_NOTES.md`
  - defconfig（作为对齐基准）：`docs/bsp/defconfig/rk3288_defconfig`
  - 运行内核配置快照（建议从设备导出）：`docs/bsp/kernel-config/kernel.config`

#### 2.3.7 Android Camera2/CameraX（UVC）采集改造复现与验收入口

- Spec：`.trae/specs/fix-android-camera2-uvc-open-failure/`
- 验收 Runbook：`docs/runbooks/rk3288-android-uvc-camera2-camerax-acceptance.md`
- 关键代码入口（变更路径）：
  - UI 与状态机（自动/手动切换、热重启、SAFE MODE）：`src/java/com/example/rk3288_opencv/MainActivity.java`
  - Camera2 采集（ImageReader YUV_420_888）：`src/java/com/example/rk3288_opencv/Camera2CaptureController.java`
  - CameraX 采集（ImageAnalysis YUV_420_888）：`src/java/com/example/rk3288_opencv/CameraXCaptureController.java`
  - Native 外部帧输入与转换：`src/cpp/src/Engine.cpp`（外部帧通道、YUV_420_888/NV21 转换）
- 最短验收路径（摘要，完整步骤以 Runbook 为准）：
  1) RK3288 + UVC：运行 30 分钟无崩溃；日志需出现 `SYSTEM READY` 与 `首帧推入 ok`
  2) Mock 回归：`Mock Source (File Picker)` 与 `Mock Camera (System App)` 均可启动监控并出帧（静态图最稳）
  3) 权限拒绝：稳定进入 SAFE MODE，且不触发 Native 相机初始化（日志不应出现 `Engine initialize` / `外部帧输入通道 已启用`）
- 已知限制（当前实现口径）：
  - Camera2 默认优先 640×480；未提供 UI 分辨率配置入口（修改入口：`Camera2CaptureController.chooseYuvSize`）。
  - CameraX 绑定为异步：可能先“启动”，后续失败由 `captureError` 与 watchdog 触发降级（修改入口：`CameraXCaptureController.start` 与 `MainActivity.startCaptureWatchdog`）。
  - 自动切换为“单次运行最多切换一次”，避免无限抖动；若两条路径都失败，会停止监控并提示失败（修改入口：`MainActivity.handleCaptureFailure`）。

### 2.4 最短验收路径（Windows + Android）
目标是用最少步骤确认：仓库能构建、核心单测能跑、Android 工程配置不崩。

#### 2.4.1 Windows：核心单测（不依赖 OpenCV，推荐先跑）
在仓库根目录打开 PowerShell，执行：

```powershell
$CMAKE_EXE = "cmake"
$CTEST_EXE = "ctest"
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  $sdk = $env:ANDROID_SDK_ROOT
  $c1 = Join-Path $sdk "cmake\\4.1.2\\bin\\cmake.exe"
  $c2 = Join-Path $sdk "cmake\\3.22.1\\bin\\cmake.exe"
  $t1 = Join-Path $sdk "cmake\\4.1.2\\bin\\ctest.exe"
  $t2 = Join-Path $sdk "cmake\\3.22.1\\bin\\ctest.exe"
  if (Test-Path $c1) { $CMAKE_EXE = $c1; $CTEST_EXE = $t1 }
  elseif (Test-Path $c2) { $CMAKE_EXE = $c2; $CTEST_EXE = $t2 }
  else { throw "未找到 cmake/ctest：请安装 CMake，或安装 Android Studio 并设置 ANDROID_SDK_ROOT" }
}

& $CMAKE_EXE -S . -B build_ci_win_smoke -G "Visual Studio 17 2022" -A x64 -DRK_SKIP_OPENCV=ON -DRK_BUILD_CORE_UNIT_TESTS=ON
& $CMAKE_EXE --build build_ci_win_smoke --config Release --target core_unit_tests
& $CTEST_EXE --test-dir build_ci_win_smoke -C Release --output-on-failure
```

#### 2.4.2 Android：构建 + 单测 + Lint（需要 Android Studio SDK）
在仓库根目录打开 PowerShell，执行：

```powershell
.\gradlew.bat --no-daemon clean assembleDebug testDebugUnitTest lintDebug
```

#### 2.4.3 文档同步审计（可选，但发布前建议）
在仓库根目录打开 PowerShell，执行：

```powershell
node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit
```


---

## 3. 核心模块开发指南 (Core Development)

### 3.1 架构设计 (Architecture)
系统采用分层架构，核心业务逻辑下沉至 Native C++ 层，以最大化利用硬件性能。

<a id="fig-3-1"></a>
#### 图 3-1 系统分层架构（Mermaid）
```mermaid
graph TD
    subgraph "Hardware Layer"
        Cam[Camera Sensor] --> V4L2[V4L2 Driver]
        VPU[Video Process Unit] --> MPP[Rockchip MPP]
        CPU[ARM Cortex-A17 CPU（NEON）]
        GPU[Mali-T764 GPU（可选）]
        INF[ncnn / OpenCV DNN]
        CPU --> INF
        GPU --> INF
        HDMI[Display] --> DRM[DRM/KMS Driver]
    end

    subgraph "Native C++ Core"
        V4L2 --> VM[VideoManager]
        MPP --> VM
        VM --> Engine[Core Engine]
        Engine --> MD[MotionDetector]
        Engine --> BA[FacePipeline（YOLO + ArcFace）]
        INF --> BA
        Engine --> Display[DisplayManager]
        Display --> DRM
    end

    subgraph "Application Layer"
        Engine --> JNI[JNI Interface]
        JNI --> Java[Android UI]
    end
```

### 3.2 逻辑流程 (Logic Flow)
1.  **输入 (Input)**: `VideoManager` 通过 V4L2 获取 YUYV/NV12 原始帧。
2.  **处理 (Process)**:
    *   **预处理**: 缩放、色彩转换 (YUV -> RGB)。
    *   **检测**: `MotionDetector` 快速筛选动态帧。
    *   **识别**: 人脸检测/识别通过 `ncnn / OpenCV DNN`（YOLO 检测 + ArcFace 特征），并结合 1:N 检索与阈值策略输出业务结果。
3.  **输出 (Output)**:
    *   **显示**: 通过 DRM/KMS 直接渲染到 HDMI，或回调给 Android Surface。
    *   **日志**: 记录事件与性能指标。

---

## 4. 业务具体实现 (Implementation Details)

### 4.1 摄像头数据采集 (V4L2)
直接操作 `/dev/video0` 设备，使用 MMAP 内存映射方式获取数据，零拷贝。

<a id="tbl-4-1"></a>
#### 表 4-1 V4L2 采集流程（伪代码与示例）
| 伪代码 (Pseudocode) | 示例代码 (Example) |
| :--- | :--- |
| 1. 打开设备 `open("/dev/video0")`<br>2. 查询能力 `ioctl(VIDIOC_QUERYCAP)`<br>3. 设置格式 `ioctl(VIDIOC_S_FMT)` (YUYV, 640x480)<br>4. 请求缓冲 `ioctl(VIDIOC_REQBUFS)`<br>5. 内存映射 `mmap()`<br>6. 开启流 `ioctl(VIDIOC_STREAMON)`<br>7. 循环 `ioctl(VIDIOC_DQBUF)` -> 处理 -> `ioctl(VIDIOC_QBUF)` | [01_v4l2_capture.cpp](docs/examples/01_v4l2_capture.cpp)<br><br>关键点：<br>- 必须处理 `EAGAIN` 错误。<br>- YUYV 转 RGB 需高效算法 (NEON/OpenCL)。 |

### 4.2 硬件加速 JPEG 解码 (RK MPP)
利用 RK3288 的 VPU 进行硬解码，释放 CPU 资源。

<a id="tbl-4-2"></a>
#### 表 4-2 RK MPP 解码流程（伪代码与示例）
| 伪代码 (Pseudocode) | 示例代码 (Example) |
| :--- | :--- |
| 1. 创建 MPP 上下文 `mpp_create()`<br>2. 初始化解码器 `mpp_init(MPP_CTX_DEC, AVC)`<br>3. 准备数据包 `mpp_packet_init()`<br>4. 循环：<br>   - 填充数据 `mpp_packet_set_data()`<br>   - 发送包 `mpi->decode_put_packet()`<br>   - 获取帧 `mpi->decode_get_frame()`<br>5. 销毁上下文 `mpp_destroy()` | [02_rkmpp_decode.cpp](docs/examples/02_rkmpp_decode.cpp)<br><br>关键点：<br>- MPP 是异步的，put/get 需配合。<br>- 解码后的 Buffer 是物理地址，需映射后 CPU 才能访问。 |

### 4.3 端侧推理：ncnn / OpenCV DNN（YOLO 人脸检测 + ArcFace 人脸识别）
目标设备为 RK3288（仅 CPU+GPU），本项目默认以 **ncnn（主选）+ OpenCV DNN（备选/回退）** 作为端侧推理后端。

<a id="tbl-4-3"></a>
#### 表 4-3 ncnn / OpenCV DNN 推理闭环（伪代码与示例）
| 伪代码 (Pseudocode) | 示例代码 (Example) |
| :--- | :--- |
| 1. 加载 YOLO 人脸检测模型（ncnn/OpenCV DNN）<br>2. 读取帧（CameraX/V4L2 或离线图片）<br>3. 预处理：resize/letterbox/归一化<br>4. 推理：forward<br>5. 后处理：解析 bbox +（可选）5 点关键点 + NMS<br>6. 对齐：优先关键点仿射，否则 bbox 裁剪+resize 到 112×112<br>7. ArcFace 特征：输出 512D embedding（L2 归一化）<br>8. 1:N 检索：TopK + 余弦相似度<br>9. 阈值策略：版本化阈值 + 连续 K 次通过触发<br>10. 输出：事件 JSON + 审计落盘（成功写 tests/metrics，失败写 ErrorLog/） | 推理与检测模块：<br>- `src/cpp/include/YoloFaceDetector.h`<br>- `src/cpp/include/ArcFaceEmbedder.h`<br><br>闭环管线与 JSON：<br>- `src/cpp/include/FaceInferencePipeline.h`<br>- `src/cpp/src/FaceInferencePipeline.cpp`<br><br>检索与阈值：<br>- `src/cpp/include/FaceSearch.h`<br>- `src/cpp/include/ThresholdPolicy.h`<br><br>离线验证与基线：<br>- `src/cpp/main.cpp`（`--yolo-face` / `--face-infer` / `--face-baseline`）<br>- `src/cpp/tools/inference_bench_cli.cpp` |

#### 4.3.1 RKNN（仅对“带 NPU 的瑞芯微设备”可选，不适用于目标 RK3288 工控机）
如后续迁移到“带 NPU 的瑞芯微平台”，可参考 `docs/examples/` 中的 RKNN 示例（仅示例，不作为本项目目标设备的默认链路）：
- `docs/examples/03_rknn_inference.cpp`
- `docs/examples/05_opencv_rknn_bridge.cpp`

### 4.4 实时 HDMI 输出 (DRM/KMS 双缓冲)
绕过 Android SurfaceFlinger，直接向显存写入数据，实现极低延迟显示。

<a id="tbl-4-4"></a>
#### 表 4-4 DRM/KMS 双缓冲显示流程（伪代码与示例）
| 伪代码 (Pseudocode) | 示例代码 (Example) |
| :--- | :--- |
| 1. 打开 DRM 卡 `open("/dev/dri/card0")`<br>2. 获取资源 `drmModeGetResources()`<br>3. 找到 Connector (HDMI) 和 CRTC<br>4. 创建两个 Dumb Buffer (双缓冲)<br>5. 循环：<br>   - 绘制到后台 Buffer<br>   - 翻页 `drmModePageFlip()`<br>   - 等待 VSync 事件 | [04_drm_kms_display.cpp](docs/examples/04_drm_kms_display.cpp)<br><br>关键点：<br>- 双缓冲是防止画面撕裂的关键。<br>- `PageFlip` 是非阻塞的，需配合事件循环。 |

### 4.5 混合架构：CameraX 预览 + Native 分析
结合 Android CameraX 的易用性与 Native C++ 的高性能，实现零拷贝预览与高效算法处理。

<a id="tbl-4-5"></a>
#### 表 4-5 CameraX + Native 分析流程（伪代码与示例）
| 伪代码 (Pseudocode) | 示例代码 (Example) |
| :--- | :--- |
| **Java 层 (CameraX)**:<br>1. 创建 `ProcessCameraProvider`<br>2. 绑定 `Preview` 到 `PreviewView` (GPU 直接渲染)<br>3. 绑定 `ImageAnalysis` 获取 YUV 数据<br>4. 通过 JNI 传递 `ImageProxy` 的 Plane Buffer<br><br>**Native 层 (C++)**:<br>1. JNI 接收 `ByteBuffer` 地址<br>2. 零拷贝封装为 `cv::Mat` (引用模式)<br>3. 执行 OpenCV + ncnn/OpenCV DNN 推理（YOLO/ArcFace） | [07_camerax_jni_bridge.java](docs/examples/07_camerax_jni_bridge.java)<br>[07_jni_yuv_processor.cpp](docs/examples/07_jni_yuv_processor.cpp)<br><br>关键点：<br>- 预览流不经过 CPU，极大降低负载。<br>- `ImageAnalysis` 支持背压策略，避免算法阻塞预览。<br>- YUV 数据传递需注意 Stride 对齐。 |

### 4.6 设备基础信息搜集 (Device Profiling)
**状态**: [待后续执行] (Pending Implementation)

该模块负责在应用启动时收集设备的基础硬件与系统信息，用于动态调整运行策略（如针对低内存设备降级画质）。

#### 4.6.1 核心数据结构
*   **DeviceProfile**: 存储不可变的设备指纹信息。
    *   `manufacturer`, `model`, `board` (Build.*)
    *   `totalMemBytes` (内存大小)
    *   `dataTotalBytes` (存储大小)
*   **DeviceClass**: 设备分类枚举。
    *   `INDUSTRIAL_RK3288`: 工控机 (目标设备)
    *   `CN_OPPO`/`CN_VIVO` etc.: 消费级手机 (用于兼容性测试)

#### 4.6.2 实现逻辑 (伪代码)
```java
// 1. 收集原始信息
DeviceProfile profile = DeviceProfile.collect(context);

// 2. 执行分类策略
DeviceClass cls = DeviceClassifier.classify(profile);

// 3. 初始化全局运行时
DeviceRuntime.get().init(profile, cls);

// 4. 应用策略
if (cls == INDUSTRIAL_RK3288) {
    // 启用高性能模式，禁用非必要后台服务
}
```

> **注意**: 已移除所有第三方厂商 SDK (ColorOS, GMS, Play Integrity) 的依赖，仅保留纯 Android API 实现，确保代码轻量且无侵入性。

---

## 5. 第一部分：Android APP 摄像头调用机制研究

本节目标是提供一套可执行、可复现、可审计的 Android 摄像头集成方案研究结论与落地模板，覆盖选型、生命周期、权限、能力枚举、时序、异常处理与基准测试。

### 5.1 API/库选型对比（Camera1/Camera2/CameraX/第三方封装）

<a id="tbl-5-1"></a>
#### 表 5-1 摄像头 API/库对比表（结论导向）
| 维度 | Camera1（android.hardware.Camera） | Camera2（android.hardware.camera2） | CameraX（androidx.camera.*） | 第三方封装（抽象层） |
| :--- | :--- | :--- | :--- | :--- |
| 维护状态 | 已废弃，不建议新项目使用 | 官方主流底层 API | 官方 Jetpack 封装，推荐 | 依赖社区维护质量 |
| 兼容性 | 老设备覆盖广 | 受设备 HAL 与硬件级别影响 | 由 CameraX 处理适配差异 | 取决于封装策略 |
| 复杂度 | 低 | 高（线程、Session、Surface、状态机） | 中（UseCase 组合） | 低到中（但隐藏细节） |
| 预览/分析 | 自行处理 | 精细可控，支持多输出 | Preview + ImageAnalysis 常用组合 | 需确认是否支持零拷贝/背压 |
| 推荐场景 | 仅维护遗留代码 | 工业相机、强定制、必须控底层 | APP 常规预览 + 识别分析 | 快速 Demo，但需评估风险 |

#### 5.1.1 第三方封装库对比（工程审计视角）
第三方封装常见诉求是“少写代码”，但在工业设备/门禁类场景里，风险主要来自：版本漂移、权限/生命周期缺陷、对底层异常（CameraService 重启/设备断连）处理不足、以及对 CameraX/Camera2 行为的二次封装导致排障困难。推荐优先以 CameraX 为基线，自研一层极薄的业务适配层。

<a id="tbl-5-2"></a>
#### 表 5-2 常见第三方封装库对比（不引入依赖，仅用于选型）
| 方案 | 典型形态 | 优点 | 主要风险点 | 适配建议 |
| :--- | :--- | :--- | :--- | :--- |
| CameraX + 自研薄封装 | 业务只暴露“打开/关闭/拍照/录像/帧回调” | 官方维护、兼容性最好、可控 | 需要理解 UseCase 与生命周期 | 推荐基线方案 |
| CameraView 类库（社区） | 统一 API，内部接 Camera1/2 | 上手快、Demo 速度快 | 兼容性/维护不可控；错误处理与后台限制经常不全 | 仅用于快速原型，不建议量产 |
| OpenCV VideoCapture（Android） | 以 OpenCV API 调用相机 | 算法侧代码复用 | Android 侧能力与时序不透明；与 CameraX 同时使用易冲突 | 不建议作为 Android 主摄像头栈 |
| WebRTC Camera Capturer | 面向实时音视频 | 端到端链路成熟 | 与门禁业务（拍照/本地存储/离线识别）契合度低；依赖重 | 仅在 RTC 场景使用 |
| 厂商 SDK（相机/美颜/AI） | 私有 API/二进制依赖 | 功能“开箱即用” | 许可/审计风险、升级受限、适配面窄 | 除非强需求，否则禁止引入 |

#### 5.1.2 Fotoapparat / CAMKit（CameraKit）对比（不引入依赖，仅用于选型）
两者都属于“用更少代码封装相机细节”的路线，适合快速 Demo；但在门禁/工控场景，更重要的是生命周期、异常恢复与可观测性，而这恰恰是第三方封装最容易成为黑箱的部分。

<a id="tbl-5-3"></a>
#### 表 5-3 Fotoapparat 与 CAMKit 对比（工程审计口径）
| 维度 | Fotoapparat | CAMKit（CameraKit） | 落地建议 |
| :--- | :--- | :--- | :--- |
| 维护状态 | 社区项目，维护活跃度通常较低，需自行评估近一年提交/Issue | 同类封装库，维护质量差异大（存在多个同名/相近项目） | 以“可持续维护”为硬门槛：无稳定维护与发布节奏则不进入量产链路 |
| 底层依赖 | 多为 Camera1/Camera2 统一封装 | 多为 Camera1/Camera2 统一封装 | 本项目优先以 CameraX 为基线；若必须走 Camera2，建议直接用 Camera2 + 自研薄封装 |
| 预览/分析链路 | 以回调/帧处理接口封装为主，零拷贝与背压策略不一定透明 | 同类问题：帧回调的线程/队列语义可能不清晰 | 人脸识别必须明确：帧率、背压策略、线程模型、`ImageProxy.close()` 等“硬口径” |
| 异常恢复能力 | CameraService 重启、设备断连、权限回流等场景支持程度不一致 | 同上，且封装层可能吞掉底层错误细节 | 必须能暴露原始错误码/状态机，并提供可审计的重绑策略（见 5.7） |
| 可观测性 | 需要自行补齐日志与指标埋点 | 同上 | 不允许“黑箱”：至少输出 TTFF、帧率、丢帧、重启次数、错误码 |
| 适配风险（RK3288） | 老设备/HAL 碎片化下，封装层更易踩坑 | 同上 | 工控量产优先“少魔法、可控、可定位” |
| 推荐度（本项目） | 仅用于快速原型验证 | 仅用于快速原型验证 | 量产链路建议：CameraX + 自研薄封装（表 5-2 第 1 行） |

### 5.2 生命周期与调用模式（主动调用 vs 系统回调）

#### 5.2.1 主动调用（应用驱动）
- 典型代表：Camera2/CameraX 的打开与绑定由应用在 UI 生命周期（onStart/onResume）触发。
- 优点：应用可定义清晰的资源边界与性能策略（背压、线程池、分辨率/FPS 约束）。
- 风险：生命周期处理不当易造成句柄泄漏、后台持有摄像头导致系统强杀或权限异常。

#### 5.2.2 系统回调（系统驱动）
- 典型代表：系统通过回调推送帧数据（SurfaceTexture/Surface/Camera2 ImageReader）。
- 优点：对“持续流”天然友好，延迟可控。
- 风险：回调线程与队列拥塞会放大抖动，导致帧丢失与 ANR 风险。

<a id="fig-5-1"></a>
#### 图 5-1 CameraX 生命周期与资源边界（简化时序）
```mermaid
sequenceDiagram
    autonumber
    participant UI as Activity/Fragment
    participant PCP as ProcessCameraProvider
    participant Cx as CameraX UseCases
    participant CS as CameraService
    UI->>PCP: getInstance()
    UI->>PCP: bindToLifecycle(Preview, ImageAnalysis)
    PCP->>CS: openCamera()
    CS-->>PCP: onOpened()
    PCP->>Cx: startRepeating()
    UI->>PCP: unbindAll()（onStop/onDestroy）
    PCP->>CS: closeCamera()
    CS-->>PCP: onClosed()
```

### 5.3 摄像头枚举与能力查询：类型识别、输出格式与硬件级别判定

#### 5.3.1 设备枚举与前后摄像头识别
Camera2 使用 `CameraManager.getCameraIdList()` 枚举摄像头，并通过 `CameraCharacteristics` 判定类型与能力。

```kotlin
import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager

data class CameraInfo(
    val cameraId: String,
    val facing: Int?,
    val hardwareLevel: Int?,
    val availableFpsRanges: List<String>
)

fun enumerateCameras(context: Context): List<CameraInfo> {
    val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    return manager.cameraIdList.map { id ->
        val ch = manager.getCameraCharacteristics(id)
        val fps = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.map { "${it.lower}-${it.upper}" }
            .orEmpty()
        CameraInfo(
            cameraId = id,
            facing = ch.get(CameraCharacteristics.LENS_FACING),
            hardwareLevel = ch.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL),
            availableFpsRanges = fps
        )
    }
}
```

#### 5.3.2 硬件级别判定（LEGACY/FULL/LEVEL_3）
- `INFO_SUPPORTED_HARDWARE_LEVEL` 是快速分层入口，但不等价于“绝对可用”；仍需按需求检查关键能力（例如：并行输出、YUV 支持、手动曝光等）。
- 工程落地建议：将“必须能力”抽象为检查函数，启动时产出一份设备能力报告（写入 `ErrorLog/` 或测试报告目录）。

#### 5.3.3 相机类型枚举（面向业务）与可用能力查询（面向工程）
Camera2 的 `cameraId` 只是“设备节点标识”，业务需要的是“这是哪类相机、能不能完成我需要的输出”。推荐将 Camera2/CameraX 的字段归一为业务枚举，并为每个 `cameraId` 生成一份能力报告（Capability Report）。

业务枚举建议（稳定、可审计）：
- 面向“朝向”的枚举：`FRONT / BACK / EXTERNAL / UNKNOWN`（映射 `LENS_FACING`）。
- 面向“能力”的标记：例如是否支持 `DEPTH_OUTPUT`、是否为 `LOGICAL_MULTI_CAMERA`、是否支持 `RAW`、是否支持高帧率范围等（映射 `REQUEST_AVAILABLE_CAPABILITIES` 与 `SCALER_STREAM_CONFIGURATION_MAP`）。

```kotlin
import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.util.Size

enum class CameraFacingType { FRONT, BACK, EXTERNAL, UNKNOWN }

data class CameraCapabilityReport(
    val cameraId: String,
    val facingType: CameraFacingType,
    val hardwareLevel: Int?,
    val isLogicalMultiCamera: Boolean,
    val physicalCameraIds: Set<String>,
    val supportsDepth: Boolean,
    val supportsRaw: Boolean,
    val yuvSizes: List<Size>,
    val jpegSizes: List<Size>,
    val fpsRanges: List<String>
)

fun buildCameraCapabilityReport(context: Context): List<CameraCapabilityReport> {
    val manager = context.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    return manager.cameraIdList.map { id ->
        val ch = manager.getCameraCharacteristics(id)
        val facing = when (ch.get(CameraCharacteristics.LENS_FACING)) {
            CameraCharacteristics.LENS_FACING_FRONT -> CameraFacingType.FRONT
            CameraCharacteristics.LENS_FACING_BACK -> CameraFacingType.BACK
            CameraCharacteristics.LENS_FACING_EXTERNAL -> CameraFacingType.EXTERNAL
            else -> CameraFacingType.UNKNOWN
        }
        val caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.toSet().orEmpty()
        val isLogical = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA)
        val supportsDepth = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT)
        val supportsRaw = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_RAW)
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
        val yuv = map?.getOutputSizes(android.graphics.ImageFormat.YUV_420_888)?.toList().orEmpty()
        val jpeg = map?.getOutputSizes(android.graphics.ImageFormat.JPEG)?.toList().orEmpty()
        val fps = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.map { "${it.lower}-${it.upper}" }
            .orEmpty()
        CameraCapabilityReport(
            cameraId = id,
            facingType = facing,
            hardwareLevel = ch.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL),
            isLogicalMultiCamera = isLogical,
            physicalCameraIds = ch.physicalCameraIds,
            supportsDepth = supportsDepth,
            supportsRaw = supportsRaw,
            yuvSizes = yuv,
            jpegSizes = jpeg,
            fpsRanges = fps
        )
    }
}
```

能力报告的“最低交付口径”（建议写入日志或导出为 JSON/CSV）：
- 必要输出：是否支持 `YUV_420_888`（用于识别）、是否支持 `JPEG`（用于抓拍取证）。
- 必要尺寸：能否提供目标分辨率（例如 1280×720）在 YUV/JPEG 两条链路都可用。
- 必要帧率：是否存在可接受的 FPS Range（例如 15-30）。
- 异常自检：若 `hardwareLevel=LEGACY` 或缺失关键尺寸/格式，直接降级方案或提示不支持。

<a id="tbl-5-6"></a>
#### 表 5-6 广角/长焦/TOF/红外：枚举方法与启发式判定（工程口径）
| 目标类型 | Camera2 可用信号 | 推荐判定方法（可审计） | 典型陷阱与降级 |
| :--- | :--- | :--- | :--- |
| 广角/超广角（Wide/UltraWide） | `LENS_INFO_AVAILABLE_FOCAL_LENGTHS`（焦距数组），`SCALER_STREAM_CONFIGURATION_MAP`（输出尺寸），`physicalCameraIds`（逻辑多摄） | 若是逻辑多摄：读取所有 `physicalCameraId` 的焦距，按“最短焦距”标为超广角/广角候选；同一 `cameraId` 内可通过焦距与输出能力形成可解释排序 | 焦距单位为 mm，跨机型阈值不可写死；单摄机型无法可靠区分“广角 vs 普通” |
| 长焦（Tele） | 同上（焦距/物理相机） + `CONTROL_ZOOM_RATIO_RANGE`（若支持） | 逻辑多摄下：把“最长焦距”的 physical camera 标为长焦候选；若无物理相机，使用 `CONTROL_ZOOM_RATIO_RANGE` 仅能表示“可变焦”，不能等价于“长焦模组” | 数码变焦不等于长焦；有的设备把长焦暴露为独立 cameraId，有的只通过 logical camera 聚合 |
| TOF 深度（Depth/ToF） | `REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT`，`SCALER_STREAM_CONFIGURATION_MAP` 是否支持 `DEPTH16`/`DEPTH_POINT_CLOUD` | 同时满足：capabilities 含 DEPTH_OUTPUT 且 depth 输出格式可用，则判定为“深度相机/TOF 链路可用”；在报告里记录可用的 depth 分辨率与帧率范围 | 部分设备只提供“深度作为辅助”但不暴露标准 depth 输出；需厂商 tag 才能完全判断 |
| 红外（IR）/黑白（Mono） | `SENSOR_INFO_COLOR_FILTER_ARRANGEMENT`（是否 MONO），capabilities（DEPTH/LOGICAL），输出格式与分辨率 | 仅用标准字段无法稳定判定“红外补光/IR 摄像头”；建议做两级口径：一级以 `MONO` 标记“灰度/单色传感器候选”；二级由设备白名单/厂商 tag/实测（低照度下响应）确认 IR | IR 常依赖厂商私有 tag；不同模组（IR flood/IR camera/ToF）暴露方式差异极大，必须保留 UNKNOWN 与人工标注通道 |

#### 5.3.4 相机“角色枚举”（广角/长焦/TOF/红外）与可解释分类函数
工程上不建议把“广角/长焦/红外”写死成固定 cameraId。推荐将识别逻辑做成“可解释的启发式 + 可覆盖的配置层”：
- 启发式：用 Camera2 标准字段给出候选类型与理由（焦距/深度输出/单色传感器等）。
- 覆盖层：允许用远端配置/本地白名单对特定 `device_id + cameraId` 强制指定角色（用于量产落地与问题回滚）。

```kotlin
import android.hardware.camera2.CameraCharacteristics
import android.graphics.ImageFormat

enum class CameraRole {
    RGB,
    WIDE,
    ULTRA_WIDE,
    TELEPHOTO,
    DEPTH_TOF,
    INFRARED,
    UNKNOWN
}

data class CameraRoleHint(
    val role: CameraRole,
    val evidence: List<String>
)

fun classifyCameraRoleHint(ch: CameraCharacteristics): CameraRoleHint {
    val evidence = mutableListOf<String>()
    val caps = ch.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)?.toSet().orEmpty()
    val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)

    val hasDepthCap = caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_DEPTH_OUTPUT)
    if (hasDepthCap) evidence += "capabilities:DEPTH_OUTPUT"

    val depth16 = map?.getOutputSizes(ImageFormat.DEPTH16)?.isNotEmpty() == true
    val depthPointCloud = map?.getOutputSizes(ImageFormat.DEPTH_POINT_CLOUD)?.isNotEmpty() == true
    if (depth16) evidence += "format:DEPTH16"
    if (depthPointCloud) evidence += "format:DEPTH_POINT_CLOUD"
    if (hasDepthCap && (depth16 || depthPointCloud)) {
        return CameraRoleHint(CameraRole.DEPTH_TOF, evidence)
    }

    val cfa = ch.get(CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT)
    val isMono = cfa == CameraCharacteristics.SENSOR_INFO_COLOR_FILTER_ARRANGEMENT_MONO
    if (isMono) {
        evidence += "sensor:CFA_MONO"
        return CameraRoleHint(CameraRole.INFRARED, evidence)
    }

    val focal = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.toList().orEmpty()
    if (focal.isNotEmpty()) evidence += "focal_mm:${focal.joinToString(",")}"

    val minF = focal.minOrNull()
    val maxF = focal.maxOrNull()
    if (minF != null && maxF != null && maxF > minF * 1.8f) {
        evidence += "focal_span:tele_candidate"
        return CameraRoleHint(CameraRole.TELEPHOTO, evidence)
    }
    if (minF != null && maxF != null && minF < maxF / 1.3f) {
        evidence += "focal_span:wide_candidate"
        return CameraRoleHint(CameraRole.WIDE, evidence)
    }

    return CameraRoleHint(CameraRole.RGB, evidence.ifEmpty { listOf("default:RGB") })
}
```

<a id="tbl-5-7"></a>
#### 表 5-7 常用“能力查询”字段速查（闪光/对焦/曝光补偿等）
| 能力项 | Camera2 字段（CameraCharacteristics / CaptureRequest / CaptureResult） | 判定/读取口径 | 备注 |
| :--- | :--- | :--- | :--- |
| 闪光灯可用 | `FLASH_INFO_AVAILABLE` | `true` 表示设备具备闪光灯硬件 | 具备不等于“所有模式都可用”，仍需按场景处理异常 |
| 自动对焦模式 | `CONTROL_AF_AVAILABLE_MODES` | 列表包含 `CONTROL_AF_MODE_CONTINUOUS_PICTURE` 等即表示可用 | 仅“可用”不代表效果，需实测对焦速度与稳定性 |
| 近焦能力（是否能对近距离对焦） | `LENS_INFO_MINIMUM_FOCUS_DISTANCE` | `null` 或 `0f` 通常表示固定焦/不可调焦；>0 表示支持对焦驱动 | 厂商实现差异大，作为“启发式”记录即可 |
| 曝光补偿支持 | `CONTROL_AE_COMPENSATION_RANGE` + `CONTROL_AE_COMPENSATION_STEP` | range 非空且 step > 0 表示支持；配置时必须落在 range 内 | 业务应输出“可配置的 EV 刻度”和当前设置值 |
| 自动曝光模式 | `CONTROL_AE_AVAILABLE_MODES` | 是否包含 `CONTROL_AE_MODE_ON`/`ON_AUTO_FLASH` 等 | 实际可用还受 `FLASH_INFO_AVAILABLE` 影响 |
| 自动白平衡模式 | `CONTROL_AWB_AVAILABLE_MODES` | 是否包含 `CONTROL_AWB_MODE_AUTO` 等 | RK3288/老 HAL 上可能只有 AUTO |
| OIS（防抖） | `LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION` | 列表包含 `ON` 即可记录为“可能支持” | OIS 对识别清晰度有帮助，但不应强依赖 |
| 变焦范围（逻辑） | `CONTROL_ZOOM_RATIO_RANGE` | 若存在则记录最小/最大 zoom ratio | 变焦范围不等于“长焦模组存在” |

#### 5.3.5 可审计“能力报告”扩展字段（建议落地为 JSON）
在 5.3.3 的能力报告基础上，建议增加以下字段，直接服务于门禁/识别类场景的工程决策：
- 闪光：`FLASH_INFO_AVAILABLE`，以及 AE 模式是否含 `ON_AUTO_FLASH`/`ON_ALWAYS_FLASH`。
- 对焦：`CONTROL_AF_AVAILABLE_MODES`，以及 `LENS_INFO_MINIMUM_FOCUS_DISTANCE`（启发式）。
- 曝光补偿：`CONTROL_AE_COMPENSATION_RANGE/STEP`，并输出“可配置 EV 刻度表”。
- 深度：`DEPTH_OUTPUT` 能力 + depth 输出格式与尺寸列表（用于 TOF/深度链路判定）。
- 多摄：是否 logical multi camera，`physicalCameraIds`，每个 physical camera 的焦距与关键能力快照。

### 5.4 运行时权限：Manifest 声明 + 动态申请 + 拒绝/永久拒绝处理模板

#### 5.4.1 Manifest 最小声明模板
```xml
<manifest>
    <uses-feature android:name="android.hardware.camera" android:required="false" />
    <uses-permission android:name="android.permission.CAMERA" />
</manifest>
```

#### 5.4.2 运行时权限处理模板（拒绝/永久拒绝/设置页返回）
```kotlin
import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.provider.Settings
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.fragment.app.Fragment

class CameraPermissionGate(
    private val fragment: Fragment,
    private val onGranted: () -> Unit,
    private val onDenied: () -> Unit,
    private val onPermanentlyDenied: () -> Unit
) {
    private val launcher = fragment.registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            onGranted()
            return@registerForActivityResult
        }
        val showRationale = fragment.shouldShowRequestPermissionRationale(Manifest.permission.CAMERA)
        if (showRationale) {
            onDenied()
        } else {
            onPermanentlyDenied()
        }
    }

    fun request() {
        val ctx = fragment.requireContext()
        val granted = ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED
        if (granted) {
            onGranted()
            return
        }
        launcher.launch(Manifest.permission.CAMERA)
    }

    fun openAppSettings() {
        val ctx = fragment.requireContext()
        val intent = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS).apply {
            data = Uri.parse("package:${ctx.packageName}")
        }
        fragment.startActivity(intent)
    }
}
```

#### 5.4.3 Android 13+ 权限变化与后台限制（摄像头/录像必读）
本节只描述与“拍照/录像/门禁常态运行”强相关的变化点，目标是把“线上崩溃/不可用”转成“可预期的降级与提示”。

<a id="tbl-5-4"></a>
#### 表 5-4 Android 13+ 权限与后台限制对照（工程落地）
| 场景 | 关键权限/声明 | Android 13+ 变化点 | 推荐策略 |
| :--- | :--- | :--- | :--- |
| 打开相机预览/识别 | `android.permission.CAMERA` | 仍为运行时权限 | 权限 Gate + 生命周期 onStop 强制释放 |
| 录像带音频 | `android.permission.RECORD_AUDIO` | 仍为运行时权限 | 仅在用户开启“带声录像”时申请 |
| 保存到系统相册/共享目录 | MediaStore 写入（通常不需存储权限） | `READ_MEDIA_*` 替代旧的读取权限 | 写入优先 MediaStore；读取按需申请 `READ_MEDIA_IMAGES/VIDEO` |
| 发送“运行中通知” | `android.permission.POST_NOTIFICATIONS` | Android 13 起为运行时权限 | 常态运行（前台服务）必须确保通知可发，否则降级为短任务 |
| 长时间后台录像/采集 | 前台服务（FGS）+ 通知 | Android 13+ 对后台启动组件更严格；Android 14 起 FGS 类型更严格 | 只在用户可感知时运行；用前台服务承载长任务；必要时引导到设置 |

前台服务（录像/持续采集）声明模板（面向 Android 14+ 兼容，Android 13 也建议提前就位）：
```xml
<manifest>
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />
    <uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_CAMERA" />
    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_MICROPHONE" />

    <application>
        <service
            android:name=".camera.CameraForegroundService"
            android:exported="false"
            android:foregroundServiceType="camera|microphone" />
    </application>
</manifest>
```

后台限制落地要点（避免“后台持有摄像头”导致不可预期异常）：
- 资源边界：`onStop`/`onDestroy` 必须释放 UseCase/Session，禁止后台持有 CameraDevice。
- 录像策略：需要长时录像时，必须切到前台服务并展示持续通知；否则仅允许“短录像”并在退后台立即停止。
- 恢复策略：从设置页返回/从后台回前台，重新走“权限检查 → 重新绑定 → 重新测首帧”流程，禁止复用旧句柄。

### 5.5 打开 → 预览配置 → 拍照/录像 → 回调 → 释放：完整时序与常见泄漏点

<a id="fig-5-2"></a>
#### 图 5-2 CameraX 打开-预览-分析-释放（关键节点）
```mermaid
sequenceDiagram
    autonumber
    participant UI as UI
    participant C as CameraX
    participant P as Preview
    participant A as ImageAnalysis
    participant N as Native（JNI）
    participant S as CameraService
    UI->>C: bindToLifecycle(P, A)
    C->>S: openCamera()
    S-->>C: opened
    C->>P: setSurfaceProvider(PreviewView)
    C->>A: setAnalyzer(executor)
    A-->>N: onFrame(ImageProxy)
    UI->>C: unbindAll()/onStop
    C->>A: clearAnalyzer()
    C->>S: closeCamera()
```

#### 5.5.1 常见泄漏点与风险清单（摄像头侧）
- Analyzer 未清理：`ImageAnalysis.clearAnalyzer()` 或解绑时未关闭后台线程，导致持有 `ImageProxy` 引用。
- `ImageProxy.close()` 未调用：帧缓冲被占用，造成背压阻塞与内存上涨。
- 后台持有摄像头：Android 10+ 后台限制可能触发系统回收与 CameraService 异常，需在 onStop 及时释放。

<a id="fig-5-3"></a>
#### 图 5-3 CameraX 拍照（ImageCapture）完整时序（建议口径）
```mermaid
sequenceDiagram
    autonumber
    participant UI as UI
    participant C as CameraX
    participant IC as ImageCapture
    participant CS as CameraService
    participant IO as MediaStore/文件
    UI->>C: bindToLifecycle(Preview, ImageAnalysis, ImageCapture)
    UI->>IC: takePicture(OutputFileOptions)
    IC->>CS: captureStillPicture()
    CS-->>IC: onCaptureCompleted()
    IC->>IO: 写入 JPEG/元数据
    IC-->>UI: onImageSaved()/onError()
```

<a id="fig-5-4"></a>
#### 图 5-4 CameraX 录像（VideoCapture + Recorder）完整时序（建议口径）
```mermaid
sequenceDiagram
    autonumber
    participant UI as UI
    participant C as CameraX
    participant VC as VideoCapture/Recorder
    participant CS as CameraService
    participant IO as MediaStore/文件
    UI->>C: bindToLifecycle(Preview, VideoCapture)
    UI->>VC: prepareRecording(OutputOptions)
    UI->>VC: start()
    VC->>CS: startRepeating(录像管线)
    VC-->>UI: onStart()
    UI->>VC: stop()
    VC->>CS: stopRepeating()
    VC->>IO: 写入 mp4/元数据
    VC-->>UI: onFinalize()
```

#### 5.5.2 输出格式与容器约束（拍照/录像）
<a id="tbl-5-5"></a>
#### 表 5-5 常见拍照/录像输出格式（工程约束与选择）
| 输出链路 | 常见格式 | 典型用途 | 关键注意事项 |
| :--- | :--- | :--- | :--- |
| 预览（Preview） | `PRIVATE`（SurfaceTexture） | UI 显示 | 不是给算法用的像素格式 |
| 分析（ImageAnalysis） | `YUV_420_888` | 人脸检测/特征提取 | 必须 `ImageProxy.close()`；注意 stride/pixelStride |
| 拍照（ImageCapture） | `JPEG`（或 YUV->JPEG） | 抓拍取证/注册照 | JPEG 写入走 MediaStore 更稳；避免主线程 IO |
| 录像（VideoCapture） | H.264/HEVC + AAC（容器多为 mp4） | 事件录像 | 带音频需 `RECORD_AUDIO`；长时录像需前台服务 |

格式选择建议（门禁/识别场景）：
- 识别主链路固定为 `ImageAnalysis(YUV_420_888)`，避免从预览帧做截图。
- 抓拍用 `ImageCapture(JPEG)`，并把抓拍与识别解耦，避免抓拍阻塞分析线程。

### 5.6 性能基准与可复现测试方案（首帧/连拍/后台切换）

#### 5.6.1 指标定义
- 冷启动首帧（TTFF）：从“触发打开摄像头”到“第一帧可见/可分析”的耗时（ms）。
- 连续拍照稳定性：30 次连续拍照无 OOM、无崩溃、无明显泄漏增长。
- 前后台切换稳定性：50 次切换 CameraService 重启次数为 0（可用 logcat 关键词统计）。

#### 5.6.2 通过标准（可直接作为验收口径）
- TTFF：P50 < 600ms，P95 < 900ms（目标设备：RK3288 工控机 + 指定摄像头模组）。
- 连拍：30/30 成功，内存峰值不超过基线 + 120MB。
- 切换：50 次切换后仍可恢复预览与分析；CameraService 重启次数 = 0。

#### 5.6.3 ADB 基准脚本模板（不硬编码路径）
- Windows：`scripts/bench_camera_adb.ps1`
- Linux/macOS：`scripts/bench_camera_adb.sh`

脚本默认按“日志标记”采集指标，应用需输出如下日志格式（Tag/键名可统一但必须稳定）：
- `BENCH_CAMERA TTFF_MS=<number>`
- `BENCH_CAMERA CAPTURE_OK=<number> CAPTURE_FAIL=<number>`
- `BENCH_CAMERA CAMERA_SERVICE_RESTART=<number>`

### 5.7 CameraService 重启/崩溃检测：可观测信号、计数口径与自恢复
CameraService 重启通常表现为：正在预览/分析时突然黑屏、回调停止、随后出现 `ERROR_CAMERA_SERVICE` 或 CameraX `CameraState` 报错。工程上需要两件事：一是“明确计数口径”，二是“可自动恢复且不无限重试”。

#### 5.7.1 可观测信号（优先级从高到低）
- Camera2：`CameraDevice.StateCallback.onError(ERROR_CAMERA_SERVICE)` / `onDisconnected()`。
- CameraX：监听 `cameraInfo.cameraState`，当状态进入 `ERROR` 且 `error.code` 指向服务异常时计数。
- 系统侧（验收/排障）：`adb logcat` 关键词（示例：`cameraserver`、`CameraService`、`restarting`）与 `adb shell dumpsys media.camera`。

#### 5.7.2 应用侧计数与自恢复策略（建议模板）
恢复策略建议：释放全部 UseCase → 延迟重绑（指数退避）→ 达到阈值后进入“需要人工干预”状态（提示重启应用/检查摄像头占用/检查权限）。

```kotlin
import android.os.SystemClock
import androidx.camera.core.CameraState
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.Observer
import java.util.concurrent.atomic.AtomicInteger

class CameraServiceRestartMonitor(
    private val lifecycleOwner: LifecycleOwner,
    private val maxRetries: Int = 3
) {
    private val restartCount = AtomicInteger(0)
    private var lastErrorAtMs: Long = 0L

    fun attach(cameraStateLiveData: androidx.lifecycle.LiveData<CameraState>, onRecover: (attempt: Int) -> Unit, onGiveUp: () -> Unit) {
        cameraStateLiveData.observe(lifecycleOwner, Observer { state ->
            val err = state.error ?: return@Observer
            val isFatal = err.code == CameraState.ERROR_CAMERA_FATAL_ERROR
            if (!isFatal) return@Observer
            val now = SystemClock.elapsedRealtime()
            if (now - lastErrorAtMs < 1_000) return@Observer
            lastErrorAtMs = now
            val attempt = restartCount.incrementAndGet()
            if (attempt <= maxRetries) {
                onRecover(attempt)
            } else {
                onGiveUp()
            }
        })
    }
}
```

计数口径建议：
- 每次触发“服务异常导致的重绑”时输出：`BENCH_CAMERA CAMERA_SERVICE_RESTART=+1 ATTEMPT=<n>`.
- 在基准脚本统计时，只统计“重绑开始”的次数，避免重复计数（如同一次异常导致多处回调）。

---

## 6. 第二部分：人脸识别技术实现方案研究

本节提供人脸识别方案的工程化对比与落地模板，强调“口径一致”“可复现评估”“合规优先”“可维护集成”。

### 6.1 端侧离线 vs 云端：原理、模板、阈值口径、延迟与隐私差异

<a id="tbl-6-1"></a>
#### 表 6-1 离线端侧与云端方案对比表（工程视角）
| 维度 | 离线端侧（设备本地） | 云端（服务端 API） |
| :--- | :--- | :--- |
| 网络依赖 | 无 | 强依赖 |
| 延迟组成 | 预处理 + 推理 + 1:N 检索 | 预处理 + 上传 + 服务推理 + 返回 |
| 数据合规 | 生物特征不出端更易满足最小化原则 | 需额外合规审查与传输/存储说明 |
| 成本 | 设备算力与本地存储成本 | API 调用成本与带宽成本 |
| 可控性 | 高（阈值、版本、回滚） | 中（受服务更新影响） |

#### 6.1.1 常见端侧/云端方案快速对比（结论导向）
本表强调“能否在 RK3288 工控量产场景可控落地”，不追求覆盖全部能力项；最终仍需以本项目基准口径（6.3/6.4）做实测裁决。

<a id="tbl-6-6"></a>
#### 表 6-6 人脸方案对比（ML Kit / MediaPipe / ArcFace / Dlib / 百度 / 优图）
| 方案 | 类型 | 典型输出能力 | 集成复杂度 | RK3288 风险点 | 成本/许可 | 推荐落地形态 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Google ML Kit（Face Detection） | 端侧 | 人脸框/关键点/追踪（偏检测） | 低到中 | 算子与机型差异需实测；包体与性能需评估 | 免费（以官方条款为准） | 用于“检测/质量分/关键点”；识别（特征）建议自研/自带模型 |
| MediaPipe（Face Detector / Face Landmarker） | 端侧 | 人脸框/关键点/FaceMesh（偏几何） | 中 | 构建体系与依赖版本需固定；部分方案对 GPU/NNAPI 依赖需评估 | 免费（以官方条款为准） | 用于“关键点/对齐/质量评估”；识别向量建议独立模型 |
| ArcFace（算法/论文；常见开源实现为 InsightFace 等） | 端侧 | 以“特征提取（Embedding）+ 余弦相似度”为核心的识别链路 | 中 | 需要自行补齐检测/对齐/存储/检索与阈值口径；模型转换与端侧加速需评估 | 开源（以具体实现许可证为准） | 推荐“自研可控链路”：检测/对齐可独立，特征模型固定并版本化 |
| 虹软 ArcFace（商业 SDK/商标产品） | 端侧 | 检测 + 特征提取 + 1:1/1:N（取决于 SDK 形态） | 中到高 | 许可证绑定、ABI/so 兼容、离线激活与设备更换流程 | 商业授权 | 适合量产：需把激活/版本/阈值/回滚做成可审计闭环 |
| Dlib | 端侧 | HOG/CNN 检测 + 128D 特征（经典方案） | 高 | NDK 编译复杂、体积大、性能/NEON 优化不确定 | 开源许可（以官方为准） | 更适合研究/验证；量产不推荐作为主链路 |
| 百度 AI 人脸（Face） | 云端 | 检测/对比/检索/活体（按产品线） | 中 | 网络抖动与延迟；密钥管理；服务变更 | 按量计费 | 推荐“自建后端转发 + 端侧最小上传 + 全链路审计” |
| 腾讯优图/腾讯云人脸 | 云端 | 检测/对比/检索/活体（按产品线） | 中 | 同上；还需关注区域与合规条款 | 按量计费 | 推荐“自建后端转发”，端侧只拿业务结果与审计号 |

#### 6.1.2 主流云厂商补充对比（阿里 / AWS / Azure）
云厂商更适合“云端识别/跨设备共享库/集中审计”的业务形态；若目标是 RK3288 离线门禁，云方案应作为可选备援链路，而非主链路。

<a id="tbl-6-7"></a>
#### 表 6-7 云厂商对比（阿里 / AWS / Azure）
| 厂商 | 典型服务 | 典型能力 | 识别形态 | 合规/区域要点 | 计费与限额 | 推荐集成方式 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 阿里云 | Facebody（人脸人体） | 检测/对比/属性/活体（视产品线） | 1:1/检索（视产品线） | 需按业务地区选择地域与合规策略 | 按量计费，需关注 QPS/并发限制 | 仅后端调用：端侧→自建服务→阿里云（统一鉴权与审计） |
| AWS | Rekognition | 检测/对比/搜索/集合（Collection） | 1:1/1:N | 数据出境与地域合规需评审 | 按量计费，限额需预先申请提升 | 仅后端调用：端侧不直连 AWS，统一通过自建服务做签名与审计 |
| Azure | Face API（Cognitive Services） | 检测/对比/识别/相似度（以版本为准） | 1:1/1:N | 关注资源所在区域与数据保留策略 | 按量计费，限额与配额可配置 | 仅后端调用：端侧只传最小化数据并拿到审计号 |

### 6.2 Android 集成步骤模板（依赖/模型/混淆/ABI）

#### 6.2.1 依赖与资源布局（模板）
- 模型/权重：建议放置在 `app/src/main/assets/models/` 或按产品策略从安全通道拉取后落盘（路径由配置项决定，不写死）。
- ABI：仅保留目标 ABI（RK3288 多为 `armeabi-v7a`），减少包体。

```gradle
android {
    defaultConfig {
        ndk {
            abiFilters "armeabi-v7a"
        }
    }
}
```

#### 6.2.2 混淆与反射（最小模板）
```text
-keep class androidx.camera.** { *; }
-dontwarn androidx.camera.**
```

#### 6.2.3 人脸方案逐项集成清单（从“取帧”到“门禁事件”）
目标是把方案落地拆成可验收、可定位故障的最小闭环。每一项都必须定义输入/输出与可观测日志，避免“黑箱集成”。

<a id="tbl-6-4"></a>
#### 表 6-4 集成清单与交付物（建议作为里程碑）
| 模块 | 输入 | 输出 | 失败表现 | 最小验收口径 |
| :--- | :--- | :--- | :--- | :--- |
| 帧接入（CameraX/Camera2） | `YUV_420_888` | 统一的 RGB/灰度张量 | 黑屏/卡死/帧率不稳 | 可稳定跑 10 分钟，无泄漏上升 |
| 人脸检测 | 帧张量 | 人脸框/关键点/质量分 | 误检/漏检 | 在基准集上输出固定 JSON 记录 |
| 对齐与裁剪 | 人脸框/关键点 | 归一化人脸 | 特征漂移 | 关键点对齐误差可统计 |
| 特征提取（Embedding） | 归一化人脸 | 向量（维度固定） | 抖动/不稳定 | 同图多次提取余弦相似度≥0.999 |
| 1:N 检索 | 向量 + 库 | TopK + score | 误识/延迟高 | N≤10000 时 P95 < 150ms（口径见 6.3） |
| 阈值与策略 | TopK + score + 质量 | PASS/REJECT/RETRY | 误放/误拒 | 阈值、拒识策略版本化可回滚 |
| 活体检测（PAD） | 帧序列/人脸 ROI | LIVE/SPOOF + 置信度 | 被照片/屏幕攻破 | 指标口径（APCER/BPCER）固定（见 6.4） |
| 特征存储 | 向量/用户信息 | 加密文件/DB | 泄露/损坏 | AES-GCM 加密；可迁移/可清理 |
| 审计与门禁事件 | PASS/REJECT | 事件记录/截图/录像 | 无法追溯 | 事件含 build_id、阈值版本、PAD 结果 |

#### 6.2.4 端侧 SDK 集成模板（ML Kit / MediaPipe / ArcFace / Dlib）
端侧集成的共同目标是：把“黑箱 SDK”拆成可审计的输入/输出与日志口径，并把版本、阈值、模型与许可证流程纳入可回滚的发布体系。

ML Kit（检测）最小接入模板（Kotlin，来自 ImageAnalysis）：
```kotlin
import androidx.camera.core.ImageProxy
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.face.FaceDetection
import com.google.mlkit.vision.face.FaceDetectorOptions

class MlKitFaceDetector {
    private val detector = FaceDetection.getClient(
        FaceDetectorOptions.Builder()
            .setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_FAST)
            .enableTracking()
            .build()
    )

    fun detect(imageProxy: ImageProxy, onDone: (faceCount: Int) -> Unit, onError: (Throwable) -> Unit) {
        val mediaImage = imageProxy.image ?: run {
            imageProxy.close()
            onDone(0)
            return
        }
        val img = InputImage.fromMediaImage(mediaImage, imageProxy.imageInfo.rotationDegrees)
        detector.process(img)
            .addOnSuccessListener { faces -> onDone(faces.size) }
            .addOnFailureListener { e -> onError(e) }
            .addOnCompleteListener { imageProxy.close() }
    }
}
```

MediaPipe（关键点/对齐）接入要点（模板口径）：
- 固定版本：将依赖版本写死在 Gradle，并在文档与构建产物中记录（见 2.3 节“变动项修改入口”）。
- 线程与背压：只允许单一分析线程；输入队列必须可控（与 CameraX `KEEP_ONLY_LATEST` 对齐）。
- 输出口径：至少输出关键点数量、置信度、耗时（ms），并为每次推理输出可关联的 `frame_id`。

ArcFace（商业 SDK）接入要点（模板口径）：
- 许可证：禁止写死在 APK；通过安全通道下发或由设备侧安全区持有，落盘需加密并可吊销。
- ABI：按设备只打包 `armeabi-v7a`，并在启动自检时校验 so 是否齐全、版本是否匹配。
- 日志：必须输出 SDK 版本、激活状态、阈值版本、特征维度、失败错误码与重试次数（写入 `ErrorLog/`）。

Dlib（NDK）接入要点（模板口径）：
- 构建：建议单独形成 `docs/examples/` 可复现工程；不要把大段构建脚本散落在业务模块。
- 性能：必须在目标设备实测 P95 延迟；若无法满足 6.3 口径，则只保留为研究分支，不进入量产主链路。

<a id="tbl-6-8"></a>
#### 表 6-8 特征维度/模板大小/阈值/延迟对比表（统一口径模板，需以实测填充）
| 方案 | 特征维度（Embedding Dim） | 模板大小（单人，bytes） | 相似度度量 | 初始阈值建议（余弦） | 端侧 P95（特征提取 ms） | N=10000 P95（检索 ms） | 备注（许可/落地） |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| ArcFace（开源实现：InsightFace） | 常见 512（以模型为准） | 维度×4（float32）或 ×1（int8） | 余弦相似度 | 0.2~0.6（必须以数据集标定） | 待实测 | 待实测 | 强制版本化模型与阈值；量化会改变阈值分布 |
| 虹软 ArcFace（商业 SDK） | 以 SDK 文档为准 | 以 SDK 文档为准 | SDK 内部定义/通常可映射余弦 | 以 SDK 推荐为准 | 待实测 | 待实测 | 许可证/离线激活流程必须可审计可回滚 |
| Dlib（128D） | 128 | 128×4（float32） | 余弦/欧氏（以实现为准） | 需标定 | 待实测 | 待实测 | NDK 构建与性能风险高，默认不进量产主链路 |
| 云端（百度/优图等） | 厂商侧 | 厂商侧 | 厂商侧 | 厂商侧 | 网络决定 | 网络决定 | 端侧只保留最小上传与审计号，阈值口径需统一映射 |

#### 6.2.6 逐方案集成模板（保证“口径一致、可回滚、可观测”）
本小节提供“逐方案最小可交付模板”，每个模板都以同一条主链路拆分：取帧 → 检测 → 对齐 → 特征 → 检索 → 决策 → 审计。

##### 6.2.6.1 ArcFace（开源实现：InsightFace）集成模板（建议主链路形态）
- 输入：固定 `YUV_420_888` → 统一预处理（旋转/裁剪/对齐）→ 模型输入张量（固定尺寸与归一化）。
- 输出：固定维度向量（float32 或 int8），并记录 `embedding_dim`、`model_version`、`preprocess_version`。
- 阈值：必须与数据集/标注/统计脚本绑定版本（见 6.3 表 6-2），禁止只改代码不改口径。

##### 6.2.6.2 虹软 ArcFace（商业 SDK）集成模板（许可证闭环优先）
- 初始化自检：输出 SDK 版本、激活状态、ABI 匹配结果、特征维度、错误码映射表版本。
- 输入输出封装：把 SDK 的输入格式统一映射为“归一化人脸 ROI + 旋转角 + 时间戳”，输出统一为“向量/或 SDK token + score”。
- 灰度/红外：若设备存在 IR/TOF 链路，必须把“哪一路进入识别”做成配置项，并写入审计事件。

##### 6.2.6.3 MediaPipe（关键点/对齐）集成模板（作为对齐模块更稳）
- 角色定位：优先用于关键点/FaceMesh 与质量评估，把“检测/识别”拆开以降低耦合。
- 线程模型：单线程分析 + 明确背压；每帧必须输出 `frame_id` 与耗时，便于定位抖动来源。

##### 6.2.6.4 ML Kit（检测）集成模板（作为检测模块更稳）
- 角色定位：只承担“检测/追踪/质量评估”，输出框与关键点；识别向量不要依赖 ML Kit（避免口径漂移）。
- 异常处理：每次回调必须保证 `ImageProxy.close()`，否则会触发背压堆积（见 5.5.1）。

##### 6.2.6.5 Dlib（研究链路）集成模板（只做可复现验证）
- 交付边界：仅交付 `docs/examples/` 级别的可复现工程与基准 CSV，不与业务模块强耦合。
- 退出条件：若在 RK3288 设备上 P95 不能满足 6.3 口径，则标记为“研究保留”，不进入主链路。

#### 6.2.5 云端 API 集成模板（百度 / 优图 / 阿里 / AWS / Azure）
云端集成必须满足两条硬约束：端侧不直连云厂商（避免密钥泄露与难审计），以及全链路最小化上传（只上传业务必要字段，尽量在端侧裁剪/脱敏）。

推荐拓扑（模板）：
- Android：只负责采集/裁剪/压缩 + 业务状态机，不持有云密钥。
- 自建后端：统一鉴权、签名、配额、熔断、审计与缓存，按需转发到各云厂商。
- 云厂商：只接收后端请求，并返回标准化结果。

后端环境变量模板（示例命名，按你们实际改）：
```text
BAIDU_API_KEY=...
BAIDU_SECRET_KEY=...
TENCENT_SECRET_ID=...
TENCENT_SECRET_KEY=...
ALICLOUD_ACCESS_KEY_ID=...
ALICLOUD_ACCESS_KEY_SECRET=...
AWS_ACCESS_KEY_ID=...
AWS_SECRET_ACCESS_KEY=...
AZURE_FACE_ENDPOINT=...
AZURE_FACE_KEY=...
```

> 安全提示（必须遵守）：
> - 以上变量只允许出现在服务器环境变量/密钥管理系统/本机私有配置中，禁止写入 APK、禁止提交到 Git。
> - 端侧（Android/Windows 客户端）不得直连云厂商，也不得持有任何云密钥；只允许调用自建后端的受控接口。

端侧 → 自建后端（HTTP）最小请求模板（示例）：
```json
{
  "request_id": "uuid",
  "device_id": "masked",
  "image_jpeg_base64": "<base64>",
  "mode": "detect|verify|identify",
  "options": {
    "max_faces": 1,
    "return_landmarks": true
  }
}
```

自建后端统一输出模板（建议）：
```json
{
  "request_id": "uuid",
  "provider": "baidu|tencent|alicloud|aws|azure",
  "audit_id": "provider_trace_id",
  "latency_ms": 123,
  "faces": [
    {
      "bbox": [0, 0, 100, 100],
      "score": 0.98,
      "landmarks": []
    }
  ],
  "match": {
    "user_id": "masked",
    "score": 0.83,
    "threshold": 0.80,
    "decision": "PASS|REJECT|RETRY"
  }
}
```

### 6.3 硬件要求与 1:N 基准口径（≤10000，<150ms，≥97%）

#### 6.3.1 指标与记录格式（建议作为统一口径）
- 1:N：输入 1 张查询人脸，与 N 条已注册特征进行相似度检索并返回 TopK。
- 延迟：统计“预处理 + 特征提取 + 检索”的总耗时，输出 P50/P95。
- 识别率：按数据集与标注口径定义（TP/FP/FN、阈值、拒识策略必须固定）。

<a id="tbl-6-2"></a>
#### 表 6-2 基准结果记录格式（CSV 字段定义）
| 字段 | 含义 |
| :--- | :--- |
| device_id | 设备标识（机型/序列号脱敏） |
| build_id | 应用版本/构建号 |
| dataset_id | 数据集标识（如 test_set01） |
| n_gallery | 库大小 N |
| tt_feature_ms | 特征提取耗时（ms） |
| tt_search_ms | 检索耗时（ms） |
| tt_total_ms | 总耗时（ms） |
| threshold | 阈值 |
| top1_correct | Top1 是否正确（0/1） |

#### 6.3.2 基准执行流程（可复现/可追责）
建议将“数据集/配置/代码”同时版本化，避免口径漂移：
- 数据集：`dataset_id` 必须与“来源/清洗规则/标注版本/脱敏策略”一一对应。
- 配置：阈值、TopK、拒识策略、PAD 开关等写入一份可导出的配置快照（用于复现）。
- 记录：每次基准都产出 CSV（见表 6-2）+ 汇总报告（P50/P95、ROC/DET、混淆矩阵）。

#### 6.3.3 CI 门禁（两级门禁：PR 快速门禁 + 设备夜间门禁）
门禁目标不是“在 CI 上跑出最终识别率”，而是防止关键能力退化与口径漂移。

PR 快速门禁（必须通过，分钟级）：
- 构建门禁：assembleDebug + 基础单测通过。
- 兼容门禁：模型文件校验（hash/版本号），向量维度与序列化格式不变。
- 回归门禁：TopK 排序稳定、阈值策略单测覆盖、加解密存储可读可写。

设备夜间门禁（可选，小时级，建议自建 Runner + 真机）：
- 在目标 RK3288 设备上跑固定基准集，产出 CSV 与汇总报告并作为 CI artifact。
- 阈值/识别率/延迟三项同时设“红线”，超出即阻断合并或标红报警（示例默认红线：Top1≥97%，N≤10000 时 P95<150ms，PAD 的 APCER/BPCER 需在产品定义范围内）。

### 6.4 活体检测对比与攻击用例（关联 ISO 30107-3）

<a id="tbl-6-3"></a>
#### 表 6-3 活体检测路线对比（工程可交付）
| 路线 | 硬件依赖 | 抗攻击能力 | 代价 | 适用场景 |
| :--- | :--- | :--- | :--- | :--- |
| RGB 静默 | 无 | 低到中 | 低 | 低风险场景 |
| RGB 动作指令 | 无 | 中 | 中 | 人机交互可接受 |
| 红外（IR） | IR 摄像头 | 中到高 | 中到高 | 工业门禁/中风险 |
| 3D 结构光 | 专用模组 | 高 | 高 | 高风险认证 |

攻击用例建议最小集合：照片翻拍、屏幕播放视频、打印面具/硅胶面具、不同光照与角度干扰。

#### 6.4.1 PAD 指标口径（最小集合）
<a id="tbl-6-5"></a>
#### 表 6-5 PAD 指标字段与解释（ISO 30107-3 术语对齐）
| 指标 | 含义 | 备注 |
| :--- | :--- | :--- |
| APCER | 攻击样本被错误接受的比例 | 越低越好 |
| BPCER | 真用户被错误拒绝的比例 | 越低越好 |
| ACER | (APCER + BPCER)/2 | 汇总指标 |
| EER | 等错误率点 | 用于对比不同阈值 |

### 6.5 合规与性能优化：特征加密存储、热更新、多线程、零拷贝、降级策略

#### 6.5.1 特征加密存储（AES-256-GCM 口径）
本小节目标是提供一套“可直接拷贝使用”的 **AES-256-GCM + Android Keystore** 特征加密落盘模板，并明确异常处理与降级策略，避免把“存储加密”做成黑箱。

**安全目标（工程口径）**：
- 密钥仅存在于 Keystore，不可导出；应用卸载/清数据后自动失效。
- 文件被拷贝到其他设备/用户空间后不可解密（与设备 Keystore 绑定）。
- 任意篡改密文/IV/AAD 都必须解密失败（GCM 完整性校验）。
- 落盘文件不包含可反推生物特征的明文字段（例如 user_id 明文、向量维度明文等可选）。

<a id="tbl-6-9"></a>
#### 表 6-9 特征加密文件格式（建议固定，便于兼容与迁移）
| 字段 | 长度 | 含义 | 备注 |
| :--- | ---: | :--- | :--- |
| magic | 4 | 魔数 `FST0` | 用于快速识别文件类型 |
| version | 1 | 版本号 | 建议从 `1` 开始 |
| iv_len | 1 | IV 长度 | 建议固定 `12`（GCM 推荐长度） |
| iv | iv_len | GCM IV | 每次加密必须随机生成，不可复用 |
| aad_len | 2 | AAD 长度（BE） | 可为 0 |
| aad | aad_len | 绑定上下文的 AAD | 例如 `userIdHash|modelVer|schemaVer` |
| ct_len | 4 | 密文长度（BE） | `ciphertext || tag` 的总长度 |
| ciphertext_and_tag | ct_len | 密文+Tag | Tag 建议 128bit |

**AAD 建议（可选但推荐）**：把“能在业务上绑定该模板”的上下文字段写入 AAD（不加密但参与完整性校验），例如：`user_id_hash`、`embedding_dim`、`model_version`、`feature_schema_version`。这样即便密文被复制到其他用户条目下，也会因 AAD 不匹配而解密失败（等价于“额外绑定”）。

##### 6.5.1.1 Kotlin 关键代码模板（Keystore Key + AES-GCM 加解密 + 原子写入）
```kotlin
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.security.GeneralSecurityException
import java.security.KeyStore
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

sealed class FeatureCryptoError(message: String, cause: Throwable? = null) : Exception(message, cause) {
    class KeyPermanentlyInvalidated(cause: Throwable) : FeatureCryptoError("Keystore 密钥已失效/不可恢复", cause)
    class CorruptedOrTampered(cause: Throwable) : FeatureCryptoError("密文损坏或被篡改（GCM 校验失败）", cause)
    class IoFailure(cause: Throwable) : FeatureCryptoError("文件读写失败", cause)
    class CryptoFailure(cause: Throwable) : FeatureCryptoError("加解密失败", cause)
    class BadFormat : FeatureCryptoError("文件格式不合法或版本不兼容")
}

object FeatureCrypto {
    private const val ANDROID_KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "rk3288_feature_aes_gcm_v1"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"
    private const val MAGIC = "FST0"
    private const val VERSION: Byte = 1
    private const val IV_LEN: Int = 12
    private const val TAG_LEN_BITS: Int = 128

    private fun getOrCreateKey(): SecretKey {
        val ks = KeyStore.getInstance(ANDROID_KEYSTORE).apply { load(null) }
        val existing = (ks.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.secretKey
        if (existing != null) return existing

        val keyGenerator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEYSTORE)
        val spec = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setKeySize(256)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setRandomizedEncryptionRequired(true)
            .build()
        keyGenerator.init(spec)
        return keyGenerator.generateKey()
    }

    fun encrypt(plaintext: ByteArray, aad: ByteArray?): ByteArray {
        try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
            if (aad != null && aad.isNotEmpty()) cipher.updateAAD(aad)
            val iv = cipher.iv
            require(iv.size == IV_LEN) { "GCM IV 长度异常: ${iv.size}" }
            val ct = cipher.doFinal(plaintext)
            return encodeEnvelope(iv = iv, aad = aad ?: byteArrayOf(), ciphertextAndTag = ct)
        } catch (e: GeneralSecurityException) {
            throw FeatureCryptoError.CryptoFailure(e)
        } catch (e: IllegalArgumentException) {
            throw FeatureCryptoError.CryptoFailure(e)
        }
    }

    fun decrypt(envelope: ByteArray, aadExpected: ByteArray?): ByteArray {
        val parsed = try {
            decodeEnvelope(envelope)
        } catch (e: FeatureCryptoError.BadFormat) {
            throw e
        } catch (e: Exception) {
            throw FeatureCryptoError.BadFormat()
        }

        val aad = parsed.aad
        if (aadExpected != null && !aad.contentEquals(aadExpected)) {
            throw FeatureCryptoError.CorruptedOrTampered(IllegalStateException("AAD 不匹配"))
        }

        try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            val spec = GCMParameterSpec(TAG_LEN_BITS, parsed.iv)
            cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), spec)
            if (aad.isNotEmpty()) cipher.updateAAD(aad)
            return cipher.doFinal(parsed.ciphertextAndTag)
        } catch (e: AEADBadTagException) {
            throw FeatureCryptoError.CorruptedOrTampered(e)
        } catch (e: GeneralSecurityException) {
            val msg = (e.message ?: "")
            val isPermanentlyInvalidated =
                msg.contains("Key", ignoreCase = true) && msg.contains("invalid", ignoreCase = true)
            if (isPermanentlyInvalidated) throw FeatureCryptoError.KeyPermanentlyInvalidated(e)
            throw FeatureCryptoError.CryptoFailure(e)
        }
    }

    fun writeEncryptedAtomically(targetFile: File, plaintext: ByteArray, aad: ByteArray?) {
        val tmp = File(targetFile.parentFile, "${targetFile.name}.tmp")
        try {
            val blob = encrypt(plaintext, aad)
            FileOutputStream(tmp).use { out ->
                out.write(blob)
                out.fd.sync()
            }
            if (targetFile.exists() && !targetFile.delete()) {
                throw FeatureCryptoError.IoFailure(IllegalStateException("无法删除旧文件: ${targetFile.absolutePath}"))
            }
            if (!tmp.renameTo(targetFile)) {
                throw FeatureCryptoError.IoFailure(IllegalStateException("无法原子替换文件: ${targetFile.absolutePath}"))
            }
        } catch (e: FeatureCryptoError) {
            tmp.delete()
            throw e
        } catch (e: Exception) {
            tmp.delete()
            throw FeatureCryptoError.IoFailure(e)
        }
    }

    fun readDecrypted(targetFile: File, aadExpected: ByteArray?): ByteArray {
        try {
            val bytes = FileInputStream(targetFile).use { it.readBytes() }
            return decrypt(bytes, aadExpected)
        } catch (e: FeatureCryptoError) {
            throw e
        } catch (e: Exception) {
            throw FeatureCryptoError.IoFailure(e)
        }
    }

    private data class Envelope(val iv: ByteArray, val aad: ByteArray, val ciphertextAndTag: ByteArray)

    private fun encodeEnvelope(iv: ByteArray, aad: ByteArray, ciphertextAndTag: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(MAGIC.toByteArray(Charsets.US_ASCII))
        out.write(byteArrayOf(VERSION))
        out.write(byteArrayOf(iv.size.toByte()))
        out.write(iv)
        out.write(byteArrayOf(((aad.size ushr 8) and 0xFF).toByte(), (aad.size and 0xFF).toByte()))
        out.write(aad)
        out.write(
            byteArrayOf(
                ((ciphertextAndTag.size ushr 24) and 0xFF).toByte(),
                ((ciphertextAndTag.size ushr 16) and 0xFF).toByte(),
                ((ciphertextAndTag.size ushr 8) and 0xFF).toByte(),
                (ciphertextAndTag.size and 0xFF).toByte()
            )
        )
        out.write(ciphertextAndTag)
        return out.toByteArray()
    }

    private fun decodeEnvelope(input: ByteArray): Envelope {
        fun u8(b: Byte): Int = b.toInt() and 0xFF
        fun requireAt(cond: Boolean) { if (!cond) throw FeatureCryptoError.BadFormat() }

        var i = 0
        requireAt(input.size >= 4 + 1 + 1 + 12 + 2 + 4)
        val magic = String(input.copyOfRange(0, 4), Charsets.US_ASCII)
        requireAt(magic == MAGIC)
        i += 4
        val ver = input[i++]
        requireAt(ver == VERSION)
        val ivLen = u8(input[i++])
        requireAt(ivLen == IV_LEN)
        requireAt(i + ivLen <= input.size)
        val iv = input.copyOfRange(i, i + ivLen)
        i += ivLen
        requireAt(i + 2 <= input.size)
        val aadLen = (u8(input[i]) shl 8) or u8(input[i + 1])
        i += 2
        requireAt(i + aadLen <= input.size)
        val aad = input.copyOfRange(i, i + aadLen)
        i += aadLen
        requireAt(i + 4 <= input.size)
        val ctLen =
            (u8(input[i]) shl 24) or (u8(input[i + 1]) shl 16) or (u8(input[i + 2]) shl 8) or u8(input[i + 3])
        i += 4
        requireAt(ctLen > 0)
        requireAt(i + ctLen == input.size)
        val ct = input.copyOfRange(i, i + ctLen)
        return Envelope(iv = iv, aad = aad, ciphertextAndTag = ct)
    }
}
```

##### 6.5.1.1（补充）Java 示例（KeyStore + KeyGenerator + GCMParameterSpec）
```java
import java.nio.ByteBuffer;
import java.security.KeyStore;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

public final class FeatureCryptoJava {
    private static final String ANDROID_KEYSTORE = "AndroidKeyStore";
    private static final String KEY_ALIAS = "rk3288_feature_aes_gcm_v1";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final byte[] MAGIC = new byte[]{ 'F', 'S', 'T', '0' };
    private static final byte VERSION = 1;
    private static final int IV_LEN = 12;
    private static final int TAG_LEN_BITS = 128;

    private FeatureCryptoJava() {}

    private static SecretKey getOrCreateKey() throws Exception {
        KeyStore ks = KeyStore.getInstance(ANDROID_KEYSTORE);
        ks.load(null);
        KeyStore.Entry entry = ks.getEntry(KEY_ALIAS, null);
        if (entry instanceof KeyStore.SecretKeyEntry) {
            return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
        }

        KeyGenerator kg = KeyGenerator.getInstance("AES", ANDROID_KEYSTORE);
        android.security.keystore.KeyGenParameterSpec spec =
                new android.security.keystore.KeyGenParameterSpec.Builder(
                        KEY_ALIAS,
                        android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                                | android.security.keystore.KeyProperties.PURPOSE_DECRYPT
                )
                        .setKeySize(256)
                        .setBlockModes(android.security.keystore.KeyProperties.BLOCK_MODE_GCM)
                        .setEncryptionPaddings(android.security.keystore.KeyProperties.ENCRYPTION_PADDING_NONE)
                        .setRandomizedEncryptionRequired(true)
                        .build();
        kg.init(spec);
        return kg.generateKey();
    }

    public static byte[] encryptToEnvelope(byte[] plaintext, byte[] aad) throws Exception {
        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        SecretKey key = getOrCreateKey();
        cipher.init(Cipher.ENCRYPT_MODE, key);
        if (aad != null && aad.length > 0) cipher.updateAAD(aad);
        byte[] iv = cipher.getIV();
        if (iv == null || iv.length != IV_LEN) throw new IllegalStateException("GCM IV 长度异常");
        byte[] ciphertextAndTag = cipher.doFinal(plaintext);
        return encodeEnvelope(iv, aad == null ? new byte[0] : aad, ciphertextAndTag);
    }

    public static byte[] decryptFromEnvelope(byte[] envelope, byte[] aadExpected) throws Exception {
        ParsedEnvelope p = decodeEnvelope(envelope);
        if (aadExpected != null && !java.util.Arrays.equals(p.aad, aadExpected)) {
            throw new IllegalStateException("AAD 不匹配");
        }

        Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        SecretKey key = getOrCreateKey();
        GCMParameterSpec spec = new GCMParameterSpec(TAG_LEN_BITS, p.iv);
        cipher.init(Cipher.DECRYPT_MODE, key, spec);
        if (p.aad.length > 0) cipher.updateAAD(p.aad);
        return cipher.doFinal(p.ciphertextAndTag);
    }

    private static byte[] encodeEnvelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
        int size = 4 + 1 + 1 + iv.length + 2 + aad.length + 4 + ciphertextAndTag.length;
        ByteBuffer buf = ByteBuffer.allocate(size);
        buf.put(MAGIC);
        buf.put(VERSION);
        buf.put((byte) iv.length);
        buf.put(iv);
        buf.putShort((short) aad.length);
        buf.put(aad);
        buf.putInt(ciphertextAndTag.length);
        buf.put(ciphertextAndTag);
        return buf.array();
    }

    private static final class ParsedEnvelope {
        final byte[] iv;
        final byte[] aad;
        final byte[] ciphertextAndTag;

        ParsedEnvelope(byte[] iv, byte[] aad, byte[] ciphertextAndTag) {
            this.iv = iv;
            this.aad = aad;
            this.ciphertextAndTag = ciphertextAndTag;
        }
    }

    private static ParsedEnvelope decodeEnvelope(byte[] input) {
        if (input == null || input.length < (4 + 1 + 1 + IV_LEN + 2 + 4)) {
            throw new IllegalArgumentException("文件格式不合法");
        }
        ByteBuffer buf = ByteBuffer.wrap(input);
        byte[] magic = new byte[4];
        buf.get(magic);
        if (!java.util.Arrays.equals(magic, MAGIC)) throw new IllegalArgumentException("magic 不匹配");
        byte ver = buf.get();
        if (ver != VERSION) throw new IllegalArgumentException("version 不兼容");
        int ivLen = buf.get() & 0xFF;
        if (ivLen != IV_LEN) throw new IllegalArgumentException("IV 长度不合法");
        byte[] iv = new byte[ivLen];
        buf.get(iv);
        int aadLen = buf.getShort() & 0xFFFF;
        if (aadLen < 0 || aadLen > buf.remaining()) throw new IllegalArgumentException("AAD 长度不合法");
        byte[] aad = new byte[aadLen];
        buf.get(aad);
        int ctLen = buf.getInt();
        if (ctLen <= 0 || ctLen != buf.remaining()) throw new IllegalArgumentException("密文长度不合法");
        byte[] ct = new byte[ctLen];
        buf.get(ct);
        return new ParsedEnvelope(iv, aad, ct);
    }
}
```

##### 6.5.1.2 异常处理与降级策略（必须固定口径）
<a id="tbl-6-10"></a>
#### 表 6-10 特征加密存储异常映射与处理策略（建议直接照表实现）
| 场景 | 典型异常/表现 | 风险含义 | 推荐处理（门禁业务口径） |
| :--- | :--- | :--- | :--- |
| 密文被篡改/损坏 | `AEADBadTagException` 或 AAD 不匹配 | 数据不可信 | 视为该条模板损坏：删除该用户模板文件并触发重新注册/重新采集 |
| Keystore 密钥不可用 | `UnrecoverableKeyException`/`KeyStoreException`/设备策略导致的失效 | 历史数据无法解密 | 删除密钥别名与所有模板文件；引导重新注册；写入 `ErrorLog/` 便于审计 |
| 读写失败 | `IOException`/rename 失败 | 数据可能丢失或不完整 | 保留 tmp 文件（或清理并重试）；必要时降级为“只读模式”并提示空间/权限 |
| 版本不兼容 | magic/version 不匹配 | 协议漂移 | 拒绝加载并提示升级/迁移；禁止 silent fallback（避免误用错误口径） |

##### 6.5.1.3 关键约束（必须写进工程检查清单）
- 禁止复用 IV：同一 key 下 IV 重复会破坏 GCM 安全性；必须每次随机生成。
- 禁止把 user_id 明文写入文件名：文件名可用 hash/随机 id，并把映射放在受控 DB（可同样加密）中。
- Key Alias 与协议版本绑定：`feature_aes_gcm_v1` 与 envelope `version=1` 成对出现，便于升级到 v2（例如增加压缩/分片/密钥轮换）。

#### 6.5.2 性能优化要点（端侧）
- 摄像头侧：CameraX Preview + ImageAnalysis 背压策略（KEEP_ONLY_LATEST），避免分析阻塞。
- 预处理侧：复用中间 Buffer，减少 `Bitmap` 与大对象频繁分配。
- 推理侧：固定线程池，避免每帧创建线程；热身推理（warm-up）后再开始计时。
- 检索侧：N≤10000 时优先采用向量化加速与批量相似度计算；输出 TopK 需要固定排序策略与稳定阈值。

### 6.6 可复现 Demo 交付：构建/测试/CI 最小模板

#### 6.6.1 Demo APK 构建脚本模板
本节交付目标是：给出一个“可被 CI 直接跑起来”的 Demo 工程模板，并把 **Gradle/NDK/测试框架/门禁规则** 固定为可审计资产，避免版本漂移导致“昨天能构建、今天不能构建”。

**模板口径（示例，不强制等同于本仓库当前版本）**：
- Gradle Wrapper：7.5.1
- Android Gradle Plugin（AGP）：7.4.2（与 Gradle 7.5.x 兼容）
- JDK：11（AGP 7.4 推荐口径；若改 17，需同步验证）
- NDK：25.2.9519653（25.x 系列）

Demo 目录建议（可放在 `docs/examples/demo_android/`，或独立仓库）：
```text
demo_android/
├── gradle/wrapper/gradle-wrapper.properties
├── settings.gradle
├── build.gradle
├── gradle.properties
└── app/
    ├── build.gradle
    ├── src/main/AndroidManifest.xml
    ├── src/main/java/.../MainActivity.kt
    ├── src/main/cpp/CMakeLists.txt
    ├── src/main/cpp/native-lib.cpp
    ├── src/test/java/.../ThresholdPolicyTest.kt
    ├── src/test/java/.../RobolectricSmokeTest.kt
    └── src/androidTest/java/.../CameraUiTest.kt
```

Gradle Wrapper 固定（`gradle/wrapper/gradle-wrapper.properties`）：
```properties
distributionUrl=https\://services.gradle.org/distributions/gradle-7.5.1-bin.zip
```

`settings.gradle`（插件与仓库固定，避免镜像漂移）：
```gradle
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "demo_android"
include(":app")
```

根 `build.gradle`（AGP 固定）：
```gradle
plugins {
    id "com.android.application" version "7.4.2" apply false
    id "org.jetbrains.kotlin.android" version "1.8.22" apply false
}
```

`gradle.properties`（稳定性与内存口径）：
```properties
android.useAndroidX=true
org.gradle.jvmargs=-Xmx4g -Dfile.encoding=UTF-8
org.gradle.daemon=false
```

`app/build.gradle`（NDK25 + CMake + 测试依赖最小集）：
```gradle
plugins {
    id "com.android.application"
    id "org.jetbrains.kotlin.android"
}

android {
    namespace "com.example.demo"
    compileSdk 34

    defaultConfig {
        applicationId "com.example.demo"
        minSdk 21
        targetSdk 34
        versionCode 1
        versionName "v0.1beta1"

        ndkVersion "25.2.9519653"
        ndk {
            abiFilters "armeabi-v7a", "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                cppFlags "-std=c++17 -fno-exceptions -fno-rtti"
                arguments "-DANDROID_STL=c++_shared"
            }
        }

        testInstrumentationRunner "androidx.test.runner.AndroidJUnitRunner"
    }

    buildTypes {
        release {
            minifyEnabled false
        }
    }

    externalNativeBuild {
        cmake {
            path "src/main/cpp/CMakeLists.txt"
            version "3.22.1"
        }
    }

    testOptions {
        unitTests {
            includeAndroidResources true
        }
    }
}

dependencies {
    implementation "androidx.core:core-ktx:1.13.1"
    implementation "androidx.appcompat:appcompat:1.7.0"
    implementation "com.google.android.material:material:1.12.0"

    testImplementation "junit:junit:4.13.2"
    testImplementation "org.robolectric:robolectric:4.12.2"
    testImplementation "androidx.test:core:1.6.1"

    androidTestImplementation "androidx.test.ext:junit:1.2.1"
    androidTestImplementation "androidx.test.espresso:espresso-core:3.6.1"

    androidTestImplementation "androidx.camera:camera-core:1.3.4"
    androidTestImplementation "androidx.camera:camera-camera2:1.3.4"
    androidTestImplementation "androidx.camera:camera-lifecycle:1.3.4"
    androidTestImplementation "androidx.camera:camera-testing:1.3.4"
}
```

`src/main/cpp/CMakeLists.txt`（NDK demo 最小模板）：
```cmake
cmake_minimum_required(VERSION 3.22.1)
project(demo_android)

add_library(native-lib SHARED native-lib.cpp)

find_library(log-lib log)
target_link_libraries(native-lib ${log-lib})
```

#### 6.6.2 JUnit + Robolectric（单测）与 Espresso + MockCamera（集测）模板
**JUnit（纯逻辑）**：只测“可复现口径”的逻辑，不依赖 Android 框架，例如：阈值策略、TopK 稳定排序、序列化格式稳定。
```kotlin
import org.junit.Assert.assertEquals
import org.junit.Test

class ThresholdPolicyTest {
    @Test
    fun `cosine threshold should be stable`() {
        val threshold = 0.80
        val score = 0.801
        val decision = if (score >= threshold) "PASS" else "REJECT"
        assertEquals("PASS", decision)
    }
}
```

**Robolectric（轻量 Android 行为）**：验证资源可用、Context 行为、以及“不会意外崩溃”的 smoke test。注意：Robolectric 对 Keystore/硬件相关能力不应当作为真实性验证口径，Keystore 相关必须由真机/模拟器上的 instrumentation 覆盖。
```kotlin
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertNotNull
import org.junit.Test

class RobolectricSmokeTest {
    @Test
    fun `application context should be available`() {
        val ctx = ApplicationProvider.getApplicationContext<Context>()
        assertNotNull(ctx)
    }
}
```

**Espresso + MockCamera（集测）**：目标是把“UI 流程 + 相机恢复能力”做成可自动化回归。推荐两条路径：
- 路径 A（优先）：对“相机帧输入”做抽象（FrameSource），instrumentation 用 Fake/录制帧回放实现；UI 只验证权限/回流/状态机，不强依赖真摄像头。
- 路径 B（CameraX 测试库）：使用 `androidx.camera:camera-testing` 提供的 Fake/Mock 能力，验证 `bind/unbind`、异常回调与恢复策略不回归。

Espresso 用例骨架（权限回流 + 页面不崩溃口径）：
```kotlin
import androidx.test.ext.junit.rules.ActivityScenarioRule
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.rule.GrantPermissionRule
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class CameraUiTest {
    @get:Rule
    val grant: GrantPermissionRule = GrantPermissionRule.grant(android.Manifest.permission.CAMERA)

    @get:Rule
    val rule = ActivityScenarioRule(MainActivity::class.java)

    @Test
    fun openActivity_shouldNotCrash() {
        rule.scenario.onActivity { }
    }
}
```

#### 6.6.3 GitHub Actions：构建 + 测试 + 门禁（识别率≥97% 且 泄漏≤5MB）模板
本模板把门禁拆成两类：PR 快速门禁（分钟级）与 Nightly 设备门禁（小时级）。PR 门禁不强求跑出最终识别率，而是验证“口径不漂移 + 指标解析链路有效”；Nightly 门禁可接真机 Runner 或自研设备农场。

> 边界说明：
> - 下述内容为“参考模板”，用于说明门禁拆分与指标口径；不代表本仓库已启用 `.github/workflows/android-ci.yml`。
> - 本仓库当前实际启用的 CI 工作流为：`.github/workflows/ci.yml`（仓库卫生 + 最小单测/构建验证）。

<a id="tbl-6-11"></a>
#### 表 6-11 CI 门禁阈值（建议默认值，可按产品定义调整）
| 指标 | 默认阈值 | 产出来源 | 失败处理 |
| :--- | ---: | :--- | :--- |
| Top1 识别率（Accuracy） | ≥ 0.97 | `tests/metrics/accuracy.json` | 阻断合并 |
| 泄漏增长（Leak Delta） | ≤ 5 MB | `tests/metrics/leak.json` | 阻断合并 |

GitHub Actions 工作流（`.github/workflows/android-ci.yml` 模板，含门禁解析）：
```yaml
name: android-ci
on:
  pull_request:
  push:
    branches: [ "main" ]

jobs:
  pr-gate:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Build + Unit Tests
        run: ./gradlew --no-daemon clean assembleDebug testDebugUnitTest lintDebug

      - name: Upload test reports
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: reports
          path: |
            **/build/reports/**
            **/build/test-results/**

      - name: Evaluate gates (accuracy + leak)
        run: python3 scripts/ci/evaluate_gates.py tests/metrics/accuracy.json tests/metrics/leak.json

  pr-instrumentation:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Connected Android Tests (Emulator)
        uses: reactivecircus/android-emulator-runner@v2
        with:
          api-level: 30
          arch: x86_64
          target: google_apis
          disable-animations: true
          emulator-options: -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect
          script: ./gradlew --no-daemon connectedDebugAndroidTest

  nightly-device-gate:
    if: github.event_name == 'push'
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-java@v4
        with:
          distribution: temurin
          java-version: "11"
      - uses: android-actions/setup-android@v3
      - name: Nightly placeholder
        run: echo "此处建议改为自建 Runner + 真机基准（产出 CSV/JSON 并复用同一 evaluate 脚本）"
```

门禁解析脚本模板（`scripts/ci/evaluate_gates.py`，以 JSON 为准，避免解析 logcat 漂移）：
```python
import json
import sys

ACCURACY_MIN = 0.97
LEAK_MB_MAX = 5.0

def read_json(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)

def main() -> int:
    if len(sys.argv) != 3:
        print("usage: evaluate_gates.py <accuracy.json> <leak.json>")
        return 2
    acc = read_json(sys.argv[1])
    leak = read_json(sys.argv[2])

    accuracy = float(acc["top1_accuracy"])
    leak_mb = float(leak["leak_delta_mb"])

    ok = True
    if accuracy < ACCURACY_MIN:
        ok = False
        print(f"[GATE] accuracy fail: {accuracy:.4f} < {ACCURACY_MIN:.4f}")
    else:
        print(f"[GATE] accuracy ok: {accuracy:.4f} >= {ACCURACY_MIN:.4f}")

    if leak_mb > LEAK_MB_MAX:
        ok = False
        print(f"[GATE] leak fail: {leak_mb:.2f} MB > {LEAK_MB_MAX:.2f} MB")
    else:
        print(f"[GATE] leak ok: {leak_mb:.2f} MB <= {LEAK_MB_MAX:.2f} MB")

    return 0 if ok else 1

if __name__ == "__main__":
    raise SystemExit(main())
```

指标文件格式建议（由测试/基准产出，稳定可审计）：
```json
{ "top1_accuracy": 0.9731, "dataset_id": "test_set01", "build_id": "v0.1beta1" }
```
```json
{ "leak_delta_mb": 3.2, "scenario": "camera_open_close_50x", "build_id": "v0.1beta1" }
```

### 6.7 本仓库已落地的 YOLO+ArcFace 工程骨架（RK3288）

本小节用于把“方案研究”同步为可执行入口，便于后续重构时对齐口径与复现。

#### 6.7.1 关键代码入口（按职责）
- YOLO 人脸检测（统一输出 bbox/score/可选 5 点关键点）：`src/cpp/include/YoloFaceDetector.h`
- ArcFace 特征（固定 512D、float32、L2 归一化、版本化）：`src/cpp/include/ArcFaceEmbedder.h`
- 对齐（优先关键点仿射；回退 bbox 裁剪+resize 到 112×112）：`src/cpp/include/FaceAlign.h`
- 1:N 检索（N≤10000 线性 TopK + 稳定排序）：`src/cpp/include/FaceSearch.h`
- 阈值策略（版本化/回滚/连续 K 次通过触发）：`src/cpp/include/ThresholdPolicy.h`
- 闭环管线（事件 JSON + 审计落盘）：`src/cpp/include/FaceInferencePipeline.h`
- 推理对比基准（ncnn vs OpenCV DNN）：`src/cpp/tools/inference_bench_cli.cpp`
- 模板 schema（特征模板二进制格式与版本）：`src/cpp/include/FaceTemplate.h`
- Android 加密落盘（Keystore + AES-GCM，文件名不落明文 user_id）：`src/java/com/example/rk3288_opencv/FeatureTemplateEncryptedStore.java`

#### 6.7.2 设备侧可复现命令（不提交模型，部署侧提供路径）
1) 推理后端对比基准（输出 CSV/JSON 到 `tests/metrics/`）：
```bash
./inference_bench_cli --backend both \
  --opencv-model <model.onnx> \
  --ncnn-param <model.param> --ncnn-bin <model.bin> \
  --w 320 --h 320 --warmup 10 --iters 100 --out-dir tests/metrics
```

2) YOLO 离线图片检测（输出 JSON 到 stdout，并落盘到 `tests/metrics/` 或 `ErrorLog/`）：
```bash
./rk3288_cli --yolo-face <image.jpg> --backend opencv --model <yolo_face.onnx> --w 320 --h 320 --score 0.40 --nms 0.45
```

3) YOLO+ArcFace 识别闭环（输出事件 JSON + 审计落盘）：
```bash
./rk3288_cli --face-infer <image.jpg> \
  --yolo-backend opencv --yolo-model <yolo_face.onnx> --yolo-w 320 --yolo-h 320 \
  --arc-backend opencv --arc-model <arcface.onnx> --arc-w 112 --arc-h 112 \
  --gallery-dir <gallery_dir> --topk 5 --threshold 0.35 --consecutive 1
```

4) 性能基线采集（自动产出 raw/summary CSV + Markdown 报告）：
- 报告模板：`tests/reports/face_baseline/REPORT_TEMPLATE.md`
```bash
./rk3288_cli --face-baseline <imagePath|dir> \
  --warmup 5 --repeat 50 --detect-stride 1 --include-load 0 \
  --yolo-backend opencv --yolo-model <yolo_face.onnx> --yolo-w 320 --yolo-h 320 \
  --arc-backend opencv --arc-model <arcface.onnx> --arc-w 112 --arc-h 112 \
  --gallery-dir <gallery_dir> --topk 5 --face-select score_area \
  --out-dir tests/reports/face_baseline --out-prefix face_baseline
```

### 6.8 分支策略 / 代码规范 / 发布流程（用于文档同步审计）

#### 6.8.1 分支策略（Branch Strategy）
- 主分支：`master`（默认稳定分支，所有合并进入该分支后再做版本化与交付）
- 功能分支：`feature/<topic>`（开发完通过 PR 合并回 `master`）
- 修复分支：`hotfix/<issue>`（紧急问题修复，合并回 `master`）
- 版本标签：`vX.Y.Z`（与 Changelog/构建产物口径一致）

#### 6.8.2 代码规范（Code Style）
- 语言与编码：统一 UTF-8；对外输出（日志/报告/文档）默认使用 ZH_CN。
- 安全：禁止输出密钥/令牌/隐私信息；日志粘贴前必须打码。
- 质量门禁（建议）：至少通过基础构建、核心单测、文档同步审计（`scripts/docs-sync-audit.js`）。

#### 6.8.3 发布流程（Release Process）
1) 运行 `scripts/docs-sync-audit.js`，确保 high 缺陷为 0；若存在 BSP/defconfig/内核配置占位，则先补齐输入再发布。
2) 生成版本号并更新本文件的 Changelog（记录 build_id、模型版本号、阈值版本号等关键口径）。
3) 产出构建物（APK/Native 可执行程序），并附带性能基线报告（`tests/reports/face_baseline/`）。

---

## 6.9 推荐决策与分阶段路线图 + Go/No-Go 门槛 + 评审签署流程（RK3288 + Qualcomm）

本章目标：在“默认配置不引入稳定性退化”的前提下，分阶段获得可观测、可审计、可回滚的性能收益，并确保 **RK3288（CPU+可选 GPU）** 与 **至少 1 台 Qualcomm（Android，CPU+GPU/NNAPI 可选）** 的结果可比、可复现、可签署。

### 6.9.1 默认基线路径（Baseline Path）

默认基线的定位不是“最慢”，而是“最可控、最易定位、最易回滚”。基线必须满足：单一入口、可复现命令、稳定的指标输出、明确的失败原因。

**默认基线（必须长期可用）**：
- 推理后端：`ncnn (CPU)` 为主选；`OpenCV DNN (CPU)` 为备选回退（用于对照与兜底）。
- 通用计算：OpenCL/UMat 视为“可选加速”，不作为正确性/稳定性前提。
- 线程与资源：固定线程池、显式内存复用策略；禁止把“GPU 可用”当作假设。
- 失败处理：任何加速路径失败（初始化/运行时/性能抖动/异常码）必须 **自动回落到基线**，并输出“失败原因码 + 证据日志 + 回落结果”。

**现状差距提示（需要在路线图第 1 阶段补齐）**：
- 当前 `VideoManager` 构造函数直接调用 `cv::ocl::setUseOpenCL(true)`（见 `src/cpp/src/VideoManager.cpp`），属于“默认启用可选加速”的行为；若遇到驱动不稳定/算子覆盖不可控，会导致“收益不确定、故障难定位、无法精确回滚”。该行为应纳入“加速开关 + 自检 + 允许列表”体系后再进入默认配置。

### 6.9.2 推荐阶段顺序（先可观测与基准，再逐步引入加速）

阶段顺序遵循三条硬约束：
- 先把“看得见”做好：没有可观测与基准，就无法判断收益、也无法解释失败原因。
- 先把“能回退”做好：每一个加速开关都必须能独立回滚，并能给出可读的失败原因。
- 先做“低风险收益”：优先改 CPU/内存/线程等确定性优化，再引入 GPU/专用运行时等高不确定性加速。

#### 阶段 0：可观测与基准统一（必须先做）

**目标**：建立跨平台一致的指标与报告，让后续每个加速开关都有“收益/退化/失败原因”的证据链。

**范围**：
- 指标口径统一：延迟（P50/P95/P99）、吞吐（FPS/帧间隔）、资源（CPU/RSS/显存 best-effort）、稳定性（崩溃/ANR/相机重启次数）、识别指标（Top1、FAR/FRR 或至少 Top1+误识统计）。
- 报告落盘统一：输出到 `tests/metrics/` 与 `ErrorLog/`（失败材料必须落盘，便于复现与审计）。
- 设备集最小集：RK3288 1 台 + Qualcomm 1 台（真实设备），并明确“摄像头模组/分辨率/FPS/光照条件”作为基准前置条件。

**验收指标（必须同时满足）**：
- 任一设备可一键跑出“基线报告”（含原始 CSV/JSON + 汇总 Markdown），且报告字段稳定。
- 失败时能输出明确原因（例如：模型缺失、相机打开失败、OpenCL 初始化失败、权限拒绝等）并落盘到 `ErrorLog/`。

**退出条件（触发即停止进入下一阶段）**：
- 指标字段不稳定/无法复现（同一 commit 不同结果差异无解释）。
- 失败无法落盘或原因不可定位（只能看到“失败/崩溃”而没有证据）。

#### 阶段 1：加速开关 + 回退 + 失败原因码（必须先做）

**目标**：把所有“可能的加速点”包装成可控开关，并保证每个开关都有独立回退与失败原因。

**最小交付口径（建议）**：
- 开关形态：`AUTO / ON / OFF` 三态；`AUTO` 只在“自检通过 + 白名单设备 + 稳定性门槛通过”时启用。
- 回退语义：单点失败只回退该点，不影响其他点；多点失败必须回到“默认基线”。
- 原因码：为每类失败定义稳定枚举（例如 `ACCEL_OCL_UNAVAILABLE`、`ACCEL_OCL_CRASH_GUARD`、`ACCEL_NNAPI_INIT_FAIL`、`ACCEL_DNN_BACKEND_UNSUPPORTED`），并在日志/报告里出现。

**验收指标（必须同时满足）**：
- 任意开关关闭后，行为与基线一致（功能与指标可对照）。
- 任意开关开启失败时，自动回落且输出原因码；不会出现“部分启用但口径不明”的灰区状态。

**退出条件**：
- 开关无法回滚（需要重装/改代码/改模型才能恢复）。
- 失败原因码不稳定或无法映射到可执行修复路径。

#### 阶段 2：低风险收益（CPU/内存/线程/流水线）

**目标**：在不引入新依赖、不改变推理后端的情况下，获得确定性收益（通常是延迟与抖动改善）。

**优先方向**：
- 帧处理背压与丢帧策略：避免“算法线程阻塞导致全链路抖动”。
- 复用 Buffer/减少拷贝：降低峰值内存与 GC/分配抖动。
- 热身与缓存：把模型加载/首次推理抖动从基准采样中隔离并记录。

**验收指标（必须同时满足）**：
- 稳定性不退化：72h soak（或至少 8h）无崩溃/无 OOM；相机异常恢复次数不超过基线。
- 性能有收益：关键链路 P95 延迟下降 ≥ 10%（或 CPU 降幅 ≥ 10%），且识别指标不下降（Top1 下降 ≤ 0.2% 或 0）。

**退出条件**：
- 任何“收益”以显著稳定性退化为代价（崩溃/泄漏/相机重启增多）。
- 识别口径漂移（阈值/预处理版本变更但未同步标注与报告）。

#### 阶段 3：通用 GPU 加速（OpenCL/UMat 等，可选且可回滚）

**目标**：将 OpenCL 作为“可选加速”，只在验证过的设备上通过 `AUTO` 进入默认配置，以换取可观收益并保持可回退。

**范围建议（先易后难）**：
- 先只加速预处理/resize/色彩转换等“数值稳定且可对照”的算子，再考虑更复杂的算子链。
- 需要明确“实际走 OpenCL 的算子覆盖率与命中率”，并记录回退次数与原因。

**验收指标（必须同时满足）**：
- Qualcomm 与 RK3288 各至少 1 台设备上：P95 延迟下降 ≥ 15% 或 CPU 降幅 ≥ 15%，且崩溃/异常率不高于基线。
- 任意 OpenCL 相关异常（初始化失败、运行时错误、疑似驱动问题）必须触发自动降级到 CPU，并在报告中可见（计数+原因码）。

**退出条件**：
- OpenCL 使得稳定性退化（哪怕收益显著也禁止进入默认配置，只能停留在“可选试验开关”）。
- 命中率不可控（大量算子仍走 CPU，收益不稳定且无法解释）。

#### 阶段 4：Qualcomm 专用加速（NNAPI / QNN / DSP，按项目边界选择）

**目标**：在 Qualcomm 设备上获得“可观且可解释”的推理收益，并保持与 RK3288 的基线可比。

**策略**：
- 任何 Qualcomm 专用运行时必须作为“插件式后端”引入，不得破坏 RK3288 的基线闭环。
- 只允许在 `AUTO` 且满足门槛时进入默认配置；否则一律回到 `ncnn (CPU)` 或 `OpenCV DNN (CPU)`。

**验收指标（必须同时满足）**：
- Qualcomm 设备：推理 P95 延迟下降 ≥ 25%（或吞吐提升 ≥ 25%），且温升/降频不导致 30 分钟后退化。
- RK3288 不受影响：同一版本的基线仍可跑通并产出可对照报告。
- 失败原因可解释：初始化失败/算子不支持/精度漂移/委托回退等都有原因码与建议修复路径。

**退出条件**：
- 引入新依赖导致构建/许可/合规无法闭环（例如许可证不可审计、二进制来源不清）。
- 精度/阈值漂移无法被量化与回滚（“变快但不准”）。

#### 阶段 5：默认配置优化与发布（把“已验证收益”推入默认）

**目标**：把已通过门槛的加速点以“白名单 + 自检 + 失败回退”的方式进入默认配置，同时保持一键回滚能力。

**验收指标（必须同时满足）**：
- 默认配置在 RK3288 与 Qualcomm 上均不低于基线稳定性（崩溃/泄漏/相机重启不增加）。
- 默认配置至少在 1 类设备上带来可观收益（按 6.9.3 的门槛定义）。

**退出条件**：
- 无法做到“默认可观收益”且不退化稳定性：则默认应保持基线，仅允许用户手动开启加速。

### 6.9.3 Go/No-Go 门槛（默认门槛，建议写死在发布门禁）

门槛分为“全局红线”和“阶段绿线”。红线用于保护稳定性与可回滚性；绿线用于判断“值得进入默认配置”。

**全局红线（任一触发即 No-Go）**：
- 无法回滚：任何加速开关关闭后不能恢复到基线行为与指标口径。
- 稳定性退化：崩溃/ANR/相机不可恢复错误率高于基线（哪怕性能提升也 No-Go）。
- 失败不可解释：出现“启用后偶发失败但无原因码/无证据落盘”的情况。
- 精度不可控：识别指标退化超过阈值，或阈值/预处理版本变更未同步报告与标注。

**默认绿线（满足才允许进入默认配置）**：
- RK3288：关键链路 P95 延迟下降 ≥ 10% 或 CPU 降幅 ≥ 10%，且 8h+ 稳定性不退化。
- Qualcomm：关键链路 P95 延迟下降 ≥ 20% 或吞吐提升 ≥ 20%，且 30 分钟持续运行无明显退化（温升/降频造成的回落需记录）。
- 回退与原因：加速失败时能回落到基线，并统计回退次数与原因码（回退不应导致业务不可用）。

### 6.9.4 评审签署流程（从提案到默认上线）

建议将评审拆成四道门：设计门、实现门、性能门、发布门，确保“先证据、后默认、可回滚”。

1) 方案提案评审（Design Review，必须）
- 输入：加速点列表、开关/回退设计、失败原因码表、测试矩阵（RK3288+Qualcomm）、风险清单与回滚方案。
- 输出：批准进入开发（或退回补齐口径/证据链）。

2) 实现评审（PR Review，必须）
- 输入：代码变更、单测/集测、日志与指标字段变更说明、对旧口径兼容策略。
- 输出：允许合并到 `master`（或要求补齐测试与文档）。

3) 性能与稳定性评审（Perf/Soak Review，必须）
- 输入：基线对比报告（P50/P95/P99、CPU/RAM、回退计数、错误原因分布）、至少 1 台 RK3288 + 1 台 Qualcomm 的实测证据。
- 输出：是否允许进入 `AUTO` 默认白名单（或仅保留为实验开关）。

4) 发布 Go/No-Go 评审（Release Readiness，必须）
- 输入：发布说明（build_id、配置变更、默认开关状态）、回滚演练记录、已知问题与监控告警阈值。
- 输出：签署上线范围（灰度比例/白名单设备/回滚触发条件）。

### 6.9.5 利益相关方清单与必须提交的评审材料

**利益相关方（建议最小集合）**：
- 平台负责人：RK3288 设备负责人、Qualcomm 设备负责人（各 1 名）
- 算法负责人：模型/阈值/精度口径签署
- 客户端负责人：Android/Native 负责人（开关/回退/稳定性签署）
- 测试负责人：测试矩阵、稳定性与回归口径签署
- 安全与合规负责人：第三方依赖、许可证、数据最小化与日志脱敏签署
- 发布负责人：灰度/回滚/应急预案签署

**必须提交的评审材料（缺一不可）**：
- 《加速点决策表》：每个开关的默认状态（AUTO/ON/OFF）、可用设备白名单、回退条件、失败原因码示例与修复路径。
- 《基线对比报告》：至少包含 RK3288 与 Qualcomm 各 1 台的基线 vs 加速对比（原始数据 + 汇总结论）。
- 《稳定性/Soak 报告》：运行时长、崩溃/ANR、内存/句柄趋势、相机重启次数与恢复情况。
- 《回滚方案与演练记录》：如何一键回到基线、回滚触发阈值、演练截图/日志（敏感信息打码）。
- 《风险清单与已知问题》：驱动/机型差异、精度漂移风险、资源占用风险，以及对应的监控与缓解策略。
- 《依赖与许可证更新》：任何新增第三方依赖必须同步更新 `CREDITS.md`，并说明来源与许可证。

---

## 7. 高级主题与故障排查 (Advanced & Troubleshooting)

### 7.1 性能优化与安全加固检查点
- [ ] **安全加固**: 检查构建日志是否包含 `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-Wl,-z,relro,-z,now`。
- [ ] **内存泄露**: 使用 AddressSanitizer (`-fsanitize=address`) 运行测试。
- [ ] **热点分析**: 使用 `perf` 或 `simpleperf` 分析 CPU 占用。
- [ ] **GPU 利用率**: 确保 OpenCV 开启了 OpenCL (`cv::ocl::setUseOpenCL(true)`)。

### 7.2 常见问题 (FAQ)
*   **Q: 为什么 V4L2 采集到的颜色不对？**
    *   A: 检查 `pixelformat` 设置 (YUYV vs MJPEG)，以及 YUV 转 RGB 的公式是否匹配。
*   **Q: ncnn/OpenCV DNN 初始化失败？**
    *   A: 优先检查模型文件路径是否正确、模型格式是否匹配（ONNX / ncnn param+bin），以及输入/输出 blob 名称是否与模型一致；若为 Android 端，请确认 ABI 与依赖库齐全。
*   **Q: DRM 显示黑屏？**
    *   A: 确保没有其他显示服务 (如 X11, Wayland, SurfaceFlinger) 占用 `/dev/dri/card0`。

### 7.3 变更日志 (Changelog)
详见 [CHANGELOG.md](CHANGELOG.md)。

---

## 8. 验收标准 (Acceptance Criteria)

---

## 9. 图表索引

### 9.1 图索引
- [图 3-1 系统分层架构](#fig-3-1)
- [图 5-1 CameraX 生命周期与资源边界](#fig-5-1)
- [图 5-2 CameraX 打开-预览-分析-释放](#fig-5-2)
- [图 5-3 CameraX 拍照（ImageCapture）完整时序](#fig-5-3)
- [图 5-4 CameraX 录像（VideoCapture + Recorder）完整时序](#fig-5-4)

### 9.2 表索引
- [表 1-1 术语对照表](#tbl-1-1)
- [表 2-1 技术栈选型对照表](#tbl-2-1)
- [表 4-1 V4L2 采集流程](#tbl-4-1)
- [表 4-2 RK MPP 解码流程](#tbl-4-2)
- [表 4-3 ncnn / OpenCV DNN 推理闭环](#tbl-4-3)
- [表 4-4 DRM/KMS 双缓冲显示流程](#tbl-4-4)
- [表 4-5 CameraX + Native 分析流程](#tbl-4-5)
- [表 5-1 摄像头 API/库对比表](#tbl-5-1)
- [表 5-2 常见第三方封装库对比](#tbl-5-2)
- [表 5-3 Fotoapparat 与 CAMKit 对比（工程审计口径）](#tbl-5-3)
- [表 5-4 Android 13+ 权限与后台限制对照](#tbl-5-4)
- [表 5-5 常见拍照/录像输出格式](#tbl-5-5)
- [表 5-6 广角/长焦/TOF/红外：枚举方法与启发式判定（工程口径）](#tbl-5-6)
- [表 5-7 常用“能力查询”字段速查（闪光/对焦/曝光补偿等）](#tbl-5-7)
- [表 6-1 离线端侧与云端方案对比表](#tbl-6-1)
- [表 6-2 基准结果记录格式（CSV 字段定义）](#tbl-6-2)
- [表 6-3 活体检测路线对比](#tbl-6-3)
- [表 6-4 集成清单与交付物](#tbl-6-4)
- [表 6-5 PAD 指标字段与解释](#tbl-6-5)
- [表 6-6 人脸方案对比（ML Kit / MediaPipe / ArcFace / Dlib / 百度 / 优图）](#tbl-6-6)
- [表 6-7 云厂商对比（阿里 / AWS / Azure）](#tbl-6-7)
- [表 6-8 特征维度/模板大小/阈值/延迟对比表（统一口径模板，需以实测填充）](#tbl-6-8)
- [表 6-9 特征加密文件格式（建议固定，便于兼容与迁移）](#tbl-6-9)
- [表 6-10 特征加密存储异常映射与处理策略（建议直接照表实现）](#tbl-6-10)
- [表 6-11 CI 门禁阈值（建议默认值，可按产品定义调整）](#tbl-6-11)

---

## 10. 风险清单

| 风险 | 触发条件 | 影响 | 缓解策略 |
| :--- | :--- | :--- | :--- |
| 摄像头适配碎片化 | 设备 HAL 差异、硬件级别不足 | 预览/拍照不可用或质量差 | 优先 CameraX，必要时降级/黑名单；产出设备能力报告 |
| 权限与后台限制 | 用户拒绝/永久拒绝、Android 10+ 后台限制 | 无法打开摄像头或异常释放 | 权限 Gate 模板 + 设置页回流处理；onStop 强制释放 |
| 泄漏与内存上涨 | 未关闭 ImageProxy、未清理 Analyzer | OOM、抖动、帧阻塞 | 背压策略 + 强制 close；基准脚本持续压测 |
| 人脸识别口径漂移 | 阈值/数据集/统计口径不一致 | 指标不可比，验收争议 | 固化字段定义与记录格式；版本化数据集与配置 |
| 合规风险 | 生物特征采集/传输/存储无说明 | 合规审计失败 | 端侧优先、最小化原则、加密存储；第三方许可证入库更新 CREDITS.md |

---

## 11. 参考文献

1. Android Developers: CameraX 文档  
   https://developer.android.com/media/camera/camerax
2. Android Developers: Camera2 API  
   https://developer.android.com/reference/android/hardware/camera2/package-summary
3. Android Developers: 请求应用权限（运行时权限）  
   https://developer.android.com/training/permissions/requesting
4. Android Developers: 后台执行限制（与相机后台限制相关）  
   https://developer.android.com/about/versions/oreo/background
5. ISO/IEC 30107-3:2017 Biometric presentation attack detection（PAD）测试与评估  
   (需至 ISO 官网搜索标准号查阅)
6. Android Developers: CameraX VideoCapture（录像管线）  
   https://developer.android.com/media/camera/camerax/video-capture
7. Android Developers: Android 13 行为变更（权限/通知等）  
   https://developer.android.com/about/versions/13/behavior-changes-13
8. Android Developers: Android 14 行为变更（前台服务类型等）  
   https://developer.android.com/about/versions/14/behavior-changes-14
9. Android Developers: 前台服务概览（Foreground Services）  
   https://developer.android.com/develop/background-work/services/foreground-services
10. Android Developers: MediaStore（媒体保存与读取）  
    https://developer.android.com/training/data-storage/shared/media
11. Android Developers: CameraCharacteristics（能力枚举关键字段）  
    https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics
12. NIST FRVT（人脸识别基准评测项目，指标口径参考）  
    https://www.nist.gov/programs-projects/face-recognition-vendor-test-frvt
13. Google ML Kit: Face Detection  
    https://developers.google.com/ml-kit/vision/face-detection
14. MediaPipe Tasks: Vision（Face Detector / Face Landmarker）  
    https://ai.google.dev/edge/mediapipe/solutions/guide#vision
15. ArcSoft（虹软）人脸识别 SDK（ArcFace）  
    （访问受限，请在搜索引擎中查找“虹软 ArcFace 官网”）
16. 百度智能云：人脸识别  
    https://ai.baidu.com/tech/face
17. 腾讯云：人脸识别/优图（按产品线）  
    https://cloud.tencent.com/product/facerecognition
18. Dlib 官方项目  
    https://dlib.net/
19. 阿里云：视觉智能开放平台（Facebody）  
    https://help.aliyun.com/product/135949.html
20. AWS Rekognition  
    https://aws.amazon.com/rekognition/
21. Azure AI Face  
    https://learn.microsoft.com/en-us/azure/ai-services/face/overview-identity
22. Deng, J. et al.: ArcFace: Additive Angular Margin Loss for Deep Face Recognition (CVPR 2019)  
    https://arxiv.org/abs/1801.07698
23. InsightFace（开源实现集合，ArcFace 等）  
    https://github.com/deepinsight/insightface
24. Android Developers: Logical multi-camera（Camera2 多摄与 physicalCameraIds 相关）  
    https://developer.android.com/reference/android/hardware/camera2/CameraCharacteristics#physicalCameraIds
25. Android Developers: 深度输出与相关格式（DEPTH16/DEPTH_POINT_CLOUD）  
    https://developer.android.com/reference/android/graphics/ImageFormat
26. Android Developers: CaptureRequest（曝光/对焦/闪光等请求字段）  
    https://developer.android.com/reference/android/hardware/camera2/CaptureRequest
27. Android Developers: CameraMetadata（常量与枚举值定义）  
    https://developer.android.com/reference/android/hardware/camera2/CameraMetadata
28. ISO/IEC 19794-5: Face image data（人脸图像与模板相关数据交换标准，按需对齐）  
    (需至 ISO 官网搜索标准号查阅)
29. Android Developers: Android Keystore 系统（密钥生成与存储）  
    https://developer.android.com/privacy-and-security/keystore
30. Android Developers: KeyGenParameterSpec（Keystore 对称密钥参数）  
    https://developer.android.com/reference/android/security/keystore/KeyGenParameterSpec
31. Android Developers: Cipher / AES-GCM（JCA API 与使用约束）  
    https://developer.android.com/reference/javax/crypto/Cipher
32. Robolectric 官方文档  
    https://robolectric.org/
33. AndroidX Test: Espresso  
    https://developer.android.com/training/testing/espresso
34. AndroidX CameraX: Testing（camera-testing）  
    https://developer.android.com/media/camera/camerax/architecture
35. GitHub Actions: Workflow syntax for GitHub Actions  
    https://docs.github.com/actions/writing-workflows/workflow-syntax-for-github-actions
36. reactivecircus/android-emulator-runner（GitHub Actions Android 模拟器运行器）  
    https://github.com/ReactiveCircus/android-emulator-runner

---
