# 日志分析报告：evidence_20170115_035627

## 文档信息

| 项目 | 内容 |
|------|------|
| 分析日期 | 2026-05-18 |
| 证据目录 | `ErrorLog/evidence_20170115_035627/` |
| 分析依据文件 | manifest.txt, device_info.txt, last_preflight.txt, logcat_snapshot.txt, logcat_20170115_035423.log, rk3288_20170115_035040.log |

---

## 一、设备与运行环境

| 参数 | 值 |
|------|-----|
| 导出时间 | 2017-01-15 03:56:38 GMT+8 |
| 设备型号 | yx_rk3288 |
| Android 版本 | 7.1.2 (SDK 25) |
| 应用包名 | com.example.rk3288_opencv |
| 进程 PID | 16315 |
| 屏幕分辨率 | 1920x1080 |
| GPU | Mali-T760 (r18p0-01rel0, GLES 3.1) |
| OpenCL | 不可用（effective=0） |
| 总内存 | ~2.0 GB（2094325760B） |
| 可用存储 | ~4.5 GB（4879515648B） |
| 构建类型 | userdebug |
| 相机参数 | Cam 0, 1280x720 @ 25fps, Legacy Camera HAL |
| 关键宏定义 | RK_HAVE_QUALCOMM=0, RK_HAVE_MPP=0 |

---

## 二、时间线全景

``` bash
03:50:40.612 ─┬─ 应用 onCreate → 日志开始
               ├─ OpenCV 4.10.0 初始化（CPU features: 7ff）
               ├─ OpenGL ES 3.1 初始化 (EGL 1.4, Mali-T760)
               ├─ StatusService 创建
               ├─ FPS/CAP/LAT/CPU/MEM 全部 "未采样"
               │
03:50:46.574 ─┬─ ProfileInstaller：安装启动性能配置
               │
03:50:46.954 ─┬─ MainActivity.onCreate() →
               ├─ 窗口 1920x1080
               ├─ discoverCameras()
               ├─ 权限检查
               │
03:50:47.737 ─┬─ initEngine() 第一次调用
               ├─ Qualcomm SDK → CPU fallback (无硬件加速)
               ├─ MPP decoder → CPU fallback (无硬件解码)
               ├─ OpenCL → 不可用
               ├─ Engine.initialize() 第一次
               ├─ Engine.initialize() 第二次（重复调用）
               │
03:50:50.955 ─┬─ 相机预检完成 (1280x720 @ 25fps)
               ├─ 外部帧输入通道启用（背压 mode=0 cap=1）
               │
03:50:51.200 ─┬─ Engine.run() 启动
               │
03:50:52.708 ─┬─ 相机 CAPTURING 状态
03:50:52.737 ─┬─ [capture] 首帧推入 ok ← 采集链路正常！
03:50:52.815 ─┬─ SYSTEM READY
               │
03:50:54 ─────┬─ handleAbnormalEvent 开始频繁触发
               │
03:51:13.217 ─┬─ 帧分析延迟尖峰（max=27.6ms）
               │
03:51:29 ─────┬─ Session 1 停止（外部帧禁用 → Engine.stop）
               │  运行约 38s（但 UI 仅 20s）
               │
03:51:34.124 ─┬─ onDestroy → 缓存清理 1.9MB
               │
03:51:40.760 ─┬─ Activity 重建（onCreate）
               │
03:52:10.006 ─┬─ [热重启] reason=手动热重启
               │   captureEverPushed=false ← 新会话初始状态，正常
               ├─ Session 2 开始
               │
03:52:11.839 ─┬─ [capture] 首帧推入 ok（Session 2）
               │
03:53:14.082 ─┬─ 帧分析延迟尖峰（max=37.6ms）
               │
03:53:23 ─────┬─ Session 2 停止（运行约 73s）
               │
03:53:25 ─────┬─ LogViewerActivity → 日志浏览阶段
               │
03:56:38 ─────┬─ 日志导出
```

---

## 三、按模块分析

### 3.1 Engine 初始化与硬件加速

**初始化序列（原生层各组件）：**

| 组件 | 结果 | 说明 |
|------|------|------|
| Qualcomm SDK | ❌ CPU fallback | RK_HAVE_QUALCOMM 未定义，HMSS 不可用 |
| MPP Decoder | ❌ CPU fallback | RK_HAVE_MPP 未定义，硬件视频解码不可用 |
| OpenCL | ❌ 不可用 | 设备不支持 |
| VideoManager | ✅ 外部帧模式 | cameraId=-1，跳过本地相机打开 |
| OpenCV Parallel | ✅ ONETBB/TBB/OPENMP | 三者按优先级注册 |

**发现：**
- **所有硬件加速路径均不可用**：Qualcomm SDK、MPP 硬件解码、OpenCL 全部回退到 CPU。这意味着所有图像处理（帧解码、预处理、推理）完全依赖 CPU 计算，这是 RK3288 平台的根本性性能瓶颈。
- Engine.initialize() 在 `initEngine()` 中被调用了**两次**（03:50:47.742 和 03:50:47.762），均为 cameraId=-1（外部帧输入模式）。

**问题：**
1. **无硬件加速**：CPU-only 模式下，帧分析和推理性能完全受限于 CPU 频率和核数，这是延迟尖峰的根本原因。
2. **冗余初始化**：两次 initialize 调用浪费约 20ms 启动时间。

### 3.2 相机系统（Camera2Capture）

**发现：**
- 使用 **Legacy Camera HAL**，Android 兼容模式。
- 相机输出 1280x720 @ 25fps。
- 设备配置了 **ProfileInstaller**（03:50:46.574），说明启用了 Android 启动性能配置优化。
- Compatibility 警告（AWB、AF、焦距）不影响基础功能。

**帧处理统计：**

| 指标 | Session 1 | Session 2 |
|------|-----------|-----------|
| 持续时间 | ~20s (UI) / ~38s (Engine) | ~73s |
| 帧率范围 | 20-25 fps | 20-26 fps |
| pushOk | 全部成功 | 全部成功 |
| approxDrop 范围 | 3-23 帧/周期 | 0-17 帧/周期 |
| analyzeAvg | 2.3-4.9 ms | 2.5-5.9 ms |
| analyzeMin（首帧） | 13.9 ms | 6.7 ms |
| analyzeMax（异常）| 27.6 ms | **37.6 ms** |

**延迟尖峰分析：**

| 时间点 | 值 | 可能触发因素 |
|--------|-----|-------------|
| 03:50:52.741 | 13.9ms | 首帧，OpenCV 并行后端初始化 |
| 03:51:13.217 | 27.6ms | 3 次 handleAbnormalEvent 集中触发后 |
| 03:52:11.841 | 6.7ms | Session 2 首帧（缓存热了） |
| 03:52:36.344 | **20.5ms** | CPU 竞争 |
| **03:53:14.082** | **37.6ms** | 接近 25fps 帧间隔上限 |

**评估：**
- 平均分析延迟（2.3-4.9ms）对于 CPU-only 的 RK3288 平台是可接受的。
- 但最大延迟 37.6ms（03:53:14）在 25fps（40ms 间隔）下几乎占满整个帧间隔，极易导致帧丢失。
- `approxDrop` 波动较大（0-23），说明引擎处理负载不稳定。
- 3 次 **blocking GC**（JitCodeCache）暂停应用，是延迟尖峰的可能贡献者。

### 3.3 帧采集链路（capture 事件）

**这是本次分析中发现的最重要修正：**

之前的分析基于 `hotRestart` 参数 `captureEverPushed=false`，认为帧从未被消费为有效 capture。但从 `rk3288_20170115_035040.log` 中可以看到明确证据：

``` bash
03:50:52.737 I/MainActivity(16594): [capture] 首帧推入 ok   ← Session 1 首帧
03:52:11.839 I/MainActivity(18448): [capture] 首帧推入 ok   ← Session 2 首帧
```

**结论：捕获链路在两个 Session 中都正常工作。**

`captureEverPushed=false` 出现在 `hotRestart` 参数中，这是因为 `hotRestart` 发生在 Activity 重建之后（`onCreate` → `onDestroy` →
`onCreate`），引擎状态已重置为全新的 session。`captureEverPushed` 跟踪的是**当前 session** 的捕获状态，在新的 session 中自然为 `false`。

**修正：P1（原"帧从未被消费为有效 capture"）→ 已排除。捕获链路正常。**

### 3.4 StatusService（状态监控系统）

**采集指标列表：**

| 指标 | 含义 | 启动阶段状态 |
|------|------|-------------|
| FPS | 帧率统计 | "未采样" → "FPS不足样本" |
| CAP | 采集性能 | "未采样" → "CAP不足样本" |
| LAT | 延迟统计 | "未采样" → "LAT不足样本" |
| CPU | CPU 使用率 | "未采样" → "CPU首次采样" |
| MEM | 内存使用 | "未采样"（之后未再报错） |

**发现：**
- StatusService 在应用启动后立即开始周期性采集（约每 500ms 一次）。
- 启动后约 12 秒（03:50:40 → 03:50:52），所有指标逐步就绪。
- MEM 仅在开始时报一次"未采样"，之后不再报错——说明内存采集最稳定。
- CPU 在 03:50:41.466 时从"未采样"变为"CPU首次采样"，之后也未再报错。

### 3.5 GPU / Mali 渲染

**Mali GPU 详细数据：**

| 参数 | 值 |
|------|-----|
| GPU 型号 | Mali-T760 |
| 驱动版本 | r18p0-01rel0 |
| EGL 版本 | 1.4 |
| OpenGL ES | 3.1（由 Mali-T760 支持） |

**行为：**
- `mali_winsys.new_window_surface` 超过 100 次，**全部返回 0x3000**（EGL_SUCCESS）。
- 无渲染错误，无 GPU 崩溃。
- 窗口表面创建频繁，与 Activity 重建、LogViewer 多次加载、以及渲染重绘相关。
- 应用进程不是 SurfaceFlinger 进程（`current process is NOT sf, to bail out`）。

### 3.6 ART / JIT 运行时

**代码缓存演进：**

| 时间 | 类型 | 前后大小 | 新容量 |
|------|------|---------|--------|
| 03:50:51.006 | partial | 18KB→18KB | 128KB |
| 03:50:54.938 | partial | 38KB→38KB | 256KB |
| 03:51:00.563 | **full** | 90KB→83KB | — |
| 03:51:09.241 | partial | 93KB→93KB | 512KB |
| 03:53:40.780 | **full** | 195KB→194KB | 1024KB |
| 03:54:30.441 | partial | 209KB→209KB | — |

**关键发现：**
- **Debugger 在 03:50:55.624 断开连接**，并立即触发了一个 **blocking GC Instrumentation**。这是 ART 在 debugger 断开时执行的垃圾回收，会暂停应用。
- 03:51:00.563 的 **full code cache collection** 同样会暂停应用（stop-the-world）。
- 03:53:40.780 的另一个 **full collection** 发生在日志浏览阶段，对帧处理无影响。

### 3.7 LogViewerActivity（日志查看器）

**发现的 loadLogs 调用记录：**
- 03:53:25.649（入口）+ 03:53:25.660（立即第二次）
- 03:54:17.346
- 03:54:23.587
- 03:55:58.948
- 03:56:38.650

logcat_20170115_035423.log 在 03:54:23 导出（与第三次 loadLogs 时间吻合），而 logcat_snapshot.txt 在 03:56:38 导出（与最后一次 loadLogs
时间吻合）。**所以两个 logcat 文件是对同一进程日志在不同时间点的导出，核心内容一致。**

---

## 四、已排查文件清册

| 文件名 | 大小 | 分析状态 | 说明 |
|--------|------|---------|------|
| manifest.txt | 手动 | ✅ 已分析 | 文件清单声明，2个 .log 文件确认存在 |
| device_info.txt | 手动 | ✅ 已分析 | 设备和系统环境信息 |
| last_preflight.txt | 手动 | ✅ 已分析 | 相机预检数据 |
| logcat_snapshot.txt | 手动 | ✅ 已分析 | logcat 进程快照，477行（03:56:38 导出） |
| logcat_20170115_035423.log | 67KB | ✅ 已分析 | logcat 进程快照，463行（03:54:23 导出，略早） |
| rk3288_20170115_035040.log | 36KB | ✅ 已分析 | **原生层会话日志，包含关键 capture 事件** |

**文件关系说明：**
- `logcat_20170115_035423.log` 和 `logcat_snapshot.txt` 是对同一进程 logcat 在不同时间点的两次导出，核心内容一致，后者稍晚（包含更多 mali_winsys 日志）。
- `rk3288_20170115_035040.log` 是应用原生层（C++）的会话日志，由 `NativeBridge.nativeConfigureLog` 配置输出，包含 JNI/Engine 层的详细条目。

---

## 五、综合分析

### 5.1 核心问题汇总

| 编号 | 问题 | 严重度 | 影响范围 | 状态变化 |
|------|------|--------|---------|---------|
| P1 | **所有硬件加速路径均不可用（CPU-only）** | 🔴 严重 | 全局性能瓶颈 | **新增** |
| ~~原 P1~~ | ~~帧未被消费为有效 capture~~ | — | — | **已排除**（捕获链路正常） |
| P2 | **handleAbnormalEvent 过度频繁触发（50+ 次）** | 🟡 高 | CPU 干扰 | 维持，可信度提高 |
| P3 | 偶发帧分析延迟尖峰（max 37.6ms） | 🟡 中 | 帧率稳定性 | 维持，根因更清晰（CPU-only + GC） |
| P4 | Engine 初始化冗余调用（2 次） | 🟢 低 | 启动耗时 | 维持 |
| P5 | ARC（CPU-only）在 25fps 下余量不足 | 🟡 中 | 高负载场景稳定性 | **新增/合并** |
| P6 | Legacy Camera HAL 功能受限 | 🟢 低 | 不影响基础功能 | 维持 |

### 5.2 P1 详细诊断：CPU-only 性能瓶颈（核心根因）

**证据链：**
1. Qualcomm SDK → CPU fallback（RK_HAVE_QUALCOMM=0）
2. MPP decoder → CPU fallback（RK_HAVE_MPP=0）
3. OpenCL → 不可用（haveOpenCL=0）
4. OpenCV 并行后端虽启用 3 个（ONETBB, TBB, OPENMP），但在 RK3288 上实际性能有限

**影响分析：**
- 所有 YoloFaceDetector 的推理、ArcFaceEmbedder 的特征提取、图像预处理完全在 CPU 上运行。
- RK3288 的 CPU 是 ARM Cortex-A17 四核 @ 1.8GHz，面对 1280x720 @ 25fps 的人脸检测+识别任务，计算余量非常有限。
- 当 JIT code cache collection 或 blocking GC 发生时（如 03:51:00），CPU 被抢占，帧处理延迟瞬间飙升。

**结论：CPU-only 是本次日志中观察到的大部分性能问题的根本原因。**

### 5.3 P2 详细分析：handleAbnormalEvent 高频触发

**症状：** 03:50:54 ~ 03:53:23，两个会话合计触发 **约 55 次**。

**与 CPU-only 的关联：**
在 CPU-only 条件下，帧处理 pipeline 的**每个阶段**都可能因 CPU 资源不足而出现处理超时或失败，触发异常事件。具体可能原因：
1. YoloFaceDetector 推理超时 → 触发异常
2. 帧预处理阶段内存分配延迟 → 触发异常
3. 后端人脸识别线程调度延迟 → 触发异常

**影响：** 每次异常处理都会产生额外的 CPU 消耗，在 CPU-only 的系统中形成"处理慢 → 触发异常 → 异常处理消耗CPU → 处理更慢"的恶性循环。

### 5.4 P3 详细分析：延迟尖峰

**Session 1 延迟分布：**
- 典型：2.3-4.9ms
- 最大：27.6ms（03:51:13）
- 3次 handleAbnormalEvent 在 03:51:12.4-13.0 密集触发，随后出现 27.6ms 尖峰

**Session 2 延迟分布：**
- 典型：2.5-5.9ms
- 最大：37.6ms（03:53:14）
- 37.6ms 在 25fps（40ms 间隔）下仅剩 ~2.4ms 余量

**延迟尖峰时间与系统事件的关联：**

| 时间 | 延迟 | 同时发生的系统事件 |
|------|------|-------------------|
| 03:50:55.814 | 13.4ms | Debugger 断开 + blocking GC Instrumentation |
| 03:51:00.932 | 9.4ms | full code cache collection (90KB→83KB) |
| 03:51:13.217 | **27.6ms** | 密集 handleAbnormalEvent ×3 |
| 03:53:14.082 | **37.6ms** | 稳定运行 1 分钟后，CPU 负载累积 |
| 03:52:36.344 | 20.5ms | handleAbnormalEvent 触发中 |

### 5.5 说明：为什么两次监控会话都停止了

两个 Session 都是被**手动停止**的（非崩溃或异常退出）：
- Session 1: 从监控启动到停止仅约 20s UI 运行时间，可能因为测试人员主动停止系统查看状态
- Session 2: 更长（~73s），但最终同样被手动停止
- 随后用户进入了 LogViewerActivity 浏览日志，最终导出日志文件

这表明这是一次**测试/调试会话**，而非生产环境中的崩溃恢复场景。

---

## 六、改进建议

| 优先级 | 改进项 | 预期效果 | 工作量估计 |
|--------|--------|----------|-----------|
| **P0** | **启用 MPP 硬件解码**（定义 RK_HAVE_MPP，配置 MPP 库） | 帧解码阶段从 CPU 卸载到硬件，显著降低预处理延迟 | 中 |
| **P0** | **调查 handleAbnormalEvent 触发条件**，区分真正的异常与可忽略警告 | 打断"处理慢→异常→更慢"的恶性循环 | 中 |
| **P1** | 在帧分析 Pipeline 中实现阶段计时（decode/pre/infer/post），精确定位 37.6ms 尖峰的阶段 | 明确优化方向 | 低 |
| **P1** | 消除 Engine 初始化冗余调用（`initEngine()` 只调用一次） | 减少启动耗时 ~20ms | 低 |
| **P1** | 评估 RK3288 上 CPU-only 运行 Yolo+ArcFace 的可行性，考虑模型量化或轻量化 | 降低推理延迟，提升 FPS 稳定性 | 高 |
| **P2** | 优化相机预热流程，缩短首帧到达时间 | 改善启动体验 | 低 |
| **P2** | 完善 ErrorLog 归档规范，确保原始日志文件被完整提交 | 避免未来分析受限 | 低 |

---

## 七、进一步诊断建议

1. **查看 C++ 源码**：检查 `Engine::handleAbnormalEvent()` 的实现，明确异常判定逻辑。
2. **检查 MPP 和 Qualcomm SDK 集成**：确认 RK_HAVE_MPP 和 RK_HAVE_QUALCOMM 宏在 build 系统中是否可启用，以及对应的硬件库是否可用。
3. **性能 profile**：对帧处理管道进行阶段计时（decode、pre-process、infer、post-process），精确定位 37.6ms 尖峰的具体阶段。
4. **重现测试**：在开启 MPP 硬件解码（如果可以）和纯 CPU 模式下对比运行，验证硬件加速的实际效果。
5. **降低推理分辨率**：如果 640x480 推理分辨率仍然太高，可尝试 320x240 量化模型。

---

## 八、文件间差异说明

- `logcat_snapshot.txt` 和 `logcat_20170115_035423.log` 内容几乎一致，分别于 03:56:38 和 03:54:23 导出，后者略早。
- `rk3288_20170115_035040.log`（36KB）包含原生层日志，与 logcat 中的 Java 层日志互补，共同构成完整的诊断信息。

---

*分析结束。本文档基于 ErrorLog/evidence_20170115_035627/ 目录下的全部 6 个文件生成。*

[← 返回目录](#5-深度研究与专项文档-research-deep-dive)
