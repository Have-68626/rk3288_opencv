# RK3288 机器视觉引擎 (AI Engine)

![Platform](https://img.shields.io/badge/Platform-RK3288%20%7C%20ARMv8%20%7C%20x86-blue)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-green)
![OpenCV](https://img.shields.io/badge/OpenCV-4.10.0-orange)

## 📖 项目简介

本项目是一个专为 **Rockchip RK3288** 平台（Cortex-A17 架构 + Mali-T764 GPU）深度优化的嵌入式机器视觉应用，同时兼容 **ARMv8 (arm64-v8a)** 和 **x86_64** 架构。

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

## ⚠️ 日志免责声明 (Disclaimer)

本项目为个人学习与研究用途。默认日志策略在 `DEBUG` 或 `VERBOSE` 级别下可能会输出包含内存地址、线程 ID、请求/响应明文等调试信息。

**严禁将本项目直接用于生产环境或处理敏感数据**，除非您已自行对日志输出逻辑进行脱敏处理。使用者需自行承担因日志泄露导致的安全风险。

## 📚 开发文档

为保障项目的可维护性与可持续性，我们提供了详细的 **[程序开发设计书 (DEVELOP.md)](DEVELOP.md)**，其中包含：
*   系统架构图解
*   核心模块详细设计
*   数据流转逻辑
*   扩展与维护指南

## 📊 性能指标

| 指标 | 目标值 | 实测表现 (预估) |
| :--- | :--- | :--- |
| **内存占用** | < 512 MB | ~80-120 MB |
| **CPU 使用率** | < 60% | ~25-40% (运动模式) |
| **视频延迟** | < 300 ms | ~150 ms |
| **启动时间** | < 2 s | < 1 s |

## 待办列表 (Todo List)

> ✅ 表示已完成

1.  ✅ **非工控机安卓设备兼容性优化**
    *   支持 Android API 21–34（minSdk=21，targetSdk=34）
    *   在清单中将相机/USB Host 等硬件特性声明为可选（required=false），避免部分设备安装失败
    *   当关键运行时权限缺失时进入安全模式（SAFE MODE），阻断监控与引擎初始化并给出提示
    *   适配 Android 13+ 媒体权限（READ_MEDIA_IMAGES/READ_MEDIA_VIDEO）与旧版分区存储差异

2.  ✅ **必要权限申请与检查机制优化**
    *   最小必要权限门控：相机（CAMERA）、麦克风（RECORD_AUDIO）、媒体读取（Android 13+）/外部存储读取（旧版）
    *   统一权限说明（rationale）对话框：用户拒绝后进入安全模式，允许继续浏览界面与日志
    *   支持“永久拒绝”场景：引导跳转系统设置页手动授权
    *   在监控启动入口与引擎初始化入口执行权限缺失检查，缺失时直接阻断敏感调用
    *   悬浮窗（在其他应用上层显示）权限与运行时权限分离处理：仅在开启悬浮窗功能时引导授权

3.  ✅ **日志输出能力优化与日志导出功能**
    *   **3.1 日志格式、命名与落盘**
        *   会话级文件名：`rk3288_yyyyMMdd_HHmmss.log`
        *   Java（AppLog）与 Native（NativeLog）写入同一会话文件
        *   双路径落盘：内部 `files/logs/` + 外部 `Android/data/<package>/logs/`（可用时）
        *   启动时清理旧会话：超过 7 天或超过 20 个的日志文件将被删除
        *   单文件超过 5MB 自动滚动：生成 `.1`–`.9` 备份文件
    *   **3.2 日志查看与导出**
        *   日志列表在后台线程加载并按修改时间倒序显示，支持通过勾选框进行多选
        *   详情页预览最多 100KB，超出部分截断，并对敏感信息执行脱敏展示
        *   导出使用 Storage Access Framework 创建目标 ZIP 文件：`logs_yyyyMMdd_HHmmss.zip`
        *   导出时为每个文件计算 CRC32，并写入 ZIP 内的 `manifest.txt`
    *   **3.3 隐私**
        *   敏感字段（身份证号、手机号、GPS 坐标）统一脱敏为 `***`，脱敏逻辑封装为 `SensitiveDataUtil`

4.  ✅ **摄像机检测能力优化**
    *   启动时通过 `CameraManager.getCameraIdList()` 枚举摄像头并展示镜头朝向信息（前置/后置/外接）
    *   在主界面 Spinner 中支持手动切换摄像头，并将 Camera ID 持久化到 `SharedPreferences`
    *   监听 USB 设备插拔广播，插拔时刷新摄像头列表并提示用户
    *   提供两类 Mock 入口：系统相机拍照回传（System Camera Mock）与文件选择（Mock Source）

5.  ✅ **应用状态栏显示功能优化**
    *   通过 `StatusService` 在屏幕顶部显示悬浮层（Android O+ 使用 `TYPE_APPLICATION_OVERLAY`，需悬浮窗权限）
    *   悬浮层实时显示 FPS、CPU、MEM 三项指标，刷新频率 500ms
    *   FPS 基于 `Choreographer`；MEM 基于 `Debug.MemoryInfo`；CPU 优先读取 `/proc`，失败时自动回退到标准 API 计算应用 CPU 使用率
    *   主界面提供悬浮窗开关，关闭时停止 `StatusService` 并移除悬浮层

6.  ✅ **调试入口维护**
    *   持续维护 Native CLI 调试入口（`main.cpp` / `rk3288_cli`）：支持命令行参数传入 `cameraId`，并可选传入 `cascadePath` 与 `storagePath`，用于脱离 Android UI 进行算法验证与性能分析。


8.  ✅ **[P0] FFMPEG 增强 Mock 模式与实时监控**
    *   **技术约束**
        *   引入移动版 FFMPEG v6.0（LGPL 版）
        *   Mock 模式支持本地 MP4、HLS、RTSP 三种协议作为虚拟视频源
        *   真实监控场景：将相机输出 NV21 通过 FFMPEG 实时编码为 H.264/AAC，并以 RTMP 推流到服务端，同时提供 1 路本地回显 Surface
        *   提供 `arm64-v8a` 与 `armeabi-v7a` 双架构 `.so`，并支持灰度压缩策略
    *   **当前进度**
        *   已支持在设置中输入 Mock URL（MP4/HLS/RTSP）作为虚拟视频源
        *   已提供 RTMP 推流入口（从 Mock 源执行推流命令）；FFmpeg 以可选 AAR 方式集成：放置 `app/libs/ffmpeg-kit.aar`
        *   Mock 模式已支持无限循环播放 (`-stream_loop -1`)
    *   **验收标准**
        *   Mock 模式可循环播放 1080p@30fps 视频无花屏
        *   实时监控端到端延迟 ≤ 400 ms（局域网）
        *   APK 增量 ≤ 12 MB

9.  ✅ **[P1] 主界面横竖屏自适应**
    *   **技术约束**
        *   使用 Jetpack WindowManager + ConstraintLayout 提供横竖屏两套布局资源
        *   竖屏：底部 5 个功能按钮（监控、回放、设置、日志、关于）+ 顶部状态栏；横屏：左侧抽屉式导航 + 右侧监控区域占 70% 宽度
        *   AndroidManifest 声明 `configChanges="orientation|screenSize|screenLayout"`，Activity 内重写 `onConfigurationChanged`，禁止重启
        *   切换动画 300 ms 线性插值
    *   **验收标准**
        *   旋转 200 次无内存泄漏（Profiler 检测）
        *   切换过程无黑闪
        *   支持 180° 反向横屏

10. ✅ **[P1] 监控区域媒体自适应**
    *   **技术约束**
        *   SurfaceView / TextureView 使用动态 LayoutParams 自适应：分辨率 ≥ 16:9 按宽度铺满等比缩放；< 16:9 按高度铺满等比缩放
        *   支持双击放大到全屏，再次双击恢复
        *   边缘暗角、水印、时间戳 OSD 通过 OpenGL 着色器叠加，且帧率不降低
        *   引入 `video_wrapper` 容器确保 OSD 与视频画面始终对齐
    *   **验收标准**
        *   4K 视频下 GPU 占用 ≤ 25%（Adreno 530 测试）
        *   缩放等级切换耗时 ≤ 150 ms
        *   无绿边、拉伸、裁剪异常

11. ✅ **[P2] “Recognition Events” 面板手动显隐**
    *   **技术约束**
        *   监控区域下方新增高度 180dp 的 RecyclerView 面板，默认可见
        *   提供全局悬浮按钮（FAB）切换显示状态，状态持久化到 SharedPreferences
        *   隐藏时高度动画收缩至 0，同时监控区域等速扩展
    *   **验收标准**
        *   动画 60fps 无掉帧
        *   状态在应用重启后保持
        *   无障碍朗读正确

12. ✅ **[P1] UI 文案一致性走查**
    *   **技术约束**
        *   遍历所有 layout、`strings.xml`、Jetpack Compose 文本节点，建立“文案-功能”映射表并与 PRD 做 diff
        *   统一中英文标点、空格、大小写；动态拼接字符串使用 Android plural
        *   输出走查报告（Excel）并附加截图前后对比
    *   **验收标准**
        *   零 P0 文案错误上线
        *   走查报告需产品经理二次确认签字

13. ✅ **[P1] 退出应用时自动清理缓存**
    *   **技术约束**
        *   仅清理应用缓存目录（`getCacheDir()` 与 `getExternalCacheDir()`），不得删除日志目录（`files/logs/` 与 `Android/data/<package>/logs/`）
        *   清理由主线程触发、后台线程执行，不阻塞退出流程
    *   **验收标准**
        *   退出应用后缓存目录文件数归零（允许系统保留的占位文件除外）
        *   `Android/data/<package>/logs/` 下日志与 `security.log` 不被清理
        *   清理结果在日志中可追溯（deletedFiles/deletedBytes/errors）

14. ✅ **[P1] 系统稳定性与性能优化 (New)**
    *   **技术约束**
        *   **日志逻辑修复**: 区分“进度通知”与“错误警告”，消除误导性的 "SYSTEM NOT READY" 日志。
        *   **UI 自适应增强**: 弃用 16:9 硬编码阈值，改为基于容器实际宽高比的 Letterbox/Pillarbox 自适应算法。
        *   **OpenCV 优化**: Native 层启用 NEON 指令集加速，并重构 `VideoManager` 减少 `Mat` 内存拷贝。
        *   **权限容错**: 修复 `/proc/stat` 访问权限拒绝导致的 CPU 采样异常。
    *   **验收标准**
        *   日志中不再出现“引擎初始化成功”被标记为错误的情况。
        *   非 16:9 屏幕下监控画面无裁剪、无拉伸。
        *   1080p 视频流 CPU 占用率降低 10%。

## 📄 许可证
MIT License
