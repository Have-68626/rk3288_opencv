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

> ✅ 已完成代办 1–15 已迁移归档至 [CHANGELOG.md](CHANGELOG.md)（见 `[Unreleased]` → `Documented`）。此处仅保留未完成待办（从 16 开始编号）。

16. ⬜ **[P0] 新增“多人脸”检测与识别：多目标跟踪 + 身份稳定输出（最优策略待定）**
    *   **现状核对**：当前实时链路 `BioAuth::verify()` 仅对“主脸(最大框)”做识别；虽可检测到多脸，但未对每张脸做稳定的识别输出与跨帧关联。
    *   **资料依据**：常见工程策略为“多脸检测 + 跟踪(SORT/DeepSORT/ByteTrack 等) + 必要时抽帧做重识别”，以避免每帧对所有脸都做 embedding 推理；跟踪可显著提升实时性并降低身份跳变（ID switch）风险。[Khalifa et al., 2022](https://www.mdpi.com/2076-3417/12/11/5568/htm)
    *   **目标**：支持同时识别多张人脸，并在 UI/日志中稳定输出每张脸的身份（含 Unknown）与置信度；支持可配置策略：检测频率、跟踪器类型、主脸优先/全脸输出、最大处理人脸数。
    *   **验收**：多人同框场景下，每张脸的身份输出在短时遮挡/移动时保持稳定（ID switch 可控）；在 1080p60 输入下 CPU/内存可接受且不刷屏（有节流/抽帧策略）。

17. ⬜ **[P1] 预览渲染进一步降开销：减少 Native 侧颜色转换/内存拷贝（从“避免 Java Bitmap”升级到“尽量少拷贝”）**
    *   **现状核对**：当前预览已从 `ImageView.setImageBitmap()` 高频刷新迁移为 Surface 渲染，但仍存在每帧 `BGR→RGBA` 转换与整帧 `memcpy` 写入 `ANativeWindow_Buffer` 的固定成本；若每帧调用 `ANativeWindow_setBuffersGeometry()` 也可能引入额外开销。
    *   **资料依据**：CameraX 官方对预览实现强调“性能/兼容性模式权衡”，`PERFORMANCE` 倾向使用 `SurfaceView`；NDK 的 `ANativeWindow` 是 `Surface` 的 C 侧对应物，适合低延迟显示但仍需注意缓冲区尺寸/格式设置与写入路径成本。[CameraX Preview](https://developer.android.com/media/camera/camerax/preview)；[PreviewView.ImplementationMode](https://developer.android.com/reference/kotlin/androidx/camera/view/PreviewView.ImplementationMode)；[ANativeWindow](https://developer.android.com/ndk/reference/group/a-native-window)
    *   **目标**：仅在帧尺寸/格式变化时更新 `setBuffersGeometry`；尽量复用 `RGBA` 缓冲避免重复 `cvtColor`；为后续“GPU 渲染链路（GL/SurfaceTexture）或更低拷贝路径”预留接口与回退策略（Surface 不可用时回退到 Bitmap）。
    *   **验收**：在 1080p60 场景下渲染线程 CPU 占用进一步下降且更稳定（抖动减少）；长时间运行无 OOM/ANR；旋转/前后台切换时 Surface 解绑/重绑稳定，预览可自动恢复。

18. ⬜ **[P1] CameraX 分辨率选择策略现代化：从 `setTargetResolution` 迁移到 `ResolutionSelector`（并明确回退规则）**
    *   **现状核对**：当前 CameraX 侧分辨率限制仍依赖 `ImageAnalysis.Builder.setTargetResolution(Size)`，该 API 已标注为 deprecated，长期维护风险更高。
    *   **资料依据**：CameraX 官方 API 说明 `setTargetResolution` 已弃用，推荐使用 `ResolutionSelector + ResolutionStrategy` 来表达“偏好分辨率 + 回退”。[ImageAnalysis.Builder](https://developer.android.com/reference/kotlin/androidx/camera/core/ImageAnalysis.Builder.html)
    *   **目标**：用 `ResolutionSelector` 表达“优先 ≤1080p、必要时 ≤720p”并提供明确的 fallback（避免绑定失败）；在 UI/日志中记录最终选中的输出分辨率（`getResolutionInfo()`）。
    *   **验收**：不同机型上分辨率选择一致可解释；绑定失败可自动回退而不是直接闪退/卡住；日志能看到最终生效分辨率。

19. ⬜ **[P1] 图像分析背压与格式策略校准：确保 60fps 下不阻塞管线、避免隐式颜色转换**
    *   **现状核对**：当前以“尽量跑 60fps”为目标，但当分析耗时超过单帧预算（60fps≈16ms）时，若策略不当会卡住相机管线或导致预览抖动。
    *   **资料依据**：CameraX 图像分析文档明确说明 `STRATEGY_KEEP_ONLY_LATEST`/`STRATEGY_BLOCK_PRODUCER` 的差异，以及当分析耗时高时应选择合适策略；并且若使用 RGBA 输出格式，CameraX 会在内部把 YUV 转成 RGBA（这是一笔额外开销）。[CameraX 图像分析](https://developer.android.google.cn/media/camera/camerax/analyze)
    *   **目标**：统一采用“分析链路不阻塞预览”的背压策略（默认 keep-latest），并在日志中输出“分析耗时/丢帧/队列状态”；保持 YUV 输出，避免 CameraX 隐式 RGBA 转换。
    *   **验收**：在 1080p60 输入下预览不卡住；分析高负载时表现为“可控丢帧”而不是“整体冻结”；CPU 峰值更平滑。

20. ⬜ **[P1] 外部帧（JNI）进一步提速：引入/复用 libyuv 做 YUV 拷贝与色彩空间转换（减少 OpenCV 逐帧开销）**
    *   **现状核对**：当前 JNI 侧已做了“按行 memcpy 优先、复用缓冲”等优化，但仍依赖 OpenCV 的 `cvtColor/resize`，在部分设备上可能仍是瓶颈。
    *   **资料依据**：Android AOSP 内置维护了 libyuv（大量 YUV/ARGB/I420 互转与缩放函数），是移动端常见的高性能像素转换基础库，可作为进一步优化方向（新增依赖需同步处理许可证/来源记录）。[AOSP libyuv convert.h](https://android.googlesource.com/platform/external/libyuv/+/435db9f11b09187e0d60683813a28d07cc13166b/files/include/libyuv/convert.h)
    *   **目标**：在不改变现有输入契约的前提下，将关键路径的“YUV 打包/缩放/转 RGB”切换为 libyuv；统一缓冲复用策略，避免每帧分配与清零。
    *   **验收**：1080p60 外部输入下 CPU 占用进一步下降；长时间运行无 OOM；不同 stride/pixelStride 机型兼容性提升。

21. ⬜ **[P1] Mock 初始化“可取消”做实：取消应能中断 Native 阻塞（而非仅取消 UI 等待）**
    *   **现状核对**：目前 Mock 初始化已放到后台线程且 UI 可点取消，但若 native 初始化内部阻塞（打开/探测/首帧解码），UI 取消并不能立刻让 native 停止，仍可能造成“后台长卡/不可控等待”。
    *   **资料依据**：官方对 ANR 的建议强调“主线程不要做阻塞 I/O/长耗时计算，并把耗时工作放到工作线程”，同时要在关键路径提供可感知的进度与可终止的工作方式。[保持应用响应（避免 ANR）](https://developer.android.com/training/articles/perf-anr.html)
    *   **目标**：为 Mock 的 native 初始化链路增加“取消/超时”机制（例如可轮询的取消标志、分段初始化点、超时失败返回）；取消后释放资源并回到可再次尝试的状态。
    *   **验收**：导入超大/异常视频时，用户取消能在可控时间内生效；取消后不会遗留后台线程/锁；可重复导入不累积资源占用。

22. ⬜ **[P1] 悬浮窗 Service 稳定性增强：Android 8+ 后台限制下改为前台服务（可选）**
    *   **现状核对**：悬浮窗显示依赖 `StatusService`，但 Android 8.0+ 对后台执行/后台 Service 有更严格限制，可能导致服务被系统停止，从而出现“悬浮窗/按钮状态异常或被动消失”。（目前已做 UI 同步，但服务被杀的根因仍可能存在）
    *   **资料依据**：Android 8.0 引入后台执行限制，后台启动/运行 Service 行为受限；需要持续运行且对用户可见的任务应使用前台服务（带通知）。[后台执行限制](https://developer.android.google.cn/about/versions/oreo/background.html)；[前台服务](https://developer.android.com/develop/background-work/services/foreground-services)
    *   **目标**：为 `StatusService` 增加“前台服务模式”开关：当悬浮窗开启且应用退到后台时，自动切换为前台服务并展示低打扰通知；回到前台可降级为普通服务或保持前台（策略可配置）。
    *   **验收**：开启悬浮窗后切后台/锁屏/长时间运行不被系统无故停止；通知可关闭悬浮窗并停止服务；不引入新增 ANR。

23. ⬜ **[P1] logcat 采集能力校准：明确权限边界 + 失败降级 + 隐私脱敏**
    *   **现状核对**：当前实现通过执行 `logcat -d` 抓取；在 Android 4.1+（API 16+）读取“全设备 logcat”权限受限，通常只能稳定获取“本进程”相关日志；不同 ROM/厂商对可读范围差异较大。
    *   **资料依据**：Android 4.1 起 `READ_LOGS` 基本仅系统/特权应用可用；不建议在生产环境记录敏感信息到 logcat。[Log Info Disclosure](https://developer.android.com/privacy-and-security/risks/log-info-disclosure)
    *   **目标**：在 UI 上明确展示“采集范围（仅本进程/是否含系统缓冲）”；抓取失败时给出可诊断原因与替代方案（导出 App/Native 日志）；对导出的 logcat 做同等脱敏策略（至少对高风险字段掩码）。
    *   **验收**：不同设备上抓取行为一致可解释；失败场景有明确提示；导出文件不包含明显敏感信息。

24. ⬜ **[P1] “退出”语义补强：任务栈移除≠强制停止进程（提供可选强退策略与提示）**
    *   **现状核对**：`finishAndRemoveTask()` 的语义是“结束任务内 Activity 并从最近任务移除”，但系统不保证立刻杀死进程；如果仍有后台组件/线程，进程可能短时驻留。
    *   **资料依据**：官方对 Recents/Task 管理说明 `finishAndRemoveTask()` 用于将任务从概览中移除。[Recents（finishAndRemoveTask）](https://developer.android.google.cn/guide/components/activities/recents)；[ActivityManager.AppTask.finishAndRemoveTask](https://developer.android.com/reference/kotlin/android/app/ActivityManager.AppTask#finishandremovetask)
    *   **目标**：在“退出”按钮旁增加二级选项：默认“安全退出(推荐)”仅做资源释放+移除任务；可选“强退(不推荐)”在完成资源释放后调用 `killProcess/exit`（并给出风险提示）；同时将“仍可能驻留”的系统行为在 About/帮助中解释清楚。
    *   **验收**：安全退出时资源必然释放且无残留悬浮窗；强退模式下也不出现明显资源泄漏/文件损坏；用户能理解两种模式差异。

25. ⬜ **[P1] 日志筛选性能优化：从“整段字符串 split”升级为“流式/分页筛选”**
    *   **现状核对**：当前筛选基于已加载内容做 `split("\\n")` 后逐行匹配与高亮；当用户点击“看全部”加载超大日志时，内存与 CPU 峰值可能上升。
    *   **目标**：实现“流式筛选”：按行读取文件并匹配，限制最大渲染行数（例如最近 N 行命中）；高亮只对可见窗口或限定数量进行；为正则提供超时/长度保护，避免灾难性回溯导致卡顿。
    *   **验收**：对超大日志筛选不卡顿、不 OOM；正则错误/过慢有明确提示并可中断；命中结果可快速复制/导出。

26. ⬜ **[P0] 预览画面出现异常左右翻转：镜像/翻转语义统一 + 端到端校验**
    *   **现状核对**：Android 外部帧链路支持 `mirrored`（`Camera2CaptureController`/`CameraXCaptureController` 判定前置摄像头后传给 JNI，native 在 `Engine.cpp` 对 `mirrored` 做 `cv::flip(..., 1)`）；Windows/浏览器预览链路支持 `flipX/flipY`（`FramePipeline.cpp`/`PreviewPage.tsx`）。
    *   **资料依据**：OpenCV `flipCode` 语义：`0` 上下翻转、`>0` 左右翻转、`<0` 同时翻转（需保证预览/叠加/识别使用同一坐标系）。https://docs.rs/opencv/latest/opencv/core/fn.flip.html
    *   **目标**：明确并固化“X/Y 翻转”与“镜像/自拍”在 UI 与实现中的一致语义；预览画面、叠加框、识别 ROI 三者保持一致（避免“画面翻转但框/识别未翻转”或相反）；为每个相机（含 Mock）提供可复现的翻转设置（可持久化）。
    *   **验收**：前/后摄与 Windows 相机均可通过同一组开关得到可预测输出；在旋转/热切换相机/方案后仍保持一致；叠加框与实际人脸位置无左右偏移。

27. ⬜ **[P0] Mock 模式加载文件：兼容大文件（图/视频）前提下优化加载流程与速度（不应直接拒绝）**
    *   **现状核对**：Android `handleMockFileSelection()` 目前会把用户选择的文件整段拷贝到 `getCacheDir()`（8KB buffer、无进度、不可取消），且对扩展名做长度/字符集校验；大文件会出现“长时间卡在 Loading”或用户误以为被拒绝；Mock 初始化虽可取消，但文件拷贝阶段不可取消。
    *   **目标**：大文件不直接拒绝：优先“零拷贝/少拷贝”路径（能直接读取则不落地副本），必要时再做可观测的后台拷贝（显示进度/速率/预计耗时、允许取消/重试）；对超规格输入提供明确的自动降档策略与提示（例如转码/抽帧/缩放），并把失败原因结构化落盘到日志/证据链。
    *   **验收**：选取 ≥1GB 的 mp4 或高分辨率图片时，不出现“无响应/假死”；可看到进度与可取消；最终可进入 Mock 运行或给出明确可复现的失败原因（含建议动作）。

28. ⬜ **[P1] 日志选择器缺少全选/反选：在顶部导航栏右侧补齐；复选框触控区过小导致误触打开文件**
    *   **现状核对**：`activity_log_viewer.xml` 顶部为 `Toolbar(title=Log Viewer)`，未提供右侧操作按钮；`LogAdapter` 对整行 `itemView` 绑定了“打开日志”点击，`CheckBox` 触控区为 `wrap_content`，易误触导致直接进入详情页。
    *   **目标**：在导航栏右侧增加“全选/反选（或：全选/清空）”操作；优化选择交互（扩大复选框热区、支持长按进入多选、点击行内容与点击复选框的行为分离）。
    *   **验收**：单手操作不误触；批量选择/取消选择效率显著提升；无障碍（TalkBack）下可正确读出与操作。

29. ⬜ **[P1] 日志详情页交互修复：‘看末尾’无响应/‘加载更多’与‘看全部’效果不明确**
    *   **现状核对**：`LogDetailActivity` 已实现 `loadTail/loadMore/loadAll`，但缺少“自动滚动到末尾/当前位置提示/加载范围提示”；`loadMore()` 在更新按钮状态时使用了旧的 `currentStartOffset`（更新顺序在 UI 回调之后），可能导致按钮状态与实际数据不一致，用户感知为“点了没反应”。
    *   **目标**：明确三类按钮语义并可视化反馈（当前已加载字节范围/是否已全量/加载耗时）；`看末尾` 必须滚动到末尾并高亮新增段；修正按钮 enable/disable 的状态机与更新顺序；对超大日志避免一次性全量渲染导致卡顿。
    *   **验收**：三按钮都能稳定生效；在 10MB~200MB 日志上操作不 OOM、不长时间卡顿；用户能从 UI 明确看到“是否已经是末尾/是否已全部加载”。

30. ⬜ **[P1] 日志筛选交互升级：输入关键字时自动匹配（去掉‘应用’按钮）；‘清空’改为输入框右侧叉叉按钮**
    *   **现状核对**：`LogDetailActivity` 当前需要点击“应用”触发筛选，且有独立“清空”按钮；不符合“边输边筛”的预期。
    *   **目标**：输入即筛选（带 debounce/节流，避免每个字符都全量扫描）；移除“应用”按钮；将“清空”移动到输入框右侧（X 按钮）；保留快捷筛选按钮但与输入框同步。
    *   **验收**：输入过程实时更新且不卡顿；正则错误提示清晰；清空一键恢复原文（或当前加载范围）并保持滚动位置合理。

31. ⬜ **[P1] 日志可读性增强：左侧显示行号；日志详情页支持直接删除当前日志（按钮在顶部导航栏右侧）**
    *   **现状核对**：当前日志正文为纯文本展示，缺少行号；删除只能回到列表页通过“删除所选”完成，缺少“正在看的这份日志直接删除”的快捷入口。
    *   **目标**：在详情页增加“删除当前日志”入口（带二次确认与失败原因提示）；可选显示行号（默认开启或可配置），并保证高亮/筛选后行号口径可解释（原始行号或筛选后行号需明确）。
    *   **验收**：删除后自动返回列表并刷新；行号显示在不同字体大小/横竖屏下不遮挡正文且滚动同步；大文件下行号不导致额外卡顿。

32. ⬜ **[P0] 软件内补齐热重启入口；修复“预览黑屏但仍在检测/识别”的链路一致性**
    *   **现状核对**：App 内部已存在 `restartMonitoring()`（用于自动切换/恢复），但缺少用户可控的“热重启”入口；预览渲染在 `previewSurfaceReady` 为 true 时走 `nativeRenderFrameToSurface()`，若 Surface/ANativeWindow 链路异常可能出现黑屏，但引擎仍在处理帧并输出识别事件。
    *   **资料依据**：CameraX Preview/SurfaceView/ANativeWindow 属于性能优先路径，但需对 Surface 解绑/重绑与失败回退做完备处理。https://developer.android.com/media/camera/camerax/preview ；https://developer.android.com/ndk/reference/group/a-native-window
    *   **目标**：提供显式“热重启”按钮（例如设置页/顶部栏），并输出可诊断的状态（采集是否出帧、JNI 推帧是否成功、渲染是否成功）；当检测到“采集正常但渲染停滞/黑屏”时，自动做渲染链路重建与回退（Surface→Bitmap）。
    *   **验收**：黑屏场景可被自动恢复或一键热重启恢复；恢复后不出现崩溃/ANR；日志能定位是“采集失败/推帧失败/渲染失败”的哪一类问题。

33. ⬜ **[P1] 识别事件框显示与缩小不应影响预览画面：改为覆盖式叠加而非挤压布局**
    *   **现状核对**：`activity_main.xml` 中识别事件面板位于预览区域下方，展开后会缩小 `monitor_container` 高度，导致预览被挤压；用户期望事件面板覆盖在预览上（类似 HUD）。
    *   **目标**：将识别事件列表改为预览容器内部叠加（可半透明、可拖拽/折叠、可配置最大高度）；展开/收起仅影响叠加层，不改变预览尺寸与比例规则。
    *   **验收**：展开/收起事件面板时预览尺寸不变；在横竖屏与全屏模式下行为一致；长时间运行事件列表不会导致 UI 卡顿。

34. ⬜ **[P1] 采集策略“自动模式”UI：自动开启时灰显采集方案并明确逻辑关系，避免误导**
    *   **现状核对**：当前 `applyCaptureUiState()` 会在自动模式开启时禁用采集方案选择，但 UI 未明确告知“自动模式下实际生效的方案选择规则”；运行时也未把“当前正在使用 Camera2/CameraX”稳定展示到 UI（尤其在 watchdog 自动降级后）。
    *   **目标**：在 UI 中明确显示逻辑关系（例如“自动=默认 Camera2，失败自动切换到 CameraX；手动=固定使用所选方案”）；运行中实时展示当前生效方案与最近一次切换原因（含自动降级/恢复重试）。
    *   **验收**：用户在不看日志的情况下能理解当前生效方案；自动降级发生时 UI 有明确提示且不会造成“禁用但看起来像选中了某方案”的误解。

35. ⬜ **[P1] 加速方案研究与落地：CPU/CPU+GPU（OpenCL）/专用硬件加速，优先适配 ARM（RK3288 与 Qualcomm）**
    *   **现状核对**：当前已在 `VideoManager` 启用 OpenCL（`cv::ocl::setUseOpenCL(true)`），但缺少“哪些算子实际走 OpenCL/收益多少/失败如何回退”的可量化结论；文档已提到 RK MPP 解码与端侧推理后端，但尚未形成可执行的对比矩阵与验收口径。
    *   **资料依据**：OpenCV OpenCL/UMat 透明加速机制（算子覆盖与回退需实测）：https://docs.opencv.ac.cn/4.x/d7/d45/classcv_1_1UMat.html ；Rockchip MPP 开发指南（解码/零拷贝等）：https://github.com/rockchip-linux/mpp/blob/develop/doc/Rockchip_Developer_Guide_MPP_EN.md ；Qualcomm Neural Processing SDK（CPU/GPU/DSP 运行时）：https://developer.qualcomm.com/software/qualcomm-neural-processing-SDK ；TFLite Hexagon delegate（DSP 加速路径参考）：https://blog.tensorflow.org/2019/12/accelerating-tensorflow-lite-on-qualcomm.html
    *   **目标**：输出“端到端链路分段”的加速选型与开关：解码（CPU vs MPP）、预处理（NEON vs OpenCL vs libyuv）、检测/特征（ncnn/OpenCV DNN/TFLite/Qualcomm SDK 可选）；建立统一的基准测量脚本与报告格式（FPS、P95 延迟、功耗/温度可选、内存峰值）。
    *   **验收**：在 RK3288 与至少 1 台 Qualcomm 设备上产出可复现报告；每个加速开关都有明确回退路径与失败原因输出；默认配置在稳定性不退化的前提下获得可观收益。

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

## 📄 许可证
MIT License
