# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

> **关于版本号重置的说明 (Version Reset Notice)**
> 在项目早期的探索与架构验证阶段，曾短暂使用过 `1.x` 和 `2.0.0-rcX` 序列的版本号。随着 Windows 端向 Web SPA 架构（React + Vite + 本地 HTTP 服务）的全面迁移，以及 Android 与 Native 核心层的彻底重构，项目进入了实质性的 Beta 测试阶段。为了准确反映项目当前的成熟度与演进主线，版本号已从 `v0.1beta0` 开始重新校准，并统一收敛了全端的版本口径。

## [Unreleased]

### Fixed
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

### Code Review (2026-06-22, Round 2)
> 持续代码审查跟踪表。Round 1（HttpFacesServer）所有 8 项已修复。Round 2 覆盖 Engine 核心、JNI/Android、测试、Web 前端。

#### 上轮修复确认
| # | Round 1 问题 | 修复 commit | 状态 |
|---|------------|-------------|------|
| CR-01 | `escapeJsonString` 控制字符未转义 | `4a13def` | ✅ Fixed |
| CR-02 | `onCameras` 手工拼接 JSON 缺安全头 | `4a13def` | ✅ Fixed |
| HR-01 | 无 RAII socket 封装 | `b83545a` | ✅ Fixed |
| HR-02 | `onSettings` 手工拼接 JSON | `b83545a` | ✅ Fixed |
| HR-03 | `onModels` 文件路径泄露 | `b83545a` | ✅ Fixed |
| MR-01 | 8 处魔法数字 | `acd6f93` | ✅ Fixed |
| MR-02 | MjpegSession 空转循环 | `acd6f93` | ✅ Fixed |
| MR-03 | `readFileBinary` 低效实现 | `acd6f93` | ✅ Fixed |

#### 本轮新发现

##### CRITICAL

**Engine 核心**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-03 | `src/cpp/Engine.cpp:179` | `const_cast` 绕过 NV21 常量性 — `cv::Mat` 可能修改 const 数据，UB | 🟢 Resolved (uncommitted) |
| CR-04 | `src/cpp/Engine.cpp:536,582` | `bioAuth->initialize()` 失败后继续执行 → 后续 `verifyMulti()` 崩溃 | ✅ `c4724e6` |
| CR-05 | `src/cpp/Engine.cpp:184,252` | `thread_local cv::Mat` 异常提前返回时残留旧状态 → 不确定性恢复 | 🟢 Resolved (uncommitted) |

**JNI/Android**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-06 | `src/cpp/native-lib.cpp:387,464` | 重复 `JNIEXPORT jboolean JNICALL` 声明属性 → MSVC/clang 编译失败 | ✅ `c4724e6` |
| CR-07 | `src/cpp/native-lib.cpp:86` | `sendRecognitionResult` 无 `ExceptionCheck` → JNI env 中毒后静默失败 | 🟢 Resolved (uncommitted) |
| CR-08 | `src/java/.../NativeBridge.java:12` | `nativeInitFile(String)` 声明无 C++ 实现 → `UnsatisfiedLinkError` 崩溃 | ✅ `c4724e6` |
| CR-09 | `src/cpp/native-lib.cpp:110,269` | `g_activity` 无同步写（UI线程）读（引擎线程）→ 数据竞争 | 🟢 Resolved (uncommitted) |

**Web SPA**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-10 | `web/src/http.ts:104` | 缓存类型不安全转型 `as T` — 旧缓存版本产生静默垃圾数据 | 🟢 Resolved (uncommitted) |
| CR-11 | `web/src/http.ts:139` | API 响应 `as T` 无运行时校验 → 后端格式变更时静默出错 | 🟢 Resolved (uncommitted) |
| CR-12 | `web/src/AppStore.tsx:23,83` | `updateServerSettings` 参数 `patch: unknown` → 任何拼写错误通过编译 | 🟢 Resolved (uncommitted) |
| CR-13 | Web 全局 | 无 React Error Boundary → 任何组件崩溃导致整个 SPA 白屏 | ✅ `c4724e6` |

**测试**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-14 | `tests/cpp/test_int8_quantization.cpp` | 6 个 INT8 测试静默通过（模型文件 gitignored → 永远 skip），其中 2 个含未实现 TODO | 🟡 Partially fixed in `c4724e6` |
| CR-15 | `tests/cpp/face_infer_unit_tests_main.cpp:44` | 3 个 test_video_manager 函数声明但未注册到 cases[] → 死代码永不执行 | 🔴 Open |
| CR-16 | `tests/cpp/core_unit_tests_main.cpp:63` + `win_unit_tests_main.cpp:41` | `test_http_faces_server_path_validation` 注册在两个测试套件 → 重复执行 | 🔴 Open |

##### HIGH

**Engine 核心**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-04 | `src/cpp/src/Engine.cpp:425` | `updateInferenceThrottle()` 覆盖检测/识别两套节流为相同值 | ✅ 已修 |
| HR-05 | `src/cpp/src/Engine.cpp:688` | `fprintf/fclose` 每 30 帧阻塞写入 CSV（热路径 IO） | ✅ 已修 |
| HR-06 | `src/cpp/src/Engine.cpp:664` | 百分位计算 `v[-1]` 越界风险（`v.size()==0` 时） | ✅ 已修 |
| HR-07 | `src/cpp/src/Engine.cpp:761` | `processFrame(cv::Mat&)` 用可变引用但实际只读 → 误导性签名 | ✅ 已修 |

**JNI/Android**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-08 | `src/cpp/native-lib.cpp:60` | `JNI_OnUnload` 未 join 引擎线程 → 库卸载后线程访问释放代码 | 🔴 Open |
| HR-09 | `src/java/.../Camera2CaptureController.java:315` | `imageReader` 在 stop() 后 NPE 风险（异步 onOpened 竞争） | 🔴 Open |
| HR-10 | `src/java/.../CameraXCaptureController.java:89,161` | `provider` 字段读写无同步（set 在 lambda，get 在 stop） | 🔴 Open |
| HR-11 | `src/java/.../MainActivity.java:2337` | `nativeInit(-1,...)` 硬编码 cameraId -1，应为 `selectedCameraId` | 🔴 Open |
| HR-12 | `src/java/.../FileLogSink.java:78` | 持锁时 `SystemClock.sleep(10)` → 阻塞所有日志写入 | 🔴 Open |

**Web SPA**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-13 | `web/src/AppStore.tsx:71` | `refreshServerSettings` 耦合 UI toast → 不可测试、不能无头调用 | 🔴 Open |
| HR-14 | 全局 | `(e as Error)?.message` 模式重复 ~15 次 → `throw "string"` 时显示 "undefined" | 🔴 Open |
| HR-15 | `web/src/SplashPage.tsx:29` | useEffect 缺依赖 `refreshServerSettings`/`prefs.startPage` → 导航行为过时 | 🔴 Open |

**测试**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-16 | `tests/cpp/test_face_infer_outcome_json.cpp` | Null 守卫测试仅传 nullptr → 未覆盖部分结构/混合有效数据场景 | 🔴 Open |
| HR-17 | `tests/win/test_face_metrics.cpp` | `accumulate()` 函数定义在匿名空间中测试自身 → 非生产代码 | 🔴 Open |
| HR-18 | 多数测试文件 | `return false` 无诊断信息 → 失败时无法定位断言点 | 🔴 Open |
| HR-19 | `tests/cpp/mpp_decoder_stub.cpp` | 编译占位文件放在 tests/ 中 → 应移至 deps/stubs | 🔴 Open |

##### MEDIUM

| # | 模块 | 问题 | 状态 |
|---|------|------|------|
| MR-04 | `src/cpp/Engine.cpp` | 10+ 硬编码魔数（1200ms/0.3f/3帧/650ms/800ms/60s 等） | 🔴 Open |
| MR-05 | `src/cpp/Engine.cpp:511` | 两个 `initialize()` 重载间 ~40 行逐字重复 | 🔴 Open |
| MR-06 | `src/cpp/Engine.cpp:1275` | `catch(...){}` 静默吞异常无日志 | 🔴 Open |
| MR-07 | `src/cpp/native-lib.cpp:555` | `thread_local lastSeq` 线程切换时重复发送帧 | 🔴 Open |
| MR-08 | `src/java/.../MainActivity.java:245` | `selectedCameraId` 用 int → 部分设备 USB 相机 ID 非纯数字 | 🔴 Open |
| MR-09 | `web/src/http.ts:40` | `memoryCache` Map 无驱逐策略 → 无限增长 | 🔴 Open |
| MR-10 | `web/src/cameras.ts,actions.ts,models.ts,settings.ts` | API 路径字符串在 4 个模块中重复分散 | 🔴 Open |
| MR-11 | `web/src/index.css:54` | `#root { width: 1126px }` + Vite 模板残留 CSS → 宽屏溢出 | 🔴 Open |
| MR-12 | 测试全局 | 大量缺失测试覆盖：FaceDatabase, FaceRecognizer, WinConfig, YoloFaceDetector, ArcFaceEmbedder 等 15+ 生产组件零用例 | 🔴 Open |
| MR-13 | 测试全局 | 资源清理不完整（tempfile 泄漏、部分创建后未清理） | 🔴 Open |

##### Repository Hygiene（状态更新）
| # | 问题 | 状态 |
|---|------|------|
| RH-01 | 403 个 AI 生成远程僵尸分支 | 🔴 Open |
| RH-02 | 本地 bugfix 未推送 | ✅ Fixed |
| RH-03 | `.codegraph/` 未加入 `.gitignore` | 🔴 Open |
| RH-04 | CI `yolo_face` 引用 | 🔴 Open |
| RH-05 | Gradle 9.0-milestone-1 实验版 | 🟡 Monitoring |
| RH-06 | webroot JS bundle 跟踪 | 🟡 Known |
| RH-07 | `tests/cpp/ncnn_precision_test.cpp:222` 检测测试即使推理失败也 pass | 🔴 Open |

##### CRITICAL

**帧管线 & 媒体捕获**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-17 | `src/win/src/FramePipeline.cpp:191–end` | **88 个缺失闭合大括号** — 每个函数体从 line 191 起全部缺少 `}`，无法编译 | 🔴 Open |
| CR-18 | `src/win/src/FramePipeline.cpp:271` | **未声明变量 `prevDesired`** — 赋值给一个从未定义/声明的变量 | 🔴 Open |
| CR-19 | `src/win/src/FramePipeline.cpp:473,474` | **未声明变量 `d` 和 `f`** — 用于重连逻辑，未 declaration | 🔴 Open |
| CR-20 | `src/win/src/FramePipeline.cpp:477,480` | **未定义函数 `openOnce()`** — 调用不存在的方法 | 🔴 Open |
| CR-21 | `src/win/src/FramePipeline.cpp:416` | `tryGetRenderState()` 无锁读取 `render_` — `processLoop()` 写端持有 `renderMu_`，读端无锁 | 🔴 Open |
| CR-22 | `src/win/src/FramePipeline.cpp:597` | `processLoop()` busy-wait `if (!hasFrame_) continue;` — 无帧时 100% CPU 空转 | 🔴 Open |
| CR-23 | `src/win/src/FramePipeline.cpp:449` | `waitFacesSeqChanged()` 忽略 `timeoutMs` — 从不真正等待，函数名与行为不符 | 🔴 Open |

**人脸识别管线**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-24 | `src/win/src/FaceDatabase.cpp:82` | `persons_` map 数据竞争 — `updateMean()` 写入时 `identify()` 在 FaceRecognizer 中并发迭代 | 🔴 Open |
| CR-25 | `src/cpp/src/FaceSearch.cpp:106` | `reset()` 失败后索引状态损坏 — `norms_` 部分填充而 `entries_` 仍为旧数据 | 🔴 Open |

**配置 & 加密系统**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-26 | `src/win/src/JsonLite.cpp:248` | JSON 递归解析无深度限制 → 深度嵌套输入导致栈溢出（可被 API 端点利用） | 🔴 Open |
| CR-27 | `src/win/src/WinCrypto.cpp:144` | AES-GCM 密钥材料未 `SecureZeroMemory` → 内存转储/页面文件可恢复密钥 | 🔴 Open |
| CR-28 | `src/win/src/WinJsonConfig.cpp:1357` | `reloadFromDisk` TOCTOU 竞态 — 两次锁之间 `updateFromJsonBody` 的变更被静默覆盖 | 🔴 Open |
| CR-29 | `src/win/src/WinCrypto.cpp:181` | AES-256-GCM 未使用 AAD → 攻击者可交换加密字段（如 `postUrl` 与其他字段互换） | 🔴 Open |

##### HIGH

**帧管线 & 媒体捕获**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-20 | `src/win/src/FramePipeline.cpp:498` 多处 | `render_.status` 字符串赋值无锁，同时有读者 | 🔴 Open |
| HR-21 | `src/win/src/MfCamera.cpp:292` | 负 stride 溢出 — MF 返回负值（bottom-up）时转换为大 UINT32,传给 `cv::Mat` 错误 | 🔴 Open |
| HR-22 | `src/win/src/MfCamera.cpp:224` | IMFMediaSource 打开失败时未调用 Shutdown() → 资源泄漏 | 🔴 Open |
| HR-23 | `src/win/src/OverlayRenderer.cpp:51,64` | 每帧全分辨率 clone() (~6MB) + addWeighted → 30fps 时 180MB/s 分配 | 🔴 Open |
| HR-24 | `src/win/src/D3D11Renderer.cpp:732` | `Map()` 失败不触发设备丢失恢复 — 只有 `Present()` 有恢复路径 | 🔴 Open |
| HR-25 | `src/win/src/FramePipeline.cpp:533` | 重连使用陈旧参数 — `d`/`f` 在循环入口一次性捕获，运行中配置变更不生效 | 🔴 Open |

**配置 & 加密**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-26 | `src/win/src/WinJsonConfig.cpp:30` | `readFileAll` 无输入大小限制 → 恶意 config.json 可导致 OOM | 🔴 Open |
| HR-27 | `src/win/src/JsonLite.cpp:414` | `parseJson` 无输入长度限制 | 🔴 Open |
| HR-28 | `src/win/src/WinJsonConfig.cpp:1093` | 备份无轮转（单 `.bak`）→ 配置损坏时备份也损坏，无法回滚 | 🔴 Open |
| HR-29 | `src/win/src/WinJsonConfig.cpp:72` | `moveReplaceWriteThrough` fallback 非原子 → 崩溃后残留零长度文件 | 🔴 Open |
| HR-30 | `src/win/src/WinJsonConfig.cpp:1065` | `buildSettingsJson` 错误信息泄露到 JSON 输出 → 可能通过 HTTP 响应暴露内部错误 | 🔴 Open |

**人脸识别管线**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-31 | `src/win/src/FaceDetector.cpp:21` | CascadeClassifier detectMultiScale 非线程安全（内部可变状态） | 🔴 Open |
| HR-32 | `src/cpp/src/FaceSearch.cpp:42,55` | NEON vld1q_f32 需 16 字节对齐，`std::vector<float>` 仅 4 字节 → Cortex-A17 上 SIGBUS | 🔴 Open |
| HR-33 | `src/win/src/FaceRecognizer.cpp:97` | `identifyThreshold_` 读写数据竞争 — `double` 非原子 | 🔴 Open |
| HR-34 | `src/cpp/src/FaceInferStages.cpp:440` | 错误时 `std::move` 导致 `ctx.galleryEntries` 为空，阻塞重试 | 🔴 Open |

**构建系统**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-35 | `CMakeLists.txt` 全局 | 无 `install()` 目标 → `cmake --install` 无输出 | 🔴 Open |
| HR-36 | `CMakeLists.txt:111` | 全局 `include_directories()` 污染所有 target，包括 OpenCV 跳过时仍继承头文件路径 | 🔴 Open |
| HR-37 | `CMakeLists.txt:85` | 使用已废弃 `add_definitions()` 替代 `add_compile_definitions()` | 🔴 Open |
| HR-38 | `CMakeLists.txt:305` | `file(GLOB_RECURSE)` 固件源文件 → 新文件不被自动检测 | 🔴 Open |

##### MEDIUM

| # | 模块 | 问题 | 状态 |
|---|------|------|------|
| MR-14 | `src/cpp/src/FaceInferStages.cpp:426` | `loadGallery()` 每次 `runFaceInferOnce()` 重新解析全部模板文件（每帧磁盘 IO） | 🔴 Open |
| MR-15 | `src/cpp/include/ModelRegistry.h:41` | Singleton 无内部同步 → 并发 `createDetector()`/`createEmbedder()` Map 竞争 | 🔴 Open |
| MR-16 | `src/win/src/FaceDatabase.cpp:94` | `e.count` 使用 `int` → 溢出风险 | 🔴 Open |
| MR-17 | `src/win/src/WinConfig.cpp` | `model.int8Enabled` 在 INI 加载/保存中缺失 | 🔴 Open |
| MR-18 | `CMakeLists.txt:557` | `ncnn_precision_test` CRT 缺少 Debug 后缀 → Debug 构建链接错误 | 🔴 Open |
| MR-19 | `CMakeLists.txt` 全局 | 10+ 个 OpenCV lib 列表重复；`/utf-8` 和 `NOMINMAX` 各重复 9+ 次 | 🔴 Open |
| MR-20 | `CMakeLists.txt:106` | `-fPIE` 放在链接器标志中（应为编译器标志） | 🔴 Open |
| MR-21 | `CMakeLists.txt` | 无 `CMakePresets.json` → 4+ 构建配置需开发者手动输入 | 🔴 Open |
| MR-22 | `D3D11Renderer.cpp:493` | `pushFrameTime()` 每帧 O(N) erase + O(N log N) sort 4096 元素 | 🔴 Open |
| MR-23 | `MfCamera.cpp:103` | `Impl*` 裸指针 `new/delete` → 构造函数抛出后泄漏 | 🔴 Open |
| MR-24 | `FramePipeline.cpp:576` | 每帧 `FrameLogEntry` 分配释放 → 热路径内存抖动 | 🔴 Open |

##### CRITICAL (Round 4)

**BioAuth & MotionDetector**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-30 | `BioAuth.h:68` + `BioAuth.cpp` 全局 | **零线程安全** — `faceCascade`, `faceRecognizer`, `isModelLoaded` 无任何同步，并发 `verify()` 数据竞争 | 🔴 Open |
| CR-31 | `BioAuth.cpp:101,186` | **置信度归一化公式本质错误** — LBPH 距离无界，`(100.0 - conf) / 100.0` 截断到 [0,1]，阈值 0.92 拒绝几乎所有真实匹配 | 🔴 Open |
| CR-32 | `MotionDetector.h:28` + `MotionDetector.cpp` 全局 | **零线程安全** — `prevFrame`/`motionMask`/`grayFrame` 等可变状态无锁，并发 `detect()` 写 + `getMotionMask()` 读竞态 | 🔴 Open |
| CR-33 | `MotionDetector.cpp:43` | `getMotionMask()` 返回浅拷贝 `cv::Mat` → 调用者持有共享缓冲区引用时，后续 `detect()` 写入同一缓冲区 | 🔴 Open |

**Android 构建系统**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-34 | `app/build.gradle:64` | **硬编码绝对路径** `D:/ProgramData/NCNN/ncnn-20260113-android-vulkan` → 其他机器构建失败 | 🔴 Open |
| CR-35 | `app/build.gradle:108` | `minifyEnabled false` → release APK 无 R8 混淆/优化/压缩，可被轻易逆向 | 🔴 Open |
| CR-36 | `app/build.gradle:109` | 引用的 `proguard-rules.pro` 文件不存在 → 开启 R8 后构建立即失败 | 🔴 Open |

**脚本工具**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-37 | `.github/workflows/ci.yml:287` | `quantize_ncnn_int8.py --model yolo_face` — `yolo_face` 不是有效 preset（应为 `scrfd`）→ CI 量化步骤**从未成功执行** | 🔴 Open |
| CR-38 | `scripts/verify_opencv_host.bat:42` | `OPENCV_CONTRIB_ROOT` 未定义检查 → CMake 收到空字符串，静默跳过 `opencv_face` 模块 | 🔴 Open |
| CR-39 | `scripts/verify_faces_test_set01.bat:45` | 同上 — `OPENCV_CONTRIB_ROOT` 无验证 | 🔴 Open |
| CR-40 | `scripts/stability_switch_50_adb.ps1:57` | 使用已废弃 `dumpsys window windows`（Android 12+ 替代为 `dumpsys window displays`）→ 新设备返回空数据 | 🔴 Open |

**FaceInferencePipeline**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-41 | `src/cpp/src/FaceInferStages.cpp:451` | `assumeL2Normalized = true` 硬编码 → 若模型输出未 L2 归一化，检索结果静默错误 | 🔴 Open |
| CR-42 | `src/cpp/src/ModelRegistry.cpp:18` | `ensureBuiltinRegistered()` 无 `mutex`/`call_once` 保护 → 并发首次调用时 `unordered_map` 数据竞争 | 🔴 Open |

##### HIGH (Round 4)

| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-39 | `BioAuth.cpp:95,183` | 人脸 ROI 无边界限制 → 靠近边缘的检测框导致 `cv::Mat` 越界 / OpenCV assert 崩溃 | 🔴 Open |
| HR-40 | `BioAuth.cpp:72,129` | `detectMultiScale` 魔数 `1.1, 3, Size(60,60)` — 60×60 最小脸在 640×480 上需占 1.6% 画面，广角监控漏检小脸 | 🔴 Open |
| HR-41 | `MotionDetector.cpp:16` | 假设输入 BGR — `COLOR_BGR2GRAY` 在灰度/RGBA 输入时抛出异常 | 🔴 Open |
| HR-42 | `app/build.gradle` 多处 | 所有依赖版本硬编码为字面量字符串，无版本目录（version catalog）→ 版本更新需逐行修改 | 🔴 Open |
| HR-43 | `gradle.properties:19` | `org.gradle.workers.max=1` 强制单线程执行 → 多核机器构建性能浪费 | 🔴 Open |
| HR-44 | `scripts/run-web-e2e.ps1:31` | Vite 启动后固定 `Start-Sleep 3`，无就绪检查 → 慢 CI runner 上 Cypress 连不上 | 🔴 Open |
| HR-45 | `scripts/quantize_ncnn_int8.py:61` | `C:/ncnn/tools/quantize/` 硬编码回退路径 → 非标准安装用户获得误导性 "not found" | 🔴 Open |
| HR-46 | `scripts/build_android.bat:24` | NDK 版本号硬编码 `27.0.12077973` / `25.1.8937393` → 其他 NDK 版本导致 `NDK_ROOT` 未设置 | 🔴 Open |
| HR-47 | `FaceInferencePipeline.cpp:112` | `makeAuditFilename` 使用毫秒时间戳 → 同毫秒多调用的审计文件静默覆盖 | 🔴 Open |
| HR-48 | `FaceInferStages.cpp:232,362` | INT8→FP32 降级无日志 → 运维人员不知 INT8 未生效 | 🔴 Open |
| HR-49 | `FaceInferStages.cpp:92` | `loadGalleryDir` 每次 `runFaceInferOnce` 重新解析全部模板文件 → 5000+ 模板时大量 I/O | 🔴 Open |

##### MEDIUM (Round 4)

| # | 模块 | 问题 | 状态 |
|---|------|------|------|
| MR-25 | `BioAuth.cpp:72,129` | `detectMultiScale` 参数不可配置 | 🔴 Open |
| MR-26 | `BioAuth.cpp:69` | 无条件全局 `equalizeHist` → 低光照放大噪声，良好光照冲淡对比度 | 🔴 Open |
| MR-27 | `Config.h:44,47` | `MOTION_THRESHOLD=25`, `MIN_MOTION_AREA=500` 为 640×480 的绝对像素值 → 分辨率变化后灵敏度过高/低 | 🔴 Open |
| MR-28 | `MotionDetector.cpp` | `cv::Mat` 成员对象生命周期内永不释放 → RK3288 受限内存下浪费 | 🔴 Open |
| MR-29 | `app/build.gradle:64` | `RK_ENABLE_NCNN=ON` 无条件设置，即使 `skipOpencv=true` | 🔴 Open |
| MR-30 | `gradle.properties:14` | `android.enableJetifier=true` — 所有依赖已使用 AndroidX，无需 Jetifier，拖慢构建 | 🔴 Open |
| MR-31 | `gradle.properties` | 未开启 Gradle 构建缓存/配置缓存 | 🔴 Open |
| MR-32 | `app/build.gradle:72` | ABI split 开启 `universalApk true` → 拆包减大小的收益被抵消 | 🔴 Open |
| MR-33 | `scripts/quantize_ncnn_int8.py:96` | `run_ncnn2table` 未校验 preset 必须字段 → 新增模型时 `KeyError` 崩溃 | 🔴 Open |
| MR-34 | `scripts/bench_camera_adb.sh:58` | `wait_for_lines` 超时返回 0 → 调用者获得空数据静默记录 `,,,,` | 🔴 Open |
| MR-35 | `FaceInferStages.cpp:440` | `std::move(ctx.galleryEntries)` 后状态未指定 → 后续新增 gallery 读取代码静默获取空向量 | 🔴 Open |
| MR-36 | `ModelRegistry.cpp:23` | `static bool g_builtinRegistered` 文件作用域 → 不安全的单例注册守卫 | 🔴 Open |

##### CRITICAL (Round 5)

**适配器（9 个模型适配器全线）**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-43 | 所有 `*Adapter.cpp` | **全部 9 个适配器零线程安全** — `loaded_`/`inner_`/`net_` 无锁读写，并发 `load()` 与 `detect()`/`embed()` 数据竞争 | 🔴 Open |
| CR-44 | `ArcFaceAdapter.cpp:44`, `YoloFaceAdapter.cpp:47` | ncnn 回退 OpenCV 时传递 `.param` 文件 → OpenCV 无法加载此格式，**静默失败** | 🔴 Open |
| CR-45 | `ArcFaceAdapter.cpp:25`, `MobileFaceNetAdapter.cpp:20` | 路径拼接硬编码 `'/'` → Windows 上 `\` 不被识别 | 🔴 Open |

**事件管理 & 日志**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-46 | `EventManager.cpp:57` | `static std::mt19937 gen` 无锁线程不安全 → RNG 内部状态损坏，uid 重复或死锁 | 🔴 Open |
| CR-47 | `Storage.cpp:64` | `appendLog` 无锁并发调用 → 日志行交织或文件损坏 | 🔴 Open |
| CR-48 | `EventManager.cpp:34` | `formatEventJson` JSON 字符串未转义 → 描述/路径含 `"` 等字符时生成非法 JSON | 🔴 Open |

**帧输入管线**
| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| CR-49 | `MppDecoder.cpp:133` | `chunkBuf_` 单个缓冲区被所有 packet 复用 → MPP 未消费时 `fread` 覆写待处理数据，解码损坏 | 🔴 Open |
| CR-50 | `VideoManager.cpp:585` | MPP 回退 OpenCV 时未校验 `cap.isOpened()` → 无帧空转 | 🔴 Open |
| CR-51 | `VideoManager.cpp:660` | 摄像头读取失败无限重试（10ms sleep，无计数器）→ 设备断开后永久空转 | 🔴 Open |

##### HIGH (Round 5)

| # | 文件 | 问题 | 状态 |
|---|------|------|------|
| HR-50 | `LbphAdapter.cpp:8` | `load()` 完全忽略 `modelPath` 且始终成功 → 无法检测加载失败 | 🔴 Open |
| HR-51 | `CascadeAdapter.cpp:33` | 所有检测结果硬编码 `score=1.0f` → 下游阈值逻辑误导 | 🔴 Open |
| HR-52 | `MobileFaceNetAdapter.cpp:48` | `net_.opt.num_threads = 1` 硬编码 → 多核设备推理速度受限 | 🔴 Open |
| HR-53 | 多个 `*Adapter.cpp` | `cv::dnn::readNet` 等可能抛出，无 `try/catch` → 异常导致进程终止 | 🔴 Open |
| HR-54 | `NativeLog.cpp:144` | 持锁时执行阻塞文件 I/O（ensureDir/rotateIfNeeded/write）→ 所有日志线程串行化 | 🔴 Open |
| HR-55 | `NativeLog.cpp:121` | `write()` 返回值未检查 → 部分写入导致日志静默截断 | 🔴 Open |
| HR-56 | `NativeLog.cpp:69` | `ensureDir` 仅按 `/` 分割路径 → Windows 上路径分隔符 `\` 不识别 | 🔴 Open |
| HR-57 | `NativeLog.cpp:95` | 日志轮换非原子 → 崩溃时日志文件损坏 | 🔴 Open |
| HR-58 | `NativeLog.cpp:113` | 每次日志调用 `open/write/close` → 日志密集型路径性能极差 | 🔴 Open |
| HR-59 | `EventManager.cpp:57` | 32 位事件 ID → 约 77k 事件后 50% 碰撞概率 | 🔴 Open |
| HR-60 | `FaceAlign.cpp:15` | NaN bbox → `static_cast<int>(NaN)` 未定义行为 | 🔴 Open |
| HR-61 | `FaceTemplate.cpp:74` | `serializeFaceTemplate` 未更新 `h.dim` → 非 512-dim 嵌入时数据损坏 | 🔴 Open |
| HR-62 | `FileHash.cpp:60` | SHA-256 每次 64 字节 `read()` → 100MB 模型产生 ~160 万次 syscall | 🔴 Open |
| HR-63 | `VideoManager.cpp:549` | `cancelToken` 在 `captureLoop` 中从未被检查 → 取消请求被忽略 | 🔴 Open |
| HR-64 | `VideoManager.cpp:298` | `char chunk[1024*1024]` 栈上分配 1MB → RK3288 栈溢出风险（默认 2MB） | 🔴 Open |
| HR-65 | `MppDecoder.cpp:147,193` | `MPP_ERR_BUFFER_FULL` 时销毁未消费 packet → 解码数据间隙 | 🔴 Open |

##### MEDIUM (Round 5)

| # | 模块 | 问题 | 状态 |
|---|------|------|------|
| MR-37 | `RetinaFaceAdapter.cpp:212` | `generateAnchors()` 每次 `detect()` 调用（输入固定后可预计算） | 🔴 Open |
| MR-38 | `Storage.cpp:68` | `std::endl` 每次日志行强制 `flush` → 低吞吐场景 I/O 放大 | 🔴 Open |
| MR-39 | `EventManager.cpp:26` | 日志文件扩展名 `.json` 但格式实为 JSON Lines → 工具解析失败 | 🔴 Open |
| MR-40 | `MppDecoder.cpp:209` | 循环播放未调用 `mpi->reset()` → DPB 残留旧参考帧 | 🔴 Open |
| MR-41 | `FrameInputChannel.cpp:108` | `timeoutMs=0` 时 `wait_for` 阻塞永久 → API 陷阱 | 🔴 Open |
| MR-42 | `VideoManager.cpp:238` | `open()` 函数 255 行、4 层嵌套、5 个 try/catch → 维护负担 | 🔴 Open |
| MR-43 | `FileHash.cpp:12` | 裸 `uint32_t` 无 `std::` 前缀 → 部分编译器编译失败 | 🔴 Open |
| MR-44 | `FaceAlign.cpp:91` | `estimateAffinePartial2D` 分配 `inliers` 但从不读取 | 🔴 Open |
| MR-45 | `FaceAlign.cpp:83` | 5 点对齐每帧堆分配 `vector` → 静态大小可用数组 | 🔴 Open |
| MR-46 | `FaceTemplate.cpp:78` | header reserve 长度 24 与实际 23 字节偏差 1 | 🔴 Open |

#### Findings Lifecycle Rules
- **🔴 Open** — 已报告未处理
- **🟡 Stale** — 已知但不紧急 / 可接受
- **🟡 Monitoring** — 持续观察
- **🟢 Resolved** — 已修复但未提交
- **✅ Fixed** — 已提交修复
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

### Added
- **Build System**: 向 Windows / CLI 的 CMake 构建链路中注入了 `BUILD_ID` 宏，使得二进制产物、评估工具（`win_face_eval_cli` / `inference_bench_cli`）以及应用启动日志都能输出一致的版本号。
- **Documentation**: 在 `CREDITS.md` 中新增了完整的**模型台账 (Model Inventory)**，明确了 OpenCV DNN 模型和 LBP 级联分类器的来源、部署路径与开源许可证。

### Changed
- **Versioning**: 升级全端版本号至 `v0.1beta1` (Android `versionCode 3`)，彻底消除 Android 构建、CMake 产物与文档之间的版本漂移。
- **Documentation**: 全面校准 `DEVELOP.md`，更新了包含 `web/` 的最新目录结构与前端技术栈（React 18 + AntD 5）。
- **Configuration**: 明确 `%APPDATA%\rk_wcfr\config.json` 为 Windows 端配置的唯一事实来源 (Source of Truth)，并在旧版 `config/windows_camera_face_recognition.ini` 中添加了废弃/迁移警告及模型下载指引。

### Documented
- **README 待办（完成项 37–38）**：从 `README.md` 清理迁移至此处归档。
  - 37. **[P0] 文档全量校准：修订 README/CHANGELOG/DEVELOP/CREDITS 与 `docs/`，确保与当前项目一致**（更新目录树、补齐模型台账、校准配置指引）
  - 38. **[P0] 版本号升级到 `v0.1beta1`：统一代码/构建/文档口径**（Windows/CLI 增加 BUILD_ID 宏注入并打印，Android versionCode 升级）

## [0.1.0-beta.0] - 2026-04-15

### Added
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

### Removed
- **Legacy SDK**: 移除 `ColorOsSdkBridge`、`PlayIntegrityChecker`、`GmsDetector`、`PrivilegedCommandGate` 与 `DevicePolicy`，消除无效依赖。
- **Security**: 移除 `SecurityEventLogger` 作为 SDK 清理的一部分。

### Changed
- **Device Profiling**: 简化 `DeviceProfile`，聚焦于硬件数据采集（Build info、Memory/Storage）。
- **Documentation**: 在 `DEVELOP.md` 第 4.6 节新增简化版设备画像任务（待实现）。
- **Structure**: 重构 `DEVELOP.md` 为"概述 → 环境 → 核心开发 → 高级/排障"结构。
- **Content**: 扩展 V4L2、MPP、RKNN 与 DRM/KMS 的技术细节。
- **Style**: 强制执行严格 Markdown 标准与中英双语术语。
- **日志目录**: 错误日志目录统一为 `ErrorLog/`（区分大小写），不再兼容 `errorlog/`。

### Documented
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

### Added
- **System**: `PermissionStateMachine`，用于 Android 13+ 的健壮权限处理。
- **Logging**: `AppLog` / `FileLogSink` / `NativeLog` 双路径存储与自动回滚。
- **Monitoring**: `StatsRepository`，通过 `StatusService` 实现实时 FPS/CPU/MEM 监控。

### Changed
- **UI**: 改进 `LogViewerActivity`，支持导出与敏感数据脱敏。
- **Camera**: 动态摄像头发现与热插拔支持。

## [1.1.0] - 2026-02-09

### Added
- **Compatibility**: 优化 `AndroidManifest.xml` 以适配非工业 Android 设备。
- **Debug**: 修复 `main.cpp` CLI 构建问题。

### Fixed
- **Build**: 向 `main.cpp` 添加 `<string>` 头文件以修复编译错误。
