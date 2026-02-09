# RK3288 AI Engine - 程序开发设计书

**版本**: 1.2
**日期**: 2026-02-09
**状态**: 正式版

---

## 1. 系统概览 (System Overview)

### 1.1 背景与目标
本项目旨在为 **Rockchip RK3288** 平台（ARM Cortex-A17, Mali-T764）提供一套高性能、低资源的机器视觉解决方案，同时兼容更广泛的 Android 设备。**RK3288 为 32 位平台，实际部署需使用 `armeabi-v7a` ABI**。目标硬件通常运行 Android 5.1/7.1/10+，资源受限。
本系统的核心设计目标是：**在有限的算力下实现实时的视频监控与生物识别**。

### 1.2 GitHub 映射 (GitHub Mapping)
根据项目规范，以下目录与文件纳入版本控制：
- **程序本体**:
    - `app/` (Android 源码)
    - `CMakeLists.txt` (构建脚本)
    - `gradle/` (构建工具)
    - `build.gradle` (构建配置)
- **文档**:
    - `README.md`
    - `DEVELOP.md`
    - `CREDITS.md`
    - `TEST_REPORT.md`
- **排除项**:
    - `ErrorLog/` (本地运行日志，不上传)
    - `.cxx/`, `build/` (编译中间产物)
    - `local.properties` (本地环境配置)

### 1.3 架构设计
系统采用分层架构设计，核心业务逻辑下沉至 Native C++ 层，以最大化利用硬件性能并减少 Java/Kotlin 层的 Overhead。

```mermaid
graph TD
    subgraph "Android Application Layer"
        UI[MainActivity (Java)] --> JNI[JNI Interface (native-lib.cpp)]
        Service[StatusService (Java)] --> WindowManager
        LogViewer[LogViewerActivity] --> Logs[Log Files]
    end

    subgraph "Native C++ Core Layer"
        JNI --> Engine[Engine (Central Controller)]
        CLI[main.cpp (CLI Entry)] --> Engine
        
        Engine --> VM[VideoManager]
        Engine --> MD[MotionDetector]
        Engine --> BA[BioAuth]
        Engine --> EM[EventManager]
        
        VM --> Camera[Camera Hardware]
        EM --> Storage[Storage / FileSystem]
    end
    
    subgraph "Infrastructure"
        Config[Config.h]
        Types[Types.h]
    end
```

### 1.3 核心流程
系统运行为一个无限循环的状态机，主要流程如下：
1.  **初始化 (Init)**: 加载配置，初始化摄像头，加载人脸识别模型。
2.  **采集 (Capture)**: `VideoManager` 获取最新视频帧。
3.  **处理 (Process)**:
    *   **运动检测**: `MotionDetector` 判断画面是否变化。
    *   **生物识别**: 若有运动，触发 `BioAuth` 进行人脸检测与识别。
4.  **响应 (Action)**:
    *   **日志记录**: 使用 `AppLog` 统一输出并落盘（内部+外部双路径，自动回滚）。
    *   若识别到未授权人员或异常运动，`EventManager` 记录事件并保存快照。
5.  **清理 (Cleanup)**: 定期清理过期数据。

---

## 2. 核心模块设计 (Core Module Design)

### 2.1 Engine (中控引擎)
*   **职责**: 系统的核心控制器，负责协调各模块工作，维护系统状态。
*   **关键类**: `Engine`
*   **主要方法**:
    *   `initialize()`: 资源分配。
    *   `run()`: 启动主循环线程。
    *   `stop()`: 优雅退出。
*   **设计模式**: 有限状态机 (FSM)。

### 2.2 VideoManager (视频管理)
*   **职责**: 封装 OpenCV `VideoCapture`，处理摄像头硬件兼容性问题。
*   **关键特性**:
    *   支持 V4L2 (Video for Linux 2) 后端。
    *   自动处理分辨率协商（默认 VGA 640x480 以平衡性能）。
    *   提供帧缓存管理，避免频繁内存分配。

### 2.3 MotionDetector (运动检测)
*   **职责**: 提供低功耗的初筛机制。
*   **算法**: 基于高斯模糊 + 帧差法 (Frame Differencing)。
*   **优化**:
    *   仅在灰度图上计算。
    *   设置动态阈值 `Config::MOTION_THRESHOLD`，避免噪点误报。

### 2.4 BioAuth (生物认证)
*   **职责**: 执行具体的计算机视觉任务。
*   **算法**:
    *   **检测**: Haar Cascade Classifier (快速，适合 CPU)。
    *   **识别**: LBPH (Local Binary Patterns Histograms) Face Recognizer。
*   **数据流**: 输入 `cv::Mat` 帧 -> 输出 `AuthResult` (用户ID, 置信度)。

### 2.5 EventManager & Storage (事件与存储)
*   **职责**: 数据的持久化与生命周期管理。
*   **存储结构**:
    ```text
    /sdcard/Android/data/com.example.rk3288_opencv/files/logs/  # 日志文件
    /sdcard/rk3288_data/
    ├── events/
    │   ├── 20231027_103001_event.json  # 事件元数据
    │   └── 20231027_103001_snap.jpg    # 现场快照
    └── models/
        └── haarcascade_...xml          # 模型文件
    ```
*   **自动维护**: 每次启动时调用 `cleanupOldData()` 删除 7 天前的文件。

---

## 3. 功能特性增强 (Feature Enhancements)

### 3.1 兼容性与权限 (Compatibility & Permissions)
*   **设备兼容**: 清单文件已优化，硬件特性（相机、USB Host）标记为可选 (`required="false"`)，支持安装在非工控机设备。
*   **权限管理**: 
    *   适配 Android 13+ Granular Media Permissions (`READ_MEDIA_IMAGES/VIDEO`).
    *   使用 `PermissionStateMachine` 统一申请与状态流转，权限不足时进入安全模式并阻断敏感调用。
    *   支持永久拒绝后的系统设置跳转引导（并保持应用可用）。

### 3.2 日志系统 (Logging System)
*   **架构**: `AppLog` + `FileLogSink`（基于 `android.util.Log` 封装），同时支持 Native/Java 落盘。
*   **策略**:
    *   **会话隔离**: 每次启动生成独立日志文件 `rk3288_yyyyMMdd_HHmmss.log`。
    *   **统一写入**: Java 与 Native 层共享同一会话文件名，确保日志上下文完整。
    *   **清理**: 启动时自动清理超过 7 天或总数超过 20 个的历史日志。
    *   **滚动**: 单文件 ≤ 5MB，支持 `.1`~`.9` 滚动。
*   **查看器**: `LogViewerActivity` 提供应用内日志查看、关键词高亮（待实现）及内容预览。
*   **导出**: 支持通过 SAF (Storage Access Framework) 导出日志为 ZIP 压缩包，包含 CRC32 校验。
*   **隐私**: `SensitiveDataUtil` 自动脱敏手机号、身份证、GPS 信息。
*   **落盘路径**:
    *   内部: `/data/data/<package>/files/logs/`
    *   外部: `/sdcard/Android/data/<package>/logs/`（若受限制则回退到 `/sdcard/Android/data/<package>/files/logs/`）

### 3.3 摄像机管理 (Camera Management)
*   **动态发现**: 启动时遍历 `CameraManager`，识别前置、后置及外接 USB 摄像头。
*   **热插拔**: 监听 USB 设备插拔广播，实时刷新可用摄像头列表。
*   **切换**: 提供 UI 下拉菜单动态切换输入源，并持久化用户选择。

### 3.4 状态监控 (Status Monitor)
*   **悬浮窗**: `StatusService` 实现全局悬浮窗（需 `SYSTEM_ALERT_WINDOW` 权限）。
*   **指标**: `StatsRepository` 统一采集 FPS/MEM/CPU（500ms 刷新，同源缓存，读写锁保护），状态栏与悬浮窗同源显示。
*   **失败策略**: 任一指标采集失败则 UI 显示 `--` 且写入错误日志（包含 errno/调用路径）。
*   **交互**: 可通过主界面开关开启/关闭；权限拒绝不退出应用。

### 3.5 Mock 相机调试模式 (Mock Camera Mode)
*   **目的**: 允许在无物理摄像头的环境（如模拟器或纯逻辑验证场景）下调试核心算法与 UI。
*   **入口**: 在摄像头下拉列表中选择 "Mock Source (Image/Video)"。
*   **机制**:
    *   通过系统文件选择器 (`Intent.ACTION_OPEN_DOCUMENT`) 选取图片 (`image/*`) 或视频 (`video/*`)。
    *   文件自动复制到应用私有缓存目录，避免 Content Provider 跨进程访问限制。
    *   **Native 实现**:
        *   **图片**: 使用 `cv::imread` 加载，Native 层无限循环返回该静态帧（模拟 30 FPS）。
        *   **视频**: 使用 `cv::VideoCapture` 加载，播放结束自动倒带 (`CAP_PROP_POS_FRAMES = 0`)，实现无缝循环。
*   **限制**: Mock 模式下部分 Camera 特有属性（如曝光控制）不可用。

---

## 4. 可维护性与可持续性指南

### 4.1 代码规范
*   **语言标准**: C++17。
*   **命名规范**:
    *   类名: `PascalCase` (e.g., `VideoManager`)
    *   方法/变量: `camelCase` (e.g., `processFrame`)
    *   常量: `UPPER_CASE` (e.g., `MAX_FRAME_RATE`)
    *   成员变量: 不使用 `m_` 前缀，通过 `this->` 或清晰命名区分。
*   **内存管理**: 严禁使用裸指针 (`new`/`delete`)，必须使用智能指针 (`std::unique_ptr`, `std::shared_ptr`) 管理资源，遵循 RAII 原则。

### 4.2 配置管理
所有全局配置项（如阈值、路径、超时时间）必须集中定义在 `Config.h` 中，严禁在业务逻辑中硬编码 "Magic Numbers"。

```cpp
// Good
if (diff > Config::MOTION_THRESHOLD) { ... }

// Bad
if (diff > 25.0) { ... }
```

### 4.3 扩展性设计
*   **添加新算法**:
    1.  在 `app/src/main/cpp/include` 创建新头文件（如 `ObjectDetector.h`）。
    2.  实现检测逻辑接口。
    3.  在 `Engine` 类中注册该模块。
    4.  在 `Engine::run()` 循环中添加调用逻辑。

### 4.4 日志与调试
*   Java: 使用 `AppLog` 统一格式 `【模块-函数-线程ID-时间戳】`。
*   Native: 使用 `NativeLog` 同步输出到 Logcat 与双路径文件。
*   Debug: `StrictMode` 检测磁盘 IO 违规并输出日志。

---

## 5. 构建与部署 (Build & Deployment)

详细构建步骤请参考 `README_BUILD.md`。

### 5.1 版本控制
*   **Git**: 忽略 `build/`, `.cxx/`, `.gradle/`, `local.properties` 等生成文件。
*   **依赖库**: OpenCV SDK 不直接包含在 Git 中，需通过环境变量 `OPENCV_DIR` 指定路径。

### 5.2 持续集成 (CI) 建议
*   未来可配置 GitHub Actions，在 Ubuntu 运行环境中使用 Android NDK 自动编译 `native-lib.so` 和可执行文件 `main`，确保代码在不同环境下的编译稳定性。

## 6. 项目变更日志 (Changelog)

### [2026-02-09] v1.1.0 功能增强更新
*   **兼容性**: 优化 AndroidManifest，适配非工控机设备与 Android 13+ 权限。
*   **日志**: 新增 `LogViewerActivity`，支持日志查看、导出 (ZIP) 与敏感信息脱敏。
*   **相机**: 新增多摄像头枚举与动态切换功能，支持 USB 热插拔监听。
*   **监控**: 新增 `StatusService` 悬浮窗，实时显示 FPS/CPU/MEM。
*   **调试**: 修复 `main.cpp` CLI 构建问题，支持命令行参数。

### [2026-02-09] v1.2.0 运行时稳定性与可观测性
*   **就绪门控**: `SYSTEM READY` 改为受控显示，需权限+引擎初始化+首帧到达才显示，否则强制隐藏并写日志。
*   **权限**: 引入 `PermissionStateMachine`，拒绝后进入安全模式并阻断敏感调用。
*   **日志**: 引入 `AppLog`/`FileLogSink`/`NativeLog` 双路径落盘回滚并启用 StrictMode。
*   **指标**: 引入 `StatsRepository` 500ms 同源采集与缓存，状态栏与悬浮窗一致。

### [2026-02-08] 构建配置升级
*   **构建目标**: 在 `CMakeLists.txt` 中新增了 `rk3288_cli` 目标，专门用于编译 `main.cpp` 为独立可执行文件，方便 CLI 调试。
*   **代码修复**: `main.cpp` 增加了 `<string>` 头文件以修复 `std::stoi` 编译错误。
*   **文件重命名**: 根据项目规范，将 `DEVELOPMENT_DESIGN.md` 重命名为 `DEVELOP.md`。

---

## 7. 待办列表 (Todo List)

按优先级排序，计划执行以下优化：

1.  **[已完成] 非工控机安卓设备兼容性优化**
    *   覆盖主流手机/平板芯片平台（高通、联发科、三星、展锐等）
    *   适配不同 Android 版本（API 21–34）的硬件抽象层差异
    *   在清单中声明所有动态特征模块，确保非工控机首次安装时按需下载
    *   对缺少工控机专用外设（GPIO、串口、CAN）的场景提供降级方案并给出用户提示
    *   完成 5 款以上非工控机真机冒烟测试，记录兼容性矩阵并输出报告

2.  **[已完成] 必要权限申请与检查机制优化**
    *   梳理最小必要权限列表（相机、存储、麦克风、定位、USB Host 等）
    *   实现一次性权限说明页，使用官方推荐「权限 rationale」弹窗文案
    *   在应用启动、功能入口、后台切换三个时机做权限缺失检查，拒绝时给出可跳转系统设置的一键修复按钮
    *   对 Android 13 及以上存储权限采用 Granular Media 权限模型，兼容分区存储
    *   编写单元测试与 UI 自动化测试，覆盖允许、拒绝、二次询问、永久拒绝全部场景

3.  **[已完成] 日志输出能力优化与日志导出功能**
    *   **3.1 日志格式与命名**
        *   统一采用 `AppLog`/`NativeLog` 输出格式：`【模块-函数-线程ID-时间戳】`
        *   统一落盘文件名：`rk3288.log`（自动回滚）
        *   日志同时写入：内部 `/data/data/<package>/files/logs/` 与外部 `/sdcard/Android/data/<package>/logs/`
    *   **3.2 日志查看与导出**
        *   修复点击 {VIEW LOGS} 无响应问题：在独立线程解析文件列表，使用 RecyclerView + DiffUtil 保证 UI 流畅
        *   列表默认展示最近 7 天缓存日志，按修改时间倒序；支持单选、多选、全选、反选
        *   点击条目可预览日志内容（仅读模式，支持关键字高亮与 100 KB 分页加载）
        *   提供「导出」按钮：调用系统 Storage Access Framework 文件选择器，由用户选择目标目录；将选中日志压缩为 logs_yyyyMMdd_HHmmss.zip，压缩前计算 CRC32 校验并写入 manifest.txt
    *   **3.3 日志性能与隐私**
        *   单文件 ≤ 5MB，最多保留 10 个文件（含当前），并启用 StrictMode 记录磁盘 IO 违规
        *   敏感字段（身份证号、手机号、GPS 坐标）统一脱敏为 ***，脱敏逻辑封装为 SensitiveDataUtil

4.  **[已完成] 摄像机检测能力优化**
    *   启动时通过 CameraManager.getCameraIdList() 枚举所有摄像头（含 USB 外接、前后置、IR、TOF）
    *   解析 CameraCharacteristics，按镜头朝向、分辨率、帧率范围生成 CameraInfo 列表
    *   在预览界面提供下拉/滑动切换控件，支持手动切换；切换时保存用户偏好到 SharedPreferences
    *   对热插拔 USB 摄像头注册 BroadcastReceiver，动态刷新列表并提示用户
    *   异常场景（权限拒绝、摄像头被占用、设备离线）给出友好 Toast + 日志记录

5.  **[已完成] 应用状态栏显示功能优化**
    *   在普通安卓手机状态栏区域（非工控机副屏）实时显示 FPS、CPU、MEM 三项指标
    *   悬浮窗使用 `StatusService` + `TYPE_APPLICATION_OVERLAY`（需 `SYSTEM_ALERT_WINDOW`），应用内状态栏与悬浮窗同源显示
    *   刷新频率 500ms；FPS 基于 Choreographer，CPU 基于 `/proc/stat` 与 `/proc/self/stat`，MEM 基于 Debug.MemoryInfo
    *   提供状态栏开关，关闭时彻底移除悬浮窗并释放资源
    *   适配 Android 13 及以上通知权限与「在其他应用上层显示」权限，首次启用时引导用户手动授权

6.  **[已完成] 调试入口维护**
    *   持续维护 Native 调试入口 (`main.cpp`)，使其支持命令行参数配置（如摄像头 ID、运行模式），以便于脱离 Android UI 进行底层算法验证与性能分析。

## 8. 常见问题 (FAQ)

*   **Q: 为什么选择 C++ 而不是 Java/Kotlin?**
    *   A: 为了最大化利用 RK3288 的有限算力，避免 JVM 的 GC 暂停（Stop-the-world）影响视频流的实时性。
*   **Q: 摄像头打不开怎么办?**
    *   A: 检查 Android 权限 (`CAMERA`)，并尝试修改 `VideoManager::open(int id)` 中的 ID (0 或 1)。Native 模式下需确保有 `video` 组权限或以 root 运行。

---

## 9. 目录树与 GitHub 映射

### 9.1 目录树（需随变更同步）

```text
rk3288_opencv/
├─ app/                      # Android APK 程序本体（应提交 GitHub）
├─ screenshot-analyzer/       # 截图离线分析器（应提交 GitHub）
├─ app/src/main/cpp/          # Native 核心（应提交 GitHub）
├─ CMakeLists.txt             # Native 构建入口（应提交 GitHub）
├─ README.md                  # 项目说明（应提交 GitHub）
├─ README_BUILD.md            # 构建说明（应提交 GitHub）
├─ DEVELOP.md                 # 开发文档（应提交 GitHub）
├─ CREDITS.md                 # 第三方致谢与许可证（应提交 GitHub）
└─ ErrorLog/                  # 调试入口（建议本地保留，内容按需决定是否提交）
```

### 9.2 频繁变动项维护路径

*   **应用名/版本号**: `app/build.gradle`（`versionCode`/`versionName`）
*   **权限/组件注册**: `app/src/main/AndroidManifest.xml`
*   **日志目录与策略**: `AppLog` / `FileLogSink` / `NativeLog`
*   **性能采集策略**: `StatsRepository`

### 9.3 截图分析器使用方式

*   入口: `./gradlew :screenshot-analyzer:run`
*   输出: `ErrorLog/SCREENSHOT_REPORT.md` + `ErrorLog/crops/`
