# Android（RK3288）卡顿/掉帧与“逐帧推理”排查报告

更新时间：2026-05-17  
范围：Android 外部采集（Camera2/CameraX）→ JNI pushExternalFrame → Native Engine 推理与渲染链路  
结论重点：是否“每帧全量推理”、是否缺少 inference interval、资源指标来源、线程与阻塞点、参数化节流方案

---

## 1. 执行摘要（结论先行）

### 1.1 是否存在“每帧全量处理”

存在“尽可能每帧全量处理”的行为，但有“最新帧背压”作为被动降载：

- Java 侧采集回调每次拿到帧都会调用 `sink.onYuv420888Frame(...)`，进而调用 JNI 的 `nativePushFrameYuv420888(...)` 深拷贝并入队（或覆盖最新帧）。
- Native 侧 `Engine::run()` 主循环在拿到一帧后会执行 `processFrame()`，其中包含 `bioAuth->verifyMulti(...)`（人脸检测 + 识别），属于“全量推理/识别”路径。
- 外部帧通道 `FrameInputChannel` 默认配置为 `LatestOnly`（容量强制为 1），当推理跟不上输入帧率时，新帧会覆盖旧帧（老帧计入 dropped），因此不会无限堆积，但 Engine 仍会“能跑多快跑多快”。

结论：当前实现没有“按时间间隔/跳帧比例”的主动节流；只有“最新帧覆盖/有限队列丢帧”的被动背压。

### 1.2 是否缺少 inference interval 参数

缺少：

- `Engine`/`FrameInputChannel` 仅提供背压（LatestOnly / BoundedQueue + capacity），没有 `inferenceIntervalMs` 之类的“推理间隔/跳帧”配置。
- Java 侧也未提供任何“每 N 帧推理一次 / 每 X ms 推理一次”的可配置项。

结论：缺少可参数化的 inference interval（时间节流）能力；建议新增并与背压参数协同。

### 1.3 卡顿最可能成因（按优先级）

1) **推理耗时（bioAuth->verifyMulti）显著大于输入帧间隔**：Engine 处理不过来时，虽然 LatestOnly 会丢帧，但 UI/预览会表现为 FPS 降低、画面跳变、延迟上升。  
2) **JNI push 的深拷贝成本 + 潜在 Java 侧 fallback 拷贝/分配**：一旦 DirectByteBuffer 路径失败而走 byte[] 拷贝，会触发大量分配与 GC，显著放大卡顿风险。  
3) **渲染链路每 16ms 主动拉取 + nativeRenderFrameToSurface 的颜色转换/窗口锁**：即便推理帧率较低，渲染线程仍按 60fps 轮询，可能造成额外 CPU 压力与锁竞争。

---

## 2. “卡顿”定义与复现基线（Task 1）

### 2.1 统一术语与判定阈值（建议口径）

为避免“卡顿”含义混乱，建议将现象拆成 3 类，并为每类定义可观测阈值：

1) **预览停滞 / 掉帧（Preview Stutter）**  
   - 定义：用户看到的预览画面长时间不刷新或明显跳帧。  
   - 指标：`FPS（UI Choreographer）`、`LAT（capture→render 平均延迟）`、日志中的“渲染停滞”。  
   - 判定阈值（建议）：`FPS < 12` 持续 ≥ 2s 或 `LAT > 800ms` 持续 ≥ 2s，或出现“渲染停滞恢复”。

2) **主线程卡顿（UI Jank/ANR 风险）**  
   - 定义：UI 点击无响应、滑动掉帧、按钮触发延迟。  
   - 指标：`FPS（UI）`骤降、主线程有长任务（建议用 systrace/perfetto 验证）。  
   - 判定阈值（建议）：连续多帧 > 32ms；或“明显可感知点击延迟”。

3) **识别输出延迟（Recognition Latency）**  
   - 定义：人已在画面中，识别事件/文本输出延后出现。  
   - 指标：`LAT` 上升、`Engine` 的结果回调间隔变长（目前已做 650~2000ms 的结果发送节流）。  

### 2.2 固化复现路径（建议最小基线）

建议固定以下变量，避免一次排查引入过多干扰项：

- 输入源：USB 摄像头（Camera2）与（可选）CameraX 各一轮。
- 分辨率：优先 1280x720（若可稳定），不稳定则 640x480。
- 帧率：目标 30fps（Camera2/CameraX 均有 FPS 选择逻辑）。
- 模式：Continuous（`nativeSetMode(0)`）。
- Overlay：分别做「关闭」与「开启」两轮（Overlay 会引入额外采样与绘制开销）。

### 2.3 复现基线表（模板）

| 场景 | 采集方案 | 分辨率 | Overlay | 预期 CAP FPS | 实际 CAP FPS | 实际 UI FPS | LAT(ms) | 卡顿现象描述 | 关键日志 |
|---|---|---:|---|---:|---:|---:|---:|---|---|
| S1 | Camera2 | 1280x720 | 关 | 30 |  |  |  |  |  |
| S2 | Camera2 | 1280x720 | 开 | 30 |  |  |  |  |  |
| S3 | CameraX | 1280x720 | 关 | 30 |  |  |  |  |  |
| S4 | CameraX | 1280x720 | 开 | 30 |  |  |  |  |  |

建议“关键日志”至少包含：CameraX/Camera2 stats 行、MainActivity capture/preview watchdog 行、StatsRepository 采样行（如有）。

---

## 3. 资源占用指标来源（CPU/内存/可选 GPU）（Task 2）

### 3.1 Java 侧（UI 与 Overlay）

指标主要来自 `StatsRepository`：

- **UI FPS**：主线程 `Choreographer.FrameCallback` 的帧间隔统计（近似 UI 刷新率）。  
- **CAP FPS**：`reportCaptureFrameTimestampNs(tsNs)` 记录相邻采集帧间隔（来自 `CaptureObserver.onFramePushed`）。  
- **LAT**：`capture timestampNs → reportRenderTimeNs(System.nanoTime())` 的平均延迟（capture→render 端到端）。  
- **CPU%**：
  - 优先路径：读取 `/proc/stat` 与 `/proc/self/stat` 计算“本进程 CPU 时间 / 全系统 CPU 时间”的比例。
  - fallback：`android.os.Process.getElapsedCpuTime()` 与 `elapsedRealtime()` 计算进程 CPU%（不严格按核数归一）。
- **内存**：`Debug.getMemoryInfo`（PSS、Private Dirty），以及（可选）`summary.graphics`（API≥23 的图形内存统计字段）。

结论：仓库内已经有“端到端”与“进程级”采样，但不含 GPU 使用率（%）指标；仅能给出 graphics 内存（可选）。

### 3.2 Native 侧（可用于离线 bench）

`Engine::processFrame()` 在 Linux/Android 下会调用 `getrusage(RUSAGE_SELF)` 记录 `ru_maxrss`，并把每帧统计写入 `perfHistory`；当累计样本 ≥ 30 且到达 1s tick 时，输出汇总并可追加写入 `engine_perf.csv`（路径由环境变量 `RK_BENCH_OUT_DIR` 决定，默认 `tests/metrics`）。

注意：这套 CSV 输出更偏向“本地/bench”，在 Android App 运行时需要确认输出目录可写（以及是否希望写文件）。

---

## 4. 执行时序核查：每帧入口、JNI 触发点、背压（Task 3）

### 4.1 Android 端每帧入口

- Camera2：`onImageAvailable()` 使用 `acquireLatestImage()`，每次回调都尝试推入一帧；并按 1s 打印采集侧 stats（含 approxDrop）。  
- CameraX：`analysis.setAnalyzer(singleThreadExecutor, this::analyze)`，backpressure 为 `STRATEGY_KEEP_ONLY_LATEST`；每次 analyze 都尝试推入一帧。

结论：采集侧是“逐帧回调 → 逐帧 push”，不会主动跳帧。

### 4.2 JNI/native 推理触发点与条件

- Java 侧 `pushExternalYuv420888(...)`：
  - 优先走 DirectByteBuffer → JNI `nativePushFrameYuv420888(...)`；
  - 失败则 fallback：复制成 `byte[]` 再调 `nativePushFrameYuv420888Bytes(...)`（更重、更易 GC）。
- JNI `nativePushFrameYuv420888(...)`：
  - 深拷贝 Y/U/V 平面到 `ExternalFrame`（Native 持有，避免 Java 复用造成悬挂指针）；
  - 调用 `Engine::pushExternalFrame(std::move(f))`。
- `Engine::run()`：
  - 若启用 externalInput，则 `externalInput->waitPop(..., 30ms)` 拿一帧后做格式转换 `toBgrFromExternalFrame`；
  - 立即调用 `processFrame()`（resize/flip + `verifyMulti` + renderFrame 更新）。

结论：Native 推理线程对“每个被消费的帧”都会执行 `verifyMulti`，属于全量处理；没有“时间节流/跳帧”条件。

### 4.3 背压/最新帧策略（已有但未参数化到 UI）

外部帧通道 `FrameInputChannel` 支持：

- `LatestOnly`：仅保留最新帧（push 覆盖旧帧，旧帧计入 dropped）。
- `BoundedQueue`：有限队列，超过容量丢最旧帧。

目前 Android 启动时调用 `nativeConfigureExternalInput(true, 0, 1)`，即 LatestOnly + capacity=1。

结论：背压机制已存在，但属于“队列层面”；不等价于“推理间隔（inference interval）”。

---

## 5. 线程模型与潜在阻塞点（Task 4）

### 5.1 线程模型（从 Android App 视角）

- UI 主线程：按钮/布局/RecyclerView 更新、Choreographer FPS 采样、每帧调度下一次 FrameWorker tick 的“postDelayed(16ms)”发生在 UI 线程。
- FrameWorker（HandlerThread: `FrameWorker`）：循环调用 `nativeRenderFrameToSurface()` 或 `nativeGetFrame()` 获取最新渲染帧。
- Camera2Capture（HandlerThread: `Camera2Capture`）：相机回调 `onImageAvailable()`；其中会调用 `sink.onYuv420888Frame` → JNI 推帧。
- CameraXAnalyzer（single thread executor）：`analyze()` 回调；其中会调用 `sink.onYuv420888Frame` → JNI 推帧。
- StatsSampler（HandlerThread: `StatsSampler`）：每 500ms 采样 CPU/MEM/FPS/LAT。
- EngineThread（std::thread）：`Engine::run()` 主循环（推理/渲染帧更新）。

### 5.2 至少 3 类潜在阻塞点（含证据定位建议）

1) **相机回调线程阻塞（Camera2Capture / CameraXAnalyzer）**  
   - 阻塞来源：`pushExternalYuv420888` 中的深拷贝/校验；Direct 路径失败时 fallback 走 `byte[]` 分配与复制，会明显拖慢回调线程。  
   - 现象：CAP FPS 下降、`pushFail` 增加、Camera2/CameraX stats 中 approxDrop 上升。  
   - 建议证据点：在 Java 的 `pushExternalYuv420888()` 前后打点（成功走 direct 还是 bytes fallback），统计耗时与 fallback 次数。

2) **Engine 推理线程饱和（EngineThread）**  
   - 阻塞来源：`processFrame()` 里 `bioAuth->verifyMulti()`（检测/识别）耗时大；以及外部帧转换 `toBgrFromExternalFrame`（可能先把 720p/1080p 转 BGR，再 resize 到 640x480）。  
   - 现象：UI FPS 降、LAT 升、识别事件频率降低；externalInput dropped 增加（LatestOnly 覆盖频繁）。  
   - 建议证据点：在 native 侧打印/导出 `inferMs/preMs/totalMs`，以及 FrameInputChannel stats（pushed/popped/dropped）。

3) **渲染线程/锁竞争（FrameWorker + renderMutex）**  
   - 阻塞来源：`nativeRenderFrameToSurface` 频繁调用 `Engine::getRenderFrame`，内部 `renderMutex` 锁；同时 engine 也会持锁更新 renderFrame。虽然单次持锁短，但在低端设备上仍可能放大抖动。  
   - 现象：预览刷新不均匀、frameUpdater ok 但 UI FPS 不稳定。  
   - 建议证据点：在 `getRenderFrame` 与 `processFrame` 的 renderMutex 临界区外打点统计等待时间（仅建议在短期诊断分支启用）。

（可选第 4 类）**UI 主线程阻塞（RecyclerView/动画/大量日志 UI 刷新）**  
   - 来源：识别事件列表刷新、频繁 runOnUiThread、layout/measure 开销。  
   - 现象：UI FPS 下降但 CAP FPS 可能正常。  
   - 证据：perfetto/systrace 主线程长任务；或临时关闭识别列表刷新对比。

---

## 6. 参数化节流/间隔方案（Task 5：设计，不直接实现）

目标：在不影响“最新画面可见”的前提下，把推理负载从“尽可能满速”改成“可控速率”，并让队列背压成为第二道保障。

### 6.1 最小可行参数（建议命名）

1) `inferenceIntervalMs`（时间间隔节流，核心）  
   - 含义：两次“进入重推理（detect/identify）”之间至少间隔多少毫秒。  
   - 实现思路：在 `Engine::run()` 或 `processFrame()` 前判断 `nowMs - lastInferMs < inferenceIntervalMs` 时：
     - 仍消费/丢弃输入帧以保持“最新帧”，
     - 但跳过 `bioAuth->verifyMulti()`，只做 resize/flip/渲染更新（或仅更新 renderFrame），从而维持预览流畅度。

2) `maxInFlightFrames`（背压容量）  
   - 与已有 `queueCapacity` 对齐：LatestOnly 下强制为 1；BoundedQueue 下建议 2~3。  

3) `latestOnly`（背压模式开关）  
   - 对应已有 `FrameBackpressureMode::LatestOnly` / `BoundedQueue`。

### 6.2 默认值推导（示例）

推导方式：以“单帧重推理耗时 msTotal（或 inferMs）”与“目标推理 FPS”做反推。

- 若目标推理帧率为 8fps（每 125ms 一次），则 `inferenceIntervalMs ≈ 125`。  
- 若测得 `verifyMulti` 平均耗时约 120ms，则：
  - 想要避免堆积/饱和，`inferenceIntervalMs` 不应小于 120ms；
  - 若还要留给渲染/采样/系统的余量，可取 `150~200ms`。

建议默认值（保守）：`inferenceIntervalMs = 150`，`latestOnly=true`，`maxInFlightFrames=1`。

### 6.3 动态自适应策略（建议）

在固定默认值之外，可引入自适应（无需很复杂，优先稳定）：

- 输入：`cpuPercent`、`latencyMs`、`UI FPS`（已有 StatsRepository 可提供）。  
- 策略：
  - 若 `LAT > 800ms` 或 `UI FPS < 12` 持续 2s，则逐步增大 interval（例如 +50ms，最高到 500ms）。
  - 若 `LAT < 250ms` 且 `UI FPS > 25` 持续 5s，则逐步减小 interval（例如 -25ms，最低到 80ms）。
  - 外部输入仍保持 LatestOnly，确保画面“新鲜”。

### 6.4 验证步骤（回归口径）

1) 在相同输入源与分辨率下，对比开启/关闭节流前后的：CAP FPS、UI FPS、LAT、CPU%、MEM。  
2) 观察日志中 approxDrop、pushFail、previewRecovery 是否显著下降。  
3) 确认识别输出仍可用（但频率会按 interval 降低，应在产品上可接受）。  

---

## 7. 附：代码定位索引（用于复查/打点）

Android：

- Camera2 每帧入口：`src/java/.../Camera2CaptureController.java` → `onImageAvailable()`  
- CameraX 每帧入口：`src/java/.../CameraXCaptureController.java` → `analyze()`  
- 推帧到 JNI：`src/java/.../MainActivity.java` → `pushExternalYuv420888()`  
- 外部输入配置：`src/java/.../MainActivity.java` → `nativeConfigureExternalInput(true, 0, 1)`  
- 端到端指标：`src/java/.../StatsRepository.java`（FPS/CAP/LAT/CPU/MEM）

Native：

- JNI 深拷贝并入队：`src/cpp/native-lib.cpp` → `nativePushFrameYuv420888(...)`  
- 背压队列：`src/cpp/src/FrameInputChannel.cpp`  
- 推理主循环：`src/cpp/src/Engine.cpp` → `Engine::run()` / `processFrame()`  
- 单次推理分段耗时（现有能力，可复用）：`src/cpp/src/FaceInferencePipeline.cpp`（msDetect/msAlign/msEmbed/msSearch/msTotal）

