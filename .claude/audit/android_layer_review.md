# P2.3 模块审查报告 — Android 层代码审查

## 模块概览

| 项目 | 统计 |
|------|------|
| Java 源文件数 | 28 个（src/java/com/example/rk3288_opencv/） |
| Java 总行数 | 8,729 行 |
| C++ JNI 文件 | 1 个（native-lib.cpp, 766 行） |
| 布局文件 | 7 个（1,685 行） |
| 清单/资源 | 2 个（63 行） |
| 最大文件 | MainActivity.java (3,155 行，项目之最) |
| 注释率 | 约 1%（全局扫描数据） |

---

## 逐文件审查表

### MainActivity.java (3,155 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 2 | God Activity 反模式。UI 绑定、生命周期、摄像头控制、引擎初始化、RTMP 推流、文件 I/O、JNI 桥接、媒体回放、监控、看门狗定时器、权限管理、统计显示全部混杂。虽然部分逻辑委托给了 MonitoringCoordinator，但 MainActivity 仍承担过多职责。 |
| 架构模式 | 2 | MVC 但没有明确的 Controller 分离。大量匿名内部类回调。配置变更通过销毁/重建 View（`onConfigurationChanged` 第 1510-1590 行）实现，成本高且脆弱。 |
| 错误处理 | 3 | 广泛使用 try/catch，但大量错误被静默吞掉（`ignored`）。采集恢复有看门狗机制。首帧超时和推流停滞检测做得好。 |
| 安全 | 3 | JNI 帧处理接口安全。RTMP URL 无验证（第 2874-2901 行）。Mock URL 作为文件路径直接传递给引擎，无路径遍历防护。 |
| 性能 | 3 | 帧循环在独立 HandlerThread 上。Stats 采样在独立线程。但 `onConfigurationChanged` 重建整个视图树导致性能损失。看门狗轮询（500ms 间隔）可接受。 |
| 可测试性 | 1 | 强依赖 Android 框架。无依赖注入。大量 `static` 方法和全局状态。几乎不可单元测试。 |

### Camera2CaptureController.java (367 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 4 | Camera2 API 使用清晰。`onImageAvailable` 处理帧推送良好。 |
| 架构模式 | 3 | 实现 `CaptureController` 接口，使用回调模式。 |
| 错误处理 | 3 | 异常被捕获但不总是传播给 observer。 |
| 安全 | 3 | `@SuppressLint("MissingPermission")` 在方法级别（第 74 行）——应在调用点检查。 |
| 性能 | 3 | 2 缓冲区 ImageReader，`acquireLatestImage` 避免背压。专用线程。 |
| 可测试性 | 2 | 强耦合 CameraManager，难以 mock。 |

### CameraXCaptureController.java (385 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 3 | CameraX 集成复杂，回退逻辑（1080→720 第 125-135 行）清晰。 |
| 架构模式 | 3 | 基于接口，但强耦合 LifecycleOwner。 |
| 错误处理 | 3 | 合理的 try/catch。1080 绑定失败时优雅回退到 720。 |
| 安全 | 3 | 一般。 |
| 性能 | 3 | 单线程执行器，`STRATEGY_KEEP_ONLY_LATEST` 背压。 |
| 可测试性 | 2 | CameraX API 需要真机。 |

### MonitoringCoordinator.java (511 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 4 | 清晰的状态机模式，`Inputs`/`Decision`/`Effect` 分离良好。 |
| 架构模式 | 4 | 好的架构决策：将状态管理与 UI 解耦。Effect 模式优雅。 |
| 错误处理 | 4 | 穷举状态处理。恢复有重试和退避。 |
| 安全 | N/A | 纯业务逻辑。 |
| 性能 | 4 | `synchronized` 方法——对低频状态转换可接受。 |
| 可测试性 | 4 | 纯逻辑，无 Android 依赖。最佳可测试类之一。 |

### FeatureTemplateEncryptedStore.java (399 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 5 | 最清晰的 Java 文件之一。Envelope 编解码组织良好。 |
| 架构模式 | 4 | 静态工具风格，但封装良好。 |
| 错误处理 | 4 | 详细错误码枚举，正确的 GCM 解密错误处理。损坏时隔离。 |
| 安全 | 5 | **项目亮点**：Android KeyStore、AES-256-GCM、AAD 绑定 userId/modelVersion/schemaVersion、安全随机 IV、原子写入（tmp+rename+bak）。 |
| 性能 | 4 | 每次读/写有 SHA-256 开销，可接受。 |
| 可测试性 | 3 | 静态方法，需要 mock Android KeyStore。 |

### FfmpegRtmpPusher.java (114 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 3 | 合理，但文件与原始管道的逻辑路径混合。 |
| 架构模式 | 3 | 简单的 FFmpegKit 反射包装器。 |
| 错误处理 | 3 | 捕获 Throwable，错误传播有限。 |
| 安全 | 2 | **S1 报告高危 H2**：`escapeFFmpegArgument` 方法存在但未使用（第 72-76 行的字符串构建器是死代码）。实际执行使用安全的 `String[]` 数组。RTMP URL 未做有效性验证。 |
| 性能 | 4 | `executeAsync` 非阻塞。 |
| 可测试性 | 3 | 简单类，但反射调用难以测试。 |

### SensitiveDataUtil.java (83 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 5 | 清晰，文档完善。威胁模型注释优秀。 |
| 架构模式 | 4 | 单一职责的工具类。 |
| 错误处理 | 4 | 空安全，空字符串安全。 |
| 安全 | 5 | **项目亮点**：掩码手机号、身份证、GPS、密码/Token/Secret、邮箱。正则表达式精心构造。 |
| 性能 | 3 | 在完整字符串上顺序正则匹配——大日志可能慢。 |
| 可测试性 | 5 | 纯函数，极易测试。 |

### AppLog.java (299 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 4 | 清晰的日志抽象，含级别、模块、掩码。 |
| 架构模式 | 4 | 基于 FileLogSink 的静态门面模式。 |
| 错误处理 | 3 | `needsMasking` 启发式检查（搜索数字、@、关键词）可能有假阴性。 |
| 安全 | 4 | 写入磁盘前掩码。但 `needsMasking` 对短字符串效率低（第 200-218 行逐字符扫描）。 |
| 性能 | 3 | ThreadLocal 日期格式化器。每行日志分配 StringBuilder。 |
| 可测试性 | 3 | 静态方法。 |

### StatusService.java (241 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 3 | 基本清晰。 |
| 架构模式 | 2 | 强耦合 StatsRepository 单例。 |
| 错误处理 | 2 | 大量静默捕获。 |
| 安全 | 3 | 前台通知使用 IMMUTABLE flag（第 125 行）。 |
| 性能 | 3 | 500ms 间隔的常量轮询。 |
| 可测试性 | 1 | Android Service，难以单元测试。 |

### App.java (34 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 5 | 简短清晰。初始化日志、设备识别、统计。Debug 模式下启用 StrictMode。典范做法。 |

### PermissionStateMachine.java (158 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | 清晰的状态机（INIT/REQUESTING/GRANTED/DENIED_TEMP/DENIED_PERM/SAFE_MODE）。好的 Listener 模式。合理处理永久拒绝。 |

### StatsRepository.java (478 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 3 | FPS/Capture/Latency/CPU/Mem 采样器分开但内嵌。使用 /proc/self/stat 做 CPU 采样，含优雅的回退机制。 |
| 架构模式 | 2 | 单例模式。所有内部 tracker 都是 private static final class。 |
| 错误处理 | 3 | 各个采样器独立的 try/catch。CPU 采样器检测 ErrnoException.EACCES 并优雅回退到 `getElapsedCpuTime()` API。 |
| 安全 | 3 | /proc/self/stat 读取是进程私有的，安全。 |
| 性能 | 4 | 基于 ReentrantReadWriteLock 的读写锁。500ms 采样间隔合理。 |
| 可测试性 | 2 | 单例，强依赖 Android 系统服务。 |

### LogViewerActivity.java (675 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 3 | 功能齐全的日志查看器。支持选择/导出/删除/过滤。使用 SAF 导出（好的安全实践）。导出时对敏感数据做再掩码。logcat 捕获通过 `ProcessBuilder("logcat")` 实现（Android 权限受限）。 |

### LogDetailActivity.java (736 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 3 | 大文件的流式加载。正则过滤含超时/行数/字符数限制。行号切换。筛选有 job 序列号避免竞态。路径验证防止越权读取。 |

### LogAdapter.java (190 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 3 | 标准 RecyclerView 适配器，支持多选。选择状态在数据刷新时保留。 |

### MainScreenBinder.java (231 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | 干净的 View 绑定模式。Callback 接口定义清晰。从 MainActivity 中剥离 UI 绑定逻辑。 |

### DeviceProfile.java / DeviceClassifier.java / DeviceRuntime.java (总计 219 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | 设备识别层组织良好。`DeviceClassifier` 通过硬件/manufacturer/brand 特征识别设备类别（RK3288、OPPO、VIVO、XIAOMI、HUAWEI、GLOBAL_GMS）。含 RK3288 专门检测逻辑（硬件+Android 7+userdebug+2GB RAM+8GB ROM）。 |

### CacheCleaner.java (83 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 3 | 干净的退出时缓存清理。使用 Deque 实现 DFS 遍历确保先删文件后删目录。 |

### FileLogSink.java (78 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 2 | 所有 IOException 静默吞掉。日志写入失败无 fallback。轮转有 10ms sleep（第 75 行）——无锁段内。 |

### InferenceThrottleAutoTuner.java (87 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | 基于 CPU/延迟/FPS 的自适应推理节流。80-500ms 范围，30ms 步进增加，20ms 步进减少。1500ms 冷却期。清晰。 |

### 辅助接口/模型（CaptureController, CaptureObserver, CaptureScheme, DeviceClass, ExternalFrameSink, RecognitionEvent, RotationDegreesProvider, StatsSnapshot, NativeBridge）

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | 接口定义和值对象设计合理简洁。`NativeBridge.java` 集中声明 JNI 方法。 |

### native-lib.cpp (766 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 代码可读性 | 3 | C++ 和 JNI 代码混合。`sendRecognitionResult` 中的线程附加逻辑合理。 |
| 错误处理 | 3 | 部分 `GetStringUTFChars` 后有异常检查，但不一致（`nativeInit` 第 288-298 行）。 |
| 安全性 | 3 | GlobalRef 管理适当，但如果 AttachCurrentThread 失败会有泄漏（第 84 行 return 前未释放）。 |
| 线程安全 | 4 | 引擎线程、activity 引用、预览窗口都有 mutex 保护。 |
| 性能 | 4 | DirectBuffer 零拷贝路径 + byte[] fallback。帧序列去重避免重复渲染。 |
| 可测试性 | 2 | JNI 代码需要原生环境。 |

### 布局 XML（7 个文件，1,685 行）

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 3 | `activity_main.xml`（615 行）和横屏变体（574 行）体量大但结构清晰。横竖屏两套布局导致维护负担加倍。`item_recognition_event.xml`（23 行）和 `layout_status_overlay.xml`（17 行）简洁。 |

### AndroidManifest.xml (58 行)

| 维度 | 评分 | 评语 |
|------|------|------|
| 整体 | 4 | `allowBackup="false"`（安全）、`foregroundServiceType="dataSync"`、可选的摄像头功能。缺少 `android:networkSecurityConfig`。 |

---

## Top 5 关键问题

### H1 — MainActivity.java: God Activity 反模式（全文件）
**严重程度：高 | 文件：MainActivity.java 第 85-3155 行**

MainActivity 承担 UI 绑定、生命周期管理、摄像头控制、引擎初始化、RTMP 推流、文件 I/O、JNI 桥接、媒体回放、系统监控、看门狗定时器、权限管理、统计显示等全部职责。这是 Android 层最严重的架构问题。3,155 行几乎不可能进行单元测试，任何修改都有不可预见的副作用。

**建议：** 将摄像头控制提取到独立的 ViewModel 或 Service；将文件导入逻辑提取到独立的 Repository/UseCase 类；使用 Activity 结果 API 替代已弃用的 `startActivityForResult`。目标将 MainActivity 缩减到 1,000 行以下。

### H2 — FfmpegRtmpPusher.java: 未使用的命令注入风险代码
**严重程度：中 | 文件：FfmpegRtmpPusher.java 第 72-76 行**

`cmd` StringBuilder（第 72-76 行）通过 shell 语法拼接参数，但是死代码——实际执行使用安全的 `String[]` 数组反射调用 FFmpegKit（第 80 行）。然而，这种死代码的存在是一个安全隐患：如果将来有人启用它而未意识到风险，`rtmpUrl`（用户输入）将作为 shell 参数传递，导致命令注入。另外 `escapeFFmpegArgument` 方法（第 104-108 行）同样未使用。

**建议：** 删除第 72-76 行的死代码。添加 `rtmpUrl` 输入验证（至少检查非空且以 `rtmp://` 或 `rtmps://` 开头）。

### H3 — MainActivity.java: onConfigurationChanged 重建全部视图
**严重程度：中 | 文件：MainActivity.java 第 1510-1590 行**

配置变更（旋转）时，`onConfigurationChanged` 销毁并重建所有视图，包括重新设置 `setContentView(R.layout.activity_main)`。这在约 80 行代码中重新注册所有回调、重新绑定手势检测器、重置布局参数。这种方法成本高且脆弱——任何新 View 都必须在此处手动处理。

**建议：** 使用 `android:configChanges="orientation|screenSize"` + Fragment 保留状态，或迁移到 Jetpack Compose 实现配置不变的 UI。至少，将重建逻辑提取到专用方法中。

### H4 — native-lib.cpp: JNI GetStringUTFChars 异常检查不一致
**严重程度：中 | 文件：native-lib.cpp 第 288-298 行**

`nativeInit` 函数在 `GetStringUTFChars` 后检查 `cascadePath` 的异常（第 289-291 行），但 `cameraId` 和 `storagePath` 的其他检查点不一致。在内存压力下如果 JNI 分配失败，这可能导致未定义行为。

**建议：** 对所有 `GetStringUTFChars` 调用统一执行异常检查，或使用 RAII 包装器。

### H5 — LogDetailActivity.java: 大文件筛选可能导致内存压力
**严重程度：低 | 文件：LogDetailActivity.java 第 541-591 行**

基于 StringBuilder 的筛选处理整个已加载内容。虽然有硬限制（`MAX_FILTER_OUTPUT_LINES=5000`、`MAX_FILTER_OUTPUT_CHARS=1,200,000`），但已加载内容在掩码后可能达到 8MB（`MAX_ALL_BYTES`），可能导致 GC 压力和 UI 卡顿。

**建议：** 考虑逐行流式筛选，避免将整个掩码内容保存在内存中。

---

## 评分汇总

| 维度 | 平均分 | 说明 |
|------|--------|------|
| 代码可读性 | 3.4/5 | 小文件和工具类表现不错，但 MainActivity 拖累严重。 |
| 架构模式 | 3.2/5 | MonitoringCoordinator 和 FeatureTemplateEncryptedStore 是亮点。MainActivity 的 God 模式是主要问题。 |
| 错误处理 | 3.2/5 | FeatureTemplateEncryptedStore 最优。FileLogSink 和 StatusService 静默错误过多。 |
| 安全防护 | 3.7/5 | **强项**。FeatureTemplateEncryptedStore（Android KeyStore + AES-256-GCM）和 SensitiveDataUtil（全功能脱敏）是项目亮点。AndroidManifest 禁止备份。剩余风险：命令注入死代码、RTMP URL 无验证。 |
| 性能 | 3.3/5 | 一般。帧处理和采样在后台线程。看门狗轮询可接受。onConfigurationChanged 重建是性能问题。 |
| 可测试性 | 2.6/5 | **弱项**。无依赖注入，大量静态方法和单例，强 Android 框架耦合。MonitoringCoordinator 和 SensitiveDataUtil 例外。 |

---

## 总体评分：中（差/中/良/优）

### 推荐的重构改进清单（按优先级）

1. **拆分 MainActivity.java** —— 提取 CameraViewModel、FileImportUseCase、SettingsRepository。目标 <1,000 行。
2. **删除 FfmpegRtmpPusher.java 中的死代码** —— 移除第 72-76 行的未使用字符串构建器。添加 RTMP URL 验证。
3. **修复 JNI 异常检查不一致** —— 为所有 `GetStringUTFChars` 调用添加 RAII 包装器或一致的异常检查。
4. **引入依赖注入** —— 迁移到 Hilt 或手动 DI，替换 `StatsRepository` 的单例和 MainActivity 的手动构造。
5. **迁移到 Activity Result API** —— 替换已弃用的 `startActivityForResult`（`REQUEST_CODE_PICK_MEDIA`、`REQUEST_CODE_TAKE_PHOTO`）。
6. **为 FileLogSink 添加错误报告** —— 日志写入失败时通知用户或至少记录到 logcat。
7. **添加网络安全配置** —— 为可能通过 HTTPS 进行的任何后端通信添加 `android:networkSecurityConfig`。
8. **统一布局方向策略** —— 考虑将横竖屏布局合并为一个自适应布局，减少维护负担。
9. **为 AppLog 添加测试** —— 验证脱敏启发式算法的正确性。创建 `SensitiveDataUtil` 的综合测试套件。
