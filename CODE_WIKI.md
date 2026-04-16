# RK3288 机器视觉引擎 (AI Engine) - Code Wiki

## 1. 项目整体架构 (Project Architecture)

本项目是一个跨平台的边缘机器视觉应用，专为资源受限的设备（如 RK3288 ARM 平台）设计，同时支持 Windows x86_64 环境。系统主要包含以下四大子系统：

1. **C++ 通用核心与算法 (Core C++)**：位于 `src/cpp/`。包含底层的视频帧处理、目标检测、人脸识别（YOLO + ArcFace）、运动检测和事件总线。这部分被编译为动态链接库（Android JNI）或独立可执行文件（CLI 工具）。
2. **Android 客户端 (Android App)**：位于 `app/` 和 `src/java/`。提供带图形界面的 Android 应用，负责处理 Camera2/CameraX 摄像头采集、UI 渲染、权限管理，并通过 JNI 调用 C++ 核心进行推理。
3. **Windows 本地服务与识别 (Windows Native)**：位于 `src/win/`。使用 Media Foundation (MF) 进行摄像头采集，集成 CivetWeb 提供本地 HTTP API 与静态资源托管，实现基于 OpenCV DNN 和 LBPH 的人脸识别。
4. **Web SPA 前端 (Web SPA)**：位于 `web/`。基于 React + Vite + TypeScript 构建，通过 HTTP API 与 Windows 本地服务通信，提供无客户端界面的控制台，包括设置台、设备预览、识别记录等。

---

## 2. 主要模块职责 (Module Responsibilities)

### 2.1 C++ 核心引擎模块 (`src/cpp/`)
- **引擎编排**：`Engine` 负责生命周期管理、视频流获取、推理管线调度及事件分发。
- **人脸推理管线**：`FaceInferencePipeline` 串联了人脸检测、关键点提取、人脸对齐和特征提取（Embedding）全过程。
- **算法模型封装**：
  - `YoloFaceDetector`：使用 YOLO 进行人脸与关键点检测，支持 OpenCV DNN 和 NCNN 后端。
  - `ArcFaceEmbedder`：使用 ArcFace 提取人脸高维特征向量。
- **比对与存储**：`FaceSearch` 负责基于余弦相似度或 L2 距离的人脸特征 1:N 检索；`Storage` 负责数据持久化。
- **JNI 桥接**：`native-lib.cpp` 导出 JNI 方法供 Android Java 层调用。

### 2.2 Android 应用模块 (`src/java/`)
- **采集控制**：`Camera2CaptureController` 和 `CameraXCaptureController` 负责不同 API 层级的摄像头管理，并将 YUV 数据推入 C++ 层。
- **主界面与渲染**：`MainActivity` 是应用主入口，负责处理权限、启动监控引擎，并通过 Surface 或 Bitmap 接收 C++ 回传的渲染帧。
- **数据安全**：`FeatureTemplateEncryptedStore` 和 `SensitiveDataUtil` 负责 Android Keystore 加密的人脸模板持久化。

### 2.3 Windows 服务模块 (`src/win/`)
- **HTTP 服务**：`HttpFacesServer` 基于 CivetWeb，对外暴露 `/api/v1/*` 的 RESTful 接口（用于配置管理、人员注册、库清空等），并托管 Web SPA 静态资源。
- **视觉管线**：`FramePipeline` 串联起从 `MfCamera` 获取帧数据、人脸检测（`DnnSsdFaceDetector`）、特征提取与识别（`FaceRecognizer`、`LbphEmbedder`）。
- **安全与配置**：`WinCrypto` 提供基于 DPAPI 的配置加密；`WinJsonConfig` 提供 JSON 格式的配置读写与热重载。

### 2.4 Web 前端模块 (`web/`)
- **状态管理**：基于 `AppStore.tsx` 管理应用偏好与后端服务器配置状态。
- **页面路由**：
  - `HomePage.tsx`：仪表盘与服务健康状态概览。
  - `PreviewPage.tsx`：实时监控画面展示、人脸注册、数据库清空入口。
  - `SettingsPage.tsx`：摄像头参数、检测阈值、系统设置的管理界面。

---

## 3. 关键类与函数说明 (Key Classes & Functions)

### 3.1 C++ Core (`src/cpp/include/`)
- `Engine::initialize(cameraId, cascadePath, storagePath)`：初始化引擎及依赖的所有子模块（人脸、运动检测等）。
- `Engine::pushExternalFrame(ExternalFrame)`：接收来自 Android JNI 或其他外部模块推入的视频帧。
- `FaceInferencePipeline::process(...)`：执行一次完整的人脸推理，返回对齐后的人脸特征向量。
- `FaceSearch::searchTopK(...)`：在高维特征库中检索最相似的人脸实体。

### 3.2 Android Java (`src/java/com/example/rk3288_opencv/`)
- `MainActivity.java`：
  - `initEngine()`：调用 JNI 接口 `nativeInit` 或 `nativeInitFile` 启动 C++ 层引擎。
  - `startMonitoring()`：开启 CameraX/Camera2 采集流。
- `Camera2CaptureController.pushExternalYuv420888(...)`：将 Java 层的 YUV Buffer 转换为底层能消费的格式并传入 JNI。

### 3.3 Windows C++ (`src/win/include/rk_win/`)
- `HttpFacesServer::start(pipe, events, port, settings)`：在指定端口（默认仅 127.0.0.1）拉起服务，接管 API 和静态路由。
- `FaceDatabase::enrollFace(...)`：将新提取的人脸特征（均值池化）持久化注册到本地。
- `WinCrypto::encryptData(...)` / `decryptData(...)`：使用 Windows DPAPI 对敏感配置数据进行加解密。

---

## 4. 依赖关系 (Dependencies)

### 4.1 系统与环境库
- **CMake (>=3.18.1)**：核心与 Windows 端构建系统。
- **Android NDK (推荐 r23c)**：用于编译 `native-lib.so`。
- **Node.js & pnpm**：用于 Web SPA 的依赖安装与构建。

### 4.2 第三方核心依赖
- **OpenCV (4.10.0)**：跨平台基础视觉库，提供矩阵运算、DNN 推理、图像编解码及经典 CV 算法支持。
- **ncnn (可选)**：腾讯开源的高性能神经网络前向计算框架，用于 ARM (RK3288) 端侧加速。
- **libyuv (可选)**：用于高效的 YUV 到 BGR 色彩空间转换。
- **CivetWeb**：轻量级 C/C++ 嵌入式 Web 服务器，用于 Windows HTTP 服务。
- **React 18 & Ant Design 5**：Web SPA 前端核心框架。

---

## 5. 项目运行与构建方式 (How to Run & Build)

### 5.1 构建 C++ Core / Windows 服务 (CMake)
需要配置好 `OPENCV_ROOT` 环境变量或 CMake 参数。
```powershell
# 1. 生成项目 (Windows 示例)
cmake -S . -B build_win -G "Visual Studio 17 2022" -A x64 -DOPENCV_ROOT="C:\path\to\opencv"

# 2. 编译 Windows 本地服务
cmake --build build_win --config Release --target win_local_service

# 3. 运行本地服务 (默认运行在 http://127.0.0.1:8080)
.\build_win\Release\win_local_service.exe
```

### 5.2 构建 Android 应用 (Gradle)
```bash
# 编译 Debug APK 并进行测试与静态检查
./gradlew :app:assembleDebug :app:testDebugUnitTest :app:lintDebug

# 安装到连接的 Android 设备
./gradlew :app:installDebug
```

### 5.3 构建 Web SPA 前端 (Vite/pnpm)
构建产物会自动输出到 `src/win/app/webroot`，以供 Windows 服务静态托管。
```bash
# 进入 web 目录
cd web

# 安装依赖
pnpm install

# 启动本地开发服务器 (支持热更新)
pnpm run dev

# 生产环境构建
pnpm run build
```

### 5.4 性能基准测试与验证
本项目包含丰富的 CLI 基准测试工具：
- **C++ 核心基准**：`./build_win/Release/inference_bench_cli.exe`（支持 `--use-opencl`）。
- **人脸检测基准**：`rk3288_cli --face-baseline ...`，用于生成 P50/P95 性能报告并落盘到 `tests/reports/face_baseline/`。