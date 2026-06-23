# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **关于版本号重置的说明 (Version Reset Notice)**
> 在项目早期的探索与架构验证阶段，曾短暂使用过 `1.x` 和 `2.0.0-rcX` 序列的版本号。随着 Windows 端向 Web SPA 架构（React + Vite + 本地 HTTP 服务）的全面迁移，以及 Android
与 Native 核心层的彻底重构，项目进入了实质性的 Beta 测试阶段。为了准确反映项目当前的成熟度与演进主线，版本号已从 `v0.1beta0` 开始重新校准，并统一收敛了全端的版本口径。

## [Unreleased]

### Fixed
- **Docs**: 修复 INT8 量化文档中 Google Test 引用错误（项目使用自定义 bool 函数，非 Google Test），更新校准图片说明为实际使用的 `deps/WIDER_train/`（`5dd5cbf`）
- **HttpFacesServer**: SseSession::writeFrame 缺 pipe 空指针检查，修复空转泄漏（`454a7e0`）
- **HttpFacesServer**: buildJpegWithOverlay 直接修改 rs.bgr → 多线程数据竞争，改用 clone() 隔离副本（`7c4a04b`）
- **HttpFacesServer**: StreamSession 指针转型未定义行为 — 构造函数注入 server 替换 reinterpret_cast（`68f6cfd`）
- **HttpFacesServer**: SseSession 静态变量（lastSeq/lastKeep）改为成员变量，修复多连接帧同步异常（`8c824bc`）
- **HttpFacesServer**: `escapeJsonString` 补全控制字符转义（`\b`/`\f`/`\n`/`\r`/`\t`/`\uXXXX`）（`4a13def`）
- **HttpFacesServer**: `onCameras` 改为 JsonValue 树构建，补全安全响应头（`4a13def`）
- **HttpFacesServer**: 新增 SocketGuard RAII 封装，消除 9 处手动 closesocket（`b83545a`）
- **HttpFacesServer**: `onSettings` 改用 parseJson+jsonOk，消除手工 JSON 拼接（`b83545a`）
- **HttpFacesServer**: `onModels` 移除 configuredPath/resolvedPath 路径泄露（`b83545a`）
- **HttpFacesServer**: 8 处魔法数字提取为命名常量（`acd6f93`）
- **HttpFacesServer**: MjpegSession 无 OpenCV 时返回 false 终止流（`acd6f93`）
- **HttpFacesServer**: `readFileBinary` 改用 seekg+read 取代 ostringstream（`acd6f93`）
- **All**: 5 项 Round 2 Code Review 问题修复（`c4724e6`）:
  - `native-lib.cpp` 重复 `JNIEXPORT jboolean JNICALL`（CR-06 ✅）
  - `Engine.cpp` bioAuth 初始化失败后 `return false`（CR-04 ✅）
  - `NativeBridge.java` 删除无实现声明 `nativeInitFile`（CR-08 ✅）
  - 新增 `ErrorBoundary.tsx` Web SPA 全局错误边界（CR-13 ✅）
  - `test_int8_quantization.cpp` 增强测试有效性
- **Round 2 剩余修复（工作区未提交）**:
  - `native-lib.cpp` 添加 `g_activityMutex` 保护全局 JNI 引用（CR-09 ✅）
  - `native-lib.cpp` `sendRecognitionResult` 添加 ExceptionCheck（CR-07 ✅）
  - `Engine.cpp` 移除 `const_cast`，NV21 → BGR 改用本地副本（CR-03 ✅）
  - `Engine.cpp` `thread_local cv::Mat` 改为局部变量（CR-05 ✅）
  - `web/http.ts` 缓存命中/API 响应添加 `isValidEnvelope` 运行时校验（CR-10/CR-11 ✅）
  - `web/AppStore.tsx` `patch: unknown` 改为 `patch: Partial<ServerSettingsDoc>`（CR-12 ✅）

### Refactored
- **HttpFacesServer**: 端点注册表 + 流式会话抽象，`handleApi()` 从 348 行缩减至 24 行，总计 -343 行（`3c576bc`）

### Added
- **Documentation**: 新增 `DEVELOP.md` 附录 A "代码速查"，整合原 `CODE_WIKI.md` 的关键类与函数参考。

### Changed
- **Documentation**: 精简 `README.md`，删除与 `DEVELOP.md`/`CREDITS.md` 重复的项目结构和依赖列表，改为交叉引用。
- **Documentation**: 在 `README.md` 新增 `🔧 脚本工具` 章节，为 `scripts/` 目录下的工具脚本添加显式引用说明。

### Removed
- **Documentation**: 删除冗余文档文件：
  - `CODE_WIKI.md`（内容已合并至 `DEVELOP.md` 附录 A）
  - `Model_Inventory.md`（内容已合并至 `CREDITS.md` 模型台账部分）
  - `win_cmake.txt` / `win_cmake2.txt`（CMake 代码已整合至 `CMakeLists.txt`）
  - `test_json.sh`（临时测试脚本）
  - `docs/芯片调度优化与架构分析报告.md`（学术论文，与项目无关）
  - `docs/ARM 平台加速方案与 OpenCV.md`（与 `acceleration_study.md` 重复）
  - `docs/refactor-insights-audit-2026-05-16.md`（临时审计产物）
  - `docs/perf/stutter-investigation-report.md`（临时调试产物）
  - `docs/documents/` 目录（重构洞察笔记，已归档）
  - `docs/superpowers/plans/` 目录（AI Agent 执行计划，已归档）

### Code Review (2026-06-23, Consolidated) — 修复记录 + 全部未决问题
> 6 轮审查累计覆盖项目全量源代码。以下为完整修复记录和未决问题总表。

#### ✅ 已修复确认
| # | 问题 | 修复 commit(s) | 所属轮次 |
|---|------|---------------|---------|
| CR-01 | `escapeJsonString` 控制字符未转义 | `4a13def` | R1 |
| CR-02 | `onCameras` 手工拼接 JSON 缺安全头 | `4a13def` | R1 |
| HR-01 | 无 RAII socket 封装 | `b83545a` | R1 |
| HR-02 | `onSettings` 手工拼接 JSON | `b83545a` | R1 |
| HR-03 | `onModels` 文件路径泄露 | `b83545a` | R1 |
| MR-01 | 8 处魔法数字 | `acd6f93` | R1 |
| MR-02 | MjpegSession 空转循环 | `acd6f93` | R1 |
| MR-03 | `readFileBinary` 低效实现 | `acd6f93` | R1 |
| CR-04 | bioAuth 初始化失败后未 `return false` | `c4724e6` | R2 |
| CR-06 | 重复 JNIEXPORT 声明 | `c4724e6` | R2 |
| CR-08 | NativeBridge 无实现声明 | `c4724e6` | R2 |
| CR-13 | Web 全局缺 Error Boundary | `c4724e6` | R2 |
| CR-03 | `const_cast` 绕过 NV21 常量性 | `337c58b` | R2 |
| CR-05 | `thread_local cv::Mat` 残留旧状态 | `337c58b` | R2 |
| CR-07 | `sendRecognitionResult` 缺 ExceptionCheck | `337c58b` | R2 |
| CR-09 | `g_activity` 数据竞争 | `337c58b` | R2 |
| CR-10 | Web 缓存无运行时校验 | `337c58b` | R2 |
| CR-11 | Web API 响应无 envelope 校验 | `337c58b` | R2 |
| CR-12 | `updateServerSettings` 参数 `unknown` | `337c58b` | R2 |
| CR-15 | test_video_manager 死代码未注册 | `96703eb` | R3 |
| CR-16 | 测试套件重复注册 | `96703eb` | R3 |
| CR-14 | INT8 测试 blob 名错误 | `96703eb` + `c4724e6` | R3 |
| CR-17 | FramePipeline 88 个缺失闭合大括号 | `436e068` | R3 |
| CR-18 | FramePipeline 未声明 `prevDesired` | `436e068` | R3 |
| CR-19 | FramePipeline 未声明 `d`/`f` | `436e068` | R3 |
| CR-20 | FramePipeline 未定义 `openOnce()` | `436e068` | R3 |
| CR-21 | `tryGetRenderState` 无锁 | `436e068` + `cffa4e3` | R3 |
| CR-22 | `snapshotFaces` 无锁 | `436e068` + `cffa4e3` | R3 |
| CR-23 | `waitFacesSeqChanged` 从不等待 | `436e068` + `cffa4e3` | R3 |
| HR-04 | `updateInferenceThrottle` 覆盖两套节流 | `740401f` | R3 |
| HR-05 | `fprintf` 热路径阻塞 IO | `ba1ac45` + `740401f` | R3 |
| HR-06 | 百分位 `v[-1]` 越界 | `ba1ac45` | R3 |
| HR-07 | `processFrame` 误导性可变引用 | `ba1ac45` + `740401f` | R3 |
| HR-20 | `render_.status` 字符串赋值无锁 | `436e068` + `cffa4e3` | R3 |
| MR-04 | 旋转尺寸检查逻辑修正 | `ba1ac45` | R3 |

#### 🔴 未决问题 — 按严重性分类

##### CRITICAL (已全部修复)
| # | 模块 | 文件 | 问题 |
|---|------|------|------|
| CR-24 | 人脸识别 | `FaceDatabase.cpp:82`, `FaceRecognizer.cpp:83` | `persons_` map + `identifyThreshold_` 双数据竞争（无锁读写）| ✅ 已修 |
| CR-25 | 人脸识别 | `FaceSearch.cpp:106` | `reset()` 失败后索引状态损坏 | ✅ 已修 |
| CR-26 | 安全 | `JsonLite.cpp:248` | JSON 递归解析无深度限制 → 栈溢出可被利用 | ✅ 已修 |
| CR-27 | 安全 | `WinCrypto.cpp:144` | AES-GCM 密钥未 `SecureZeroMemory` | ✅ 已修 |
| CR-28 | 安全 | `WinJsonConfig.cpp:1357` | TOCTOU 竞态 — 配置变更被覆盖 | ✅ 已修 |
| CR-29 | 安全 | `WinCrypto.cpp:181` | AES-256-GCM 无 AAD → 字段可被交换 | ✅ 已修 |
| CR-30 | BioAuth | `BioAuth.cpp` 全局 | 零线程安全 — 并发 `verify()` 数据竞争 | ✅ 已修 |
| CR-31 | BioAuth | `BioAuth.cpp:101,186` | 置信度归一化公式本质错误 → 拒真率接近 100% | ✅ 已修 |
| CR-32 | MotionDetector | `MotionDetector.cpp` 全局 | 零线程安全 — `detect()` 与 `getMotionMask()` 竞态 | ✅ 已修 |
| CR-33 | MotionDetector | `MotionDetector.cpp:43` | `getMotionMask()` 浅拷贝 → 共享缓冲区被覆写 | ✅ 已修 |
| CR-41 | FaceInfer | `FaceInferStages.cpp:451` | `assumeL2Normalized` 硬编码 → 非归一化模型检索静默错误 | ✅ 已修 |
| CR-42 | FaceInfer | `ModelRegistry.cpp:18` | `ensureBuiltinRegistered` 无 `call_once` → 并发 UB | ✅ 已修 |
| CR-43 | 适配器 | 全部 9 个 `*Adapter.cpp` | 全线零线程安全 | ✅ 已修 |
| CR-44 | 适配器 | `ArcFaceAdapter.cpp:44`, `YoloFaceAdapter.cpp:47` | ncnn→OpenCV 回退传递 `.param` 文件 | ✅ 已修 |
| CR-45 | 适配器 | `ArcFaceAdapter.cpp:25`, `MobileFaceNetAdapter.cpp:20` | 路径拼接硬编码 `/` → Windows 不兼容 | ✅ 已修 |
| CR-46 | 日志 | `EventManager.cpp:57` | `static mt19937` 无锁 → uid 重复/死锁 | ✅ 已修 |
| CR-47 | 日志 | `Storage.cpp:64` | `appendLog` 无锁并发 → 日志损坏 | ✅ 已修 |
| CR-48 | 日志 | `EventManager.cpp:34` | `formatEventJson` JSON 未转义 → 非法 JSON | ✅ 已修 |
| CR-49 | MPP | `MppDecoder.cpp:133` | `chunkBuf_` 复用 → MPP 未消费时 `fread` 覆写 | ✅ 已修 |
| CR-50 | MPP | `VideoManager.cpp:585` | MPP 回退 OpenCV 未校验 `cap.isOpened()` → 空转 | ✅ 已修 |
| CR-51 | MPP | `VideoManager.cpp:660` | 摄像头读取失败无限重试 | ✅ 已修 |

##### HIGH (已全部修复)
| # | 模块 | 文件 | 问题 |
|---|------|------|------|
| HR-08 | JNI | `native-lib.cpp:60` | `JNI_OnUnload` 未 join 引擎线程 | ✅ 已修 |
| HR-09 | Android | `Camera2CaptureController.java:315` | `imageReader` stop 后 NPE | ✅ 已修 |
| HR-10 | Android | `CameraXCaptureController.java:89,161` | `provider` 字段无同步 | ✅ 已修 |
| HR-11 | Android | `MainActivity.java:2337` | `nativeInit(-1,...)` 硬编码 | ✅ 已修 |
| HR-12 | Android | `FileLogSink.java:78` | 持锁 sleep(10) | ✅ 已修 |
| HR-13 | Web | `AppStore.tsx:71` | 状态层耦合 UI toast | ✅ 已修 |
| HR-14 | Web | 全局 | `(e as Error)?.message` 模式 15 次 | ✅ 已修 |
| HR-15 | Web | `SplashPage.tsx:29` | useEffect 缺依赖 | ✅ 已修 |
| HR-21 | Windows | `MfCamera.cpp:292` | 负 stride 溢出 | ✅ 已修 |
| HR-22 | Windows | `MfCamera.cpp:224` | IMFMediaSource 打开失败未 Shutdown | ✅ 已修 |
| HR-23 | Windows | `OverlayRenderer.cpp:51,64` | 每帧 6MB clone | ✅ 已修 |
| HR-24 | Windows | `D3D11Renderer.cpp:732` | `Map()` 失败无设备恢复 | ✅ 已修 |
| HR-25 | Windows | `FramePipeline.cpp:533` | 重连参数陈旧 | ✅ 已修 |
| HR-26 | 安全 | `WinJsonConfig.cpp:30` | `readFileAll` 无大小限制 | ✅ 已修 |
| HR-27 | 安全 | `JsonLite.cpp:414` | `parseJson` 无长度限制 | ✅ 已修 |
| HR-28 | 安全 | `WinJsonConfig.cpp:1093` | 备份无轮转 | ✅ 已修 |
| HR-29 | 安全 | `WinJsonConfig.cpp:72` | fallback 非原子 | ✅ 已修 |
| HR-30 | 安全 | `WinJsonConfig.cpp:1065` | 错误信息泄露 | ✅ 已修 |
| HR-31 | 人脸识别 | `FaceDetector.cpp:21` | CascadeClassifier 非线程安全 | ✅ 已修 |
| HR-32 | 人脸识别 | `FaceSearch.cpp:42,55` | NEON 16 字节对齐崩溃 | ✅ 已修 |
| HR-33 | 人脸识别 | `FaceRecognizer.cpp:97` | `identifyThreshold_` 读写竞争 | ✅ 已修（CR-24） |
| HR-34 | 人脸识别 | `FaceInferStages.cpp:440` | `std::move` 导致空 gallery | ✅ 已修 |
| HR-35 | 构建 | `CMakeLists.txt` | 无 `install()` 目标 | ✅ 已修 |
| HR-36 | 构建 | `CMakeLists.txt:111` | 全局 `include_directories()` | ✅ 已修 |
| HR-37 | 构建 | `CMakeLists.txt:85` | 废弃 `add_definitions()` | ✅ 已修 |
| HR-38 | 构建 | `CMakeLists.txt:305` | `file(GLOB_RECURSE)` | ✅ 已修 |
| HR-39 | BioAuth | `BioAuth.cpp:95,183` | ROI 无边界限制 | ✅ 已修 |
| HR-40 | BioAuth | `BioAuth.cpp:72,129` | `detectMultiScale` 魔数 | ✅ 已修 |
| HR-41 | MotionDetector | `MotionDetector.cpp:16` | 假设 BGR 输入 | ✅ 已修 |
| HR-42 | 构建 | `app/build.gradle` | 版本硬编码无 catalog | ✅ 已修 |
| HR-43 | 构建 | `gradle.properties:19` | 单线程 workers | ✅ 已修 |
| HR-44 | 脚本 | `run-web-e2e.ps1:31` | Vite 无就绪检查 | ✅ 已修 |
| HR-45 | 脚本 | `quantize_ncnn_int8.py:61` | 硬编码回退路径 | ✅ 已修 |
| HR-46 | 脚本 | `build_android.bat:24` | NDK 版本硬编码 | ✅ 已修 |
| HR-47 | FaceInfer | `FaceInferencePipeline.cpp:112` | 审计文件毫秒碰撞 | ✅ 已修 |
| HR-48 | FaceInfer | `FaceInferStages.cpp:232,362` | INT8→FP32 降级无日志 | ✅ 已修 |
| HR-49 | FaceInfer | `FaceInferStages.cpp:92` | 每帧全量 gallery 重新解析 | ✅ 已修 |
| HR-50 | 适配器 | `LbphAdapter.cpp:8` | `load()` 忽略 `modelPath` | ✅ 已修 |
| HR-51 | 适配器 | `CascadeAdapter.cpp:33` | 所有检测 score=1.0 | ✅ 已修 |
| HR-52 | 适配器 | `MobileFaceNetAdapter.cpp:48` | `num_threads=1` 硬编码 | ✅ 已修 |
| HR-53 | 适配器 | 多个 `*Adapter.cpp` | `cv::dnn` 调用无 try/catch | ✅ 已修 |
| HR-54 | 日志 | `NativeLog.cpp:144` | 持锁阻塞 IO | ✅ 已修 |
| HR-55 | 日志 | `NativeLog.cpp:121` | `write()` 返回值未检查 | ✅ 已修 |
| HR-56 | 日志 | `NativeLog.cpp:69` | 路径仅按 `/` 分割 | ✅ 已修 |
| HR-57 | 日志 | `NativeLog.cpp:95` | 轮换非原子 | ✅ 已修 |
| HR-58 | 日志 | `NativeLog.cpp:113` | 每次日志 open/write/close | ✅ 已修 |
| HR-59 | 日志 | `EventManager.cpp:57` | 32 位 ID 碰撞 | ✅ 已修 `cf90288` |
| HR-60 | FaceAlign | `FaceAlign.cpp:15` | NaN bbox → UB | ✅ 已修 `cf90288` |
| HR-61 | FaceTemplate | `FaceTemplate.cpp:74` | `serialize` 未更新 `h.dim` | ✅ 已修 `cf90288` |
| HR-62 | FileHash | `FileHash.cpp:60` | SHA-256 64B/次 syscall | ✅ 已修 `cf90288` |
| HR-63 | MPP | `VideoManager.cpp:549` | `cancelToken` 从未被检查 | ✅ 已修 `cf90288` |
| HR-64 | MPP | `VideoManager.cpp:298` | 1MB 栈分配 | ✅ 已修 `cf90288` |
| HR-65 | MPP | `MppDecoder.cpp:147,193` | BUFFER_FULL 数据丢失 | ✅ 已修 `cf90288` |
| HR-66 | 脚本 | `ci.yml:287` | `yolo_face` → `scrfd` CI 量化从未成功 | ✅ 已修 `d2ec0cb` |
| HR-67 | 脚本 | `verify_opencv_host.bat:42` | `OPENCV_CONTRIB_ROOT` 无验证 | ✅ 已修 `d2ec0cb` |
| HR-68 | 脚本 | `verify_faces_test_set01.bat:45` | 同上 | ✅ 已修 `d2ec0cb` |
| HR-69 | 脚本 | `stability_switch_50_adb.ps1:57` | 已废弃 `dumpsys` API | ✅ 已修 `d2ec0cb` |
| HR-70 | 构建 | `app/build.gradle:64` | 硬编码 ncnn 路径 | ✅ 已修 `d2ec0cb` |
| HR-71 | 构建 | `app/build.gradle:108` | `minifyEnabled false` | ✅ 已修 `d2ec0cb` |
| HR-72 | 构建 | `app/build.gradle:109` | proguard 文件缺失 | ✅ 已修 `d2ec0cb` |
| HR-73 | Android | `StatusService.java:48` | overlay 权限绕过后显示 | ✅ 已修 |
| HR-74 | Android | `LogViewerActivity.java:63` | ExecutorService 未 shutdown | ✅ 已修 |
| HR-75 | Android | `LogDetailActivity.java:67` | ExecutorService 未 shutdown | ✅ 已修 |
| HR-76 | Android | `LogViewerActivity.java:417` | logcat pipe 死锁 | ✅ 已修 |
| HR-77 | Web | `cameras.ts + actions.ts` | API 路径忽略 `VITE_API_BASE` | ✅ 已修 |
| HR-78 | Web | `prefs.ts:92` | `savePrefs` 无 try/catch | ✅ 已修 |
| HR-79 | 基准 | `inference_bench_cli.cpp:444,613,785` | 预处理计时测零工作 | ✅ 已修 |
| HR-80 | 基准 | `inference_bench_cli.cpp:939` | NaN/Inf 输出非法 JSON | ✅ 已修 |
| HR-81 | 基准 | `FaceInferOutcomeJson.cpp:84` | `%g` 可输出 NaN/Inf | ✅ 已修 |

##### MEDIUM (42 项)
| # | 模块 | 问题 |
|---|------|------|
| MR-05 | Engine | 两个 `initialize()` 重载 40 行重复 |
| MR-06 | Engine | `catch(...){}` 静默吞异常 |
| MR-07 | JNI | `thread_local lastSeq` 线程切换重复帧 |
| MR-08 | Android | `selectedCameraId` int 局限 |
| MR-09 | Web | `memoryCache` 无驱逐 |
| MR-10 | Web | API 路径字符串分散 4 模块 |
| MR-11 | Web | Vite 模板残留 CSS | ✅ 已修 |
| MR-12 | 测试 | 15+ 组件零测试覆盖 | ✅ 已修 |
| MR-13 | 测试 | 资源清理不完整 | ✅ 已修 |
| MR-14 | FaceInfer | `loadGallery` 每帧磁盘 IO | ✅ 已修 |
| MR-15 | FaceInfer | `ModelRegistry` 无内部同步 | ✅ 已修 |
| MR-16 | FaceDB | `e.count` int 溢出 |
| MR-17 | 配置 | `int8Enabled` INI 缺失 |
| MR-18 | 构建 | `ncnn_precision_test` CRT Debug 缺失 |
| MR-19 | 构建 | OpenCV 列表重复 10+ 次 |
| MR-20 | 构建 | `-fPIE` 在链接器标志中 |
| MR-21 | 构建 | 无 CMakePresets.json |
| MR-22 | D3D11 | `pushFrameTime` 每帧 O(N log N) |
| MR-23 | Windows | `MfCamera.cpp:103` 裸指针 |
| MR-24 | Windows | `FramePipeline.cpp:576` 热路径内存抖动 |
| MR-25 | BioAuth | `detectMultiScale` 参数不可配置 |
| MR-26 | BioAuth | 全局 `equalizeHist` |
| MR-27 | Config | `MOTION_THRESHOLD` 为 640×480 的绝对值 |
| MR-28 | Motion | `cv::Mat` 永不释放 |
| MR-29 | 构建 | ncnn 无条件 ON |
| MR-30 | 构建 | `jetifier=true` |
| MR-31 | 构建 | 无构建缓存 |
| MR-32 | 构建 | `universalApk true` |
| MR-33 | 脚本 | `quantize` 未校验 preset |
| MR-34 | 脚本 | `wait_for_lines` 超时返 0 |
| MR-35 | FaceInfer | `std::move` 后空 gallery |
| MR-36 | FaceInfer | `g_builtinRegistered` 不安全 |
| MR-37 | 适配器 | RetinaFace 锚点未预计算 |
| MR-38 | 日志 | `std::endl` 强制 flush |
| MR-39 | 日志 | `.json` 应为 `.jsonl` |
| MR-40 | MPP | 循环播放未 reset |
| MR-41 | Input | `timeoutMs=0` 永久阻塞 |
| MR-42 | VideoMgr | `open()` 255 行复杂度 |
| MR-43 | FileHash | 裸 `uint32_t` |
| MR-44 | FaceAlign | `inliers` 分配不读 |
| MR-45 | FaceAlign | 5 点每帧堆分配 |
| MR-46 | FaceTemplate | header reserve 偏差 1 |

#### Findings Lifecycle Rules
- **🔴 Open** — 已报告未处理 | **✅ Fixed** — 已提交修复
- **🟡 Stale** — 已知但不紧急 | **🟡 Monitoring** — 持续观察
- **⏸️ Deferred** — 推迟评估

### Archived
- **Technical Plans**: 以下 AI Agent 执行计划已归档至本 CHANGELOG：

#### 检测/识别节流拆分与画框稳定性方案

**目标**：解决检测与识别共用节流参数导致的画框不稳定问题

**核心改动**：
- **A) 节流参数拆分**：新增 `DetectionThrottle` + `RecognitionThrottle` 两套独立参数
  - 检测节流：min=80ms、max=500ms
  - 识别节流：min=80ms、max=2000ms
- **B) 画框稳定性**：跳过推理的帧仍绘制"最近一次的稳定框/轨迹"
- **C) BioAuth 改造**：支持"只检测不识别"模式

**涉及文件**：`InferenceThrottle.h`、`Engine.cpp`、`BioAuth.cpp`、`NativeBridge.java`、`native-lib.cpp`、UI 布局与配置

#### Auto 推理节流日志方案

**目标**：在 Auto 推理节流模式下记录运行状态日志

**核心改动**：
- **变化日志**：每次 effective interval 变化时记录（包含 from/to、CPU/LAT/FPS）
- **心跳日志**：Auto 模式运行时每 30 秒记录一次当前状态
- 日志写入 `AppLog`，关键字 `inferenceAutoTune`

**涉及文件**：`MainActivity.java`

#### docs/ 目录审计计划

**执行结果**：删除 6 个冗余文档，保留核心目录（`bsp/`、`windows-web-spa/`、`runbooks/`、`feasibility/`）

## [0.1.1-beta.1] - 2026-04-16

<a id="added-2"></a>
### Added（2）
- **Build System**: 向 Windows / CLI 的 CMake 构建链路中注入了 `BUILD_ID` 宏，使得二进制产物、评估工具（`win_face_eval_cli` / `inference_bench_cli`）以及应用启动日志都能输出一致的版本号。
- **Documentation**: 在 `CREDITS.md` 中新增了完整的**模型台账 (Model Inventory)**，明确了 OpenCV DNN 模型和 LBP 级联分类器的来源、部署路径与开源许可证。

<a id="changed-2"></a>
### Changed（2）
- **Versioning**: 升级全端版本号至 `v0.1beta1` (Android `versionCode 3`)，彻底消除 Android 构建、CMake 产物与文档之间的版本漂移。
- **Documentation**: 全面校准 `DEVELOP.md`，更新了包含 `web/` 的最新目录结构与前端技术栈（React 18 + AntD 5）。
- **Configuration**: 明确 `%APPDATA%\rk_wcfr\config.json` 为 Windows 端配置的唯一事实来源 (Source of Truth)，并在旧版 `config/windows_camera_face_recognition.ini` 中添加了废弃/迁移警告及模型下载指引。

### Documented
- **README 待办（完成项 37–38）**：从 `README.md` 清理迁移至此处归档。
  - 37. **[P0] 文档全量校准：修订 README/CHANGELOG/DEVELOP/CREDITS 与 `docs/`，确保与当前项目一致**（更新目录树、补齐模型台账、校准配置指引）
  - 38. **[P0] 版本号升级到 `v0.1beta1`：统一代码/构建/文档口径**（Windows/CLI 增加 BUILD_ID 宏注入并打印，Android versionCode 升级）

## [0.1.0-beta.0] - 2026-04-15

<a id="added-3"></a>
### Added（3）
- **Documentation**: 全面重构 `DEVELOP.md`，采用标准化 Markdown、中英双语术语与工程化改进。
- **Examples**: 新增 `docs/examples/` 目录，包含 RK3288 平台的 8 个可编译 C++ 示例：
  - `01_v4l2_capture.cpp`: 直接使用 V4L2 + MMAP 采集摄像头。
  - `02_rkmpp_decode.cpp`: 使用 Rockchip MPP 硬件解码。
  - `03_rknn_inference.cpp`: 使用 RKNN API 进行 NPU 推理。
  - `04_drm_kms_display.cpp`: 使用 DRM/KMS + 双缓冲直接渲染。
  - `05_opencv_rknn_bridge.cpp`: OpenCV 预处理桥接 RKNN。
  - `06_integration_demo.cpp`: 集成 V4L2、RKNN 与显示逻辑的综合示例。
  - `07_camerax_jni_bridge.java`: Android CameraX 与 JNI 桥接实现零拷贝预览。
  - `07_jni_yuv_processor.cpp`: Native C++ 处理 CameraX YUV 流。
- **Architecture**: 在 `DEVELOP.md` 中新增混合架构 (CameraX + Native) 章节。
- **Checklists**: 新增 `docs/checklist/acceptance.md`，涵盖开发、测试与发布阶段验收清单。
- **Quick Start**: 在 `DEVELOP.md` 中新增快速开始脚本指南与依赖矩阵。

<a id="removed-2"></a>
### Removed（2）
- **Legacy SDK**: 移除 `ColorOsSdkBridge`、`PlayIntegrityChecker`、`GmsDetector`、`PrivilegedCommandGate` 与 `DevicePolicy`，消除无效依赖。
- **Security**: 移除 `SecurityEventLogger` 作为 SDK 清理的一部分。

<a id="changed-3"></a>
### Changed（3）
- **Device Profiling**: 简化 `DeviceProfile`，聚焦于硬件数据采集（Build info、Memory/Storage）。
- **Documentation**: 在 `DEVELOP.md` 第 4.6 节新增简化版设备画像任务（待实现）。
- **Structure**: 重构 `DEVELOP.md` 为"概述 → 环境 → 核心开发 → 高级/排障"结构。
- **Content**: 扩展 V4L2、MPP、RKNN 与 DRM/KMS 的技术细节。
- **Style**: 强制执行严格 Markdown 标准与中英双语术语。
- **日志目录**: 错误日志目录统一为 `ErrorLog/`（区分大小写），不再兼容 `errorlog/`。

<a id="documented-2"></a>
### Documented（2）
- **Roadmap**: 将 `README.md` 中已完成的待办项迁移到 `CHANGELOG.md` 以保持可追溯性。
- **README 待办（完成项 1–15）**：从 `README.md` 清理迁移至此处归档（README 仅保留未完成待办 16+）。
  - 1. **[P0] 非 RK3288 设备：高分辨率/高帧率输入的稳定性与兼容性治理**（输入端强制 ≤1080p@60fps；超规格自动降档/拒绝并提示）
  - 2. **[P0] 外部帧输入性能优化（JNI 拷贝与 YUV 转换）**（降低拷贝与转换开销；推理前统一缩放到工作分辨率）
  - 3. **[P0] Mock（文件/URL）模式：大文件/高分辨率的 ANR 与卡顿治理**（初始化后台化+可取消；超规格降档/拒绝）
  - 4. **[P0] 识别事件面板无输出：打通 Native 事件→UI 列表的闭环**（补齐回调链路并做节流）
  - 5. **[P0] 屏幕旋转后预览未回正：旋转元数据与画面布局的稳定性修复**（rotationDegrees 口径统一+配置变更后重绑）
  - 6. **[P0] 人脸识别链路"检测驱动识别"**（仅对检测到的 ROI 识别；预留策略开关）
  - 7. **[P0] 预览叠加人脸框**（渲染帧绘制人脸框，主脸高亮）
  - 8. **[P0] 预览输出上限与低开销渲染**（Surface 渲染替代 Bitmap 高频刷新；输入端仍强制 ≤1080p60）
  - 9. **[P1] 输入源"预检 + 自动降档"策略（相机/文件统一）**（开始监控前预检并记录结果）
  - 10. **[P1] App 内日志查看能力增强**（突破 100KB 限制；支持抓取/展示 logcat）
  - 11. **[P1] 稳定性回归与问题收敛（ErrorLog 证据链）**（一键导出日志+logcat+设备信息+预检结果）
  - 12. **[P1] 悬浮窗开关状态与实际悬浮窗不同步**（以服务真实运行状态回填 UI，支持回前台自动校准）
  - 13. **[P1] 新增"退出"按钮**（停止监控/释放资源/停止悬浮窗并移除任务栈）
  - 14. **[P1] 日志详情页筛选**（支持正则与快捷筛选并高亮）
  - 15. **[P1] 日志删除与保留策略可配置**（手动删除 + 自动清理保留 1/7/14/30 天）
- **README 待办（完成项 16–34）**：从 `README.md` 清理迁移至此处归档（README 仅保留未完成待办 35+）。
  - 16. **[P0] 多人脸检测与识别**（多目标稳定输出与策略化节流）
  - 17. **[P1] 预览渲染降开销**（减少颜色转换/拷贝；Surface/Bitmap 回退）
  - 18. **[P1] CameraX 分辨率选择迁移**（`ResolutionSelector`/回退规则）
  - 19. **[P1] 图像分析背压与格式策略校准**（keep-latest；保持 YUV 输出）
  - 20. **[P1] 外部帧 JNI 提速（libyuv）**（YUV 打包/转换替换 OpenCV 关键路径）
  - 21. **[P1] Mock 初始化可取消**（取消可中断 native 阻塞；超时/降级）
  - 22. **[P1] 悬浮窗 Service 稳定性增强**（前台服务模式 + 通知关闭入口）
  - 23. **[P1] logcat 采集能力校准**（范围提示；失败降级；导出脱敏）
  - 24. **[P1] "退出"语义补强**（安全退出/强退二级选项与解释）
  - 25. **[P1] 日志筛选性能优化**（流式/分页；超时/长度保护）
  - 26. **[P0] 镜像/翻转语义统一**（flipX/flipY；端到端一致；可持久化）
  - 27. **[P0] Mock 文件加载大文件优化**（少拷贝；进度与可取消）
  - 28. **[P1] 日志选择器全选/反选与误触优化**（顶栏操作；扩大勾选热区）
  - 29. **[P1] 日志详情页交互修复**（看末尾/加载更多/看全部语义与反馈）
  - 30. **[P1] 日志筛选交互升级**（输入即筛选；右侧叉叉清空）
  - 31. **[P1] 日志可读性增强**（行号显示；详情页直接删除）
  - 32. **[P0] 热重启入口与黑屏恢复**（渲染停滞检测；Surface 重建；Bitmap 回退）
  - 33. **[P1] 识别事件面板覆盖式叠加**（HUD；不挤压预览）
  - 34. **[P1] 自动模式 UI 解释与展示**（灰显方案；展示当前生效方案与切换原因）
- **DEVELOP.md 修订记录**: 将 `DEVELOP.md` 末尾"修订记录"从开发设计书中移除，并迁移到此处归档。

| 日期 | 版本 | 修订来源 | 变更摘要 |
| :--- | :--- | :--- | :--- |
| 2026-03-03 | 2.0.0-rc1 | 项目初始化 | 建立基础开发指南、目录映射与核心模块说明 |
| 2026-03-22 | 2.0.0-rc2 | document-camera-face-research spec | 新增两大研究章节、图表索引、基准与脚本模板入口、风险清单与参考文献；调整章节编号以保持连续 |
| 2026-03-22 | 2.0.0-rc3 | 文档补齐 | 补充第三方库对比、Android 13+ 权限与后台限制、相机能力报告/拍照录像时序与格式、CameraService 重启检测；补齐人脸方案集成清单/PAD 指标/CI 门禁，并扩展参考文献与图表索引 |
| 2026-03-22 | 2.0.0-rc4 | 文档补齐 | 在第 5/6 章补充 Fotoapparat/CAMKit 对比；补齐 ML Kit/MediaPipe/ArcFace/Dlib/百度/优图 及 阿里/AWS/Azure 表格与集成模板；更新图表索引与参考文献编号 |
| 2026-03-22 | 2.0.0-rc5 | 文档补齐 | 5.3 增加广角/长焦/TOF/红外枚举方法与能力查询（闪光/对焦/曝光补偿等）；6.x 增加特征维度/模板大小/阈值/延迟对比表与逐方案集成模板；修正 ArcFace 表述并扩展参考文献与表索引 |
| 2026-03-22 | 2.0.0-rc6 | 文档补齐 | 6.5.1 补齐 AES-256-GCM+Keystore 关键代码模板与异常处理；6.6 补齐 Gradle7.5+NDK25 demo 构建脚本、JUnit+Robolectric 与 Espresso+MockCamera 模板、GitHub Actions 门禁（识别率≥97% 与 泄漏≤5MB）；更新表索引与参考文献 |
| 2026-03-22 | 2.0.0-rc7 | 文档补齐 | 6.5.1 增补 Java 版 AES/GCM/Keystore 示例（含 KeyStore/KeyGenerator/GCMParameterSpec）；6.6.3 增补 android-emulator-runner 模板以跑 connectedAndroidTest；更新参考文献与示例 build_id 口径 |
| 2026-03-23 | 2.0.0-rc8 | audit-camera-pipeline spec | 相机链路审计与最小修复：权限最小化、退后台释放与回前台恢复、拉帧背压与主线程卸载、Native 引擎线程 join；补齐审计报告与回归测试计划入口 |
| 2026-04-07 | 2.0.0-rc8 | fix-android-camera2-uvc-open-failure spec | 补充 Camera2/CameraX 采集方案复现/验收入口（Runbook 链接、自动/手动切换、热重启与已知限制）。 |

### Technical Highlights
- **Android Compatibility**: 支持 Android API 21–34；声明 camera/USB Host 为可选特性（`required=false`）；关键运行时权限缺失时进入 SAFE MODE；处理 Android 13+ 媒体权限与旧版 scoped-storage 差异。
- **Permissions**: 统一权限说明流程；处理"不再询问"引导至系统设置；权限缺失时阻塞监控启动与引擎初始化；区分 overlay 权限与运行时权限。
- **Logging & Export**: Session 日志命名（`rk3288_yyyyMMdd_HHmmss.log`）；Java+Native 双写；双路径持久化（internal + external）；按时间/数量清理；日志查看器与 ZIP 导出（CRC32 + `manifest.txt`）；敏感数据脱敏（`SensitiveDataUtil`）。
- **Camera Discovery**: 通过 `CameraManager.getCameraIdList()` 枚举摄像头；UI 切换配合 `SharedPreferences` 持久化；USB 热插拔刷新；Mock 源（系统摄像头返回 + 文件选择器）。
- **Overlay Metrics**: `StatusService` overlay 显示 FPS/CPU/MEM（500ms 刷新），仅在启用时请求 overlay 权限。
- **Debug Tooling**: 维护 Native CLI 入口（`main.cpp` / `rk3288_cli`），支持 cameraId 或文件输入，可选 cascadePath/storagePath。
- **Android Capture (Camera2/CameraX)**: 使用 Android 相机栈产生 `YUV_420_888` 帧并通过 JNI 推入 Native 层（含旋转/镜像归一化）；添加 watchdog 重试/退避与自动降级路径。
- **FFmpeg Mock & RTMP**: 支持 Mock URL (MP4/HLS/RTSP)；RTMP 推流入口；可选 `ffmpeg-kit.aar` 集成；循环播放（`-stream_loop -1`）；双 ABI（`arm64-v8a` + `armeabi-v7a`）策略。
- **UI Adaptation**: 竖屏/横屏双布局；基于 `onConfigurationChanged` 的旋转处理（无 Activity 重启）配合过渡动画；识别事件面板显示/隐藏配合 FAB 与持久化状态。
- **Stability & Performance**: 减少误导性的"SYSTEM NOT READY"日志；提升媒体布局适应性；启用 NEON 并减少 `Mat` 拷贝；容忍 CPU 采样中 `/proc/stat` 权限拒绝。

## [1.2.0] - 2026-02-09

<a id="added-4"></a>
### Added（4）
- **System**: `PermissionStateMachine`，用于 Android 13+ 的健壮权限处理。
- **Logging**: `AppLog` / `FileLogSink` / `NativeLog` 双路径存储与自动回滚。
- **Monitoring**: `StatsRepository`，通过 `StatusService` 实现实时 FPS/CPU/MEM 监控。

<a id="changed-4"></a>
### Changed（4）
- **UI**: 改进 `LogViewerActivity`，支持导出与敏感数据脱敏。
- **Camera**: 动态摄像头发现与热插拔支持。

## [1.1.0] - 2026-02-09

<a id="added-5"></a>
### Added（5）
- **Compatibility**: 优化 `AndroidManifest.xml` 以适配非工业 Android 设备。
- **Debug**: 修复 `main.cpp` CLI 构建问题。

<a id="fixed-2"></a>
### Fixed（2）
- **Build**: 向 `main.cpp` 添加 `<string>` 头文件以修复编译错误。
