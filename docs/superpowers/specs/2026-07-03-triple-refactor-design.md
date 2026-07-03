# 三层重构设计：FramePipeline · HttpFacesServer · Engine

**日期**: 2026-07-03
**状态**: 设计已批准
**关联文档**: 重构洞察-FramePipeline职责耦合问题.md, 重构洞察-Http接口路由耦合问题.md, 重构洞察-Engine帧处理管线耦合.md

---

## 1. 目标

将三个关键模块从"大而全执行类"改为"薄协调器 + 可替换阶段"，在不改变外部接口的前提下实现：

- **局部变化只影响局部对象**（相机切换不拖死 HTTP，模型重载不重启管线）
- **每个阶段独立可测**（非完整 Engine/FramePipeline 实例也能验证逻辑）
- **新增端点/策略只需注册或注入，不修改主链路**

---

## 2. 模块 1：FramePipeline → 协调器（方案 1A）

### 2.1 新增 4 个对象

```
FramePipeline (薄协调器, ~50行)
  ├── RuntimeBootstrap::build(cfg) → BootstrapResult
  ├── CameraSession::switchWithRollback(...) → CameraResult
  ├── FrameProcessor::run(frame, control) → FrameResult
  └── SideEffectSink::publish(result)
```

### 2.2 各对象职责

**RuntimeBootstrap** — 纯装配，无副作用
- 输入：`AppConfig`
- 输出：`BootstrapResult { recognizer, detector, models[], warning }`
- 不启动线程，不写 `render_.status`

**CameraSession** — 相机生命周期
- `switchWithRollback(device, prev)`: 打开 → 首帧超时 → 自动回滚
- 统一首帧超时和总耗时超标两条回滚分支

**FrameProcessor** — 纯计算
- `run(frame, controlPlane) → FrameResult`
- controlPlane 合并 `clearDbRequested_` / `enrollRequested_` / stride 策略
- 不写日志，不调 render_

**SideEffectSink** — 副作用收口
- overlay 绘制 + 结构化日志 + `render_.bgr` 发布 + `facesSeq` 通知

### 2.3 热更新策略（ReloadPolicy）

| 配置变化 | 当前动作 | 新动作 |
|----------|---------|--------|
| 相机分辨率/FPS | 整管线重启 | `CameraSession::switchWithRollback` |
| 模型路径 | shutdown→initialize | 仅重建 `RuntimeBootstrap` |
| 预览布局 | 重启后再设置 | 直接更新 `SideEffectSink` |

### 2.4 新增文件

| 文件 | 内容 |
|------|------|
| `src/win/include/rk_win/RuntimeBootstrap.h` | BootstrapResult 结构 + build() 声明 |
| `src/win/src/RuntimeBootstrap.cpp` | 识别器/DNN/模型快照装配 |
| `src/win/include/rk_win/CameraSession.h` | switchWithRollback 声明 |
| `src/win/src/CameraSession.cpp` | 相机打开/回滚/首帧超时 |
| `src/win/include/rk_win/FrameProcessor.h` | FrameResult 结构 + run() 声明 |
| `src/win/src/FrameProcessor.cpp` | 取帧→推理→返回结果（纯计算） |
| `src/win/include/rk_win/SideEffectSink.h` | publish() 声明 |
| `src/win/src/SideEffectSink.cpp` | overlay/日志/渲染态发布 |

### 2.5 修改文件

| 文件 | 改动 |
|------|------|
| `src/win/src/FramePipeline.cpp` | 从 747 行减至 ~300 行，委托给 4 对象 |
| `src/win/include/rk_win/FramePipeline.h` | 新增 4 个唯一指针成员 |
| `src/win/app/win_local_service_main.cpp` | 替换整管线重启为 ReloadPolicy 调用 |
| `src/win/CMakeLists.txt` | 新增 4 个 .cpp 文件 |

---

## 3. 模块 2：HttpFacesServer → 端点注册（方案 2A）

### 3.1 新增 3 个对象

```
HttpFacesServer (传输层, ~200行)
  ├── EndpointRegistry::dispatch(req, ctx) → Response | StreamSession
  │   └── JsonEndpointHandlers (models/settings/cameras/actions)
  └── StreamSessionRunner::run(sock, ctx)  // SSE + MJPEG 统一
```

### 3.2 各对象职责

**EndpointRegistry** — 路由表
- `EndpointDef { method, path, handler }` 数组
- `dispatch(req)`: 查表 → 执行 handler 或返回 405/404
- 统一 Method 校验（不再在 17 处重复）

**JsonEndpointHandlers** — REST 端点集合
- 按域分组：modelHandlers / settingsHandlers / cameraHandlers / actionHandlers
- 每个 handler 签名：`HttpResponse(const HttpRequest&, EndpointContext&)`
- 统一 JSON 响应工厂：`ResponseFactory::ok(data)` / `ResponseFactory::err(code, msg)`

**StreamSessionRunner** — 流式会话统一
- `run(sock, ctx, StreamType)` — SSE 和 MJPEG 共用
- 封装断连检测、空帧保护、keepalive
- 替代 `handleClient` 中硬编码的路径特判

### 3.3 新增文件

| 文件 | 内容 |
|------|------|
| `src/win/include/rk_win/EndpointRegistry.h` | EndpointDef, EndpointContext, dispatch() |
| `src/win/src/EndpointRegistry.cpp` | 路由表构建 + 查找 |
| `src/win/include/rk_win/JsonEndpointHandlers.h` | handler 声明 |
| `src/win/src/JsonEndpointHandlers.cpp` | models/settings/cameras/actions handler 实现 |
| `src/win/include/rk_win/StreamSessionRunner.h` | StreamType 枚举 + run() 声明 |
| `src/win/src/StreamSessionRunner.cpp` | SSE/MJPEG 统一执行循环 |

### 3.4 修改文件

| 文件 | 改动 |
|------|------|
| `src/win/src/HttpFacesServer.cpp` | 从 1005 行减至 ~450 行，handleApi 委托注册表，handleClient 委托流式执行器 |
| `src/win/include/rk_win/HttpFacesServer.h` | 新增 EndpointRegistry / StreamSessionRunner 成员 |
| `src/win/CMakeLists.txt` | 新增 3 个 .cpp 文件 |

---

## 4. 模块 3：Engine → 值流管线（方案 3C）

### 4.1 管线数据流

```
FramePacket                PreResult               InferResult
┌──────────┐              ┌──────────┐            ┌────────────┐
│cv::Mat   │  Preprocess  │cv::Mat   │  BioAuth   │vector<     │
│decodeMs  │ ───────────→ │processed │ ─────────→ │DetectedFace│
│timestamp │  Stage       │preMs     │  Stage     │inferMs     │
└──────────┘              └──────────┘            └────────────┘
                                                         │
                              ┌──────────────────────────┘
                              ▼
                         TrackResult              FrameOutcome
                         ┌──────────────┐        ┌──────────────┐
                         │vector<       │ Annot- │cv::Mat       │
                         │TrackView>    │ ator   │renderFrame   │
                         │cv::Mat       │───────→│tracks        │
                         │annotated     │        │events        │
                         └──────────────┘        │stats         │
                                                 └──────────────┘
                                                         │
                                            ┌────────────┴────────┐
                                            ▼                     ▼
                                     ResultPublisher        PerfReporter
                                     (回调/告警/渲染)        (P50/P95/CSV)
```

### 4.2 各阶段类型定义

```cpp
// 输入 — 统一多源
struct FramePacket {
    cv::Mat bgr;
    double decodeMs;
    long long timestampMs;
};

// 预处理 → 检测/识别
struct PreResult {
    cv::Mat processed;       // resize + flip 后
    double preMs;
};
struct DetectedFace {
    cv::Rect bbox;
    std::string identityId;
    float confidence;
    bool isAuthenticated;
};
struct InferResult {
    std::vector<DetectedFace> faces;
    double inferMs;
};

// 跟踪 → 标注 → 最终结果
struct TrackView {
    int trackId;
    cv::Rect bbox;
    std::string stableId;
    float stableConfidence;
};
struct TrackResult {
    cv::Mat annotated;       // 已画框+标签的帧
    std::vector<TrackView> tracks;
    bool hasBestAuth;
    TrackView* bestAuth;
};
struct DomainEvent {
    std::string type;        // VERIFIED / AUTH_FAIL / NO_FACE
    std::string message;
    long long timestampMs;
};

struct FrameOutcome {
    cv::Mat renderFrame;
    std::vector<TrackView> tracks;
    std::vector<DomainEvent> events;
    FramePerfStats stats;
};
```

### 4.3 五个新组件

**FrameSource** (~30 行)
- `next(timeoutMs) → std::optional<FramePacket>`
- 内部：externalInputEnabled? → ExternalInputChannel → VideoManager 回退
- 可替换实现（RTSP、回放文件、测试假源）

**TrackCoordinator** (~80 行，零 OpenCV 依赖)
- `update(faces, timestampMs) → std::vector<TrackView>`
- 纯函数：IoU 匹配 + stableId 计算 + TTL 清理
- **可加入 core_unit_tests**（合成坐标即可测）

**FrameAnnotator** (~40 行)
- `annotate(frame, tracks) → cv::Mat`
- 画框 + 标签 + MOTION_DETECTED 叠加

**ResultPublisher** (~60 行)
- `publish(outcome)` — 回调节流 + 渲染帧发布 + handleAbnormalEvent
- 封装 `lastXxxMs` 节流逻辑

**PerfReporter** (~50 行)
- `submit(stats)` — 无锁入队
- 内部：单一后台线程聚合 P50/P95 + 写 CSV

### 4.4 EngineRuntime 最终形态

```cpp
class EngineRuntime {
    std::unique_ptr<FrameSource> source_;
    std::unique_ptr<FacePipeline> pipeline_;  // Preprocess+BioAuth+TrackCoordinator+Annotator
    std::unique_ptr<ResultPublisher> publisher_;
    std::unique_ptr<PerfReporter> reporter_;

    void run() {
        while (isRunning) {
            auto packet = source_->next(30);
            if (!packet) continue;
            auto outcome = pipeline_->execute(*packet);
            publisher_->publish(outcome);
            reporter_->submit(outcome.stats);
        }
    }
};
```

### 4.5 新增文件

| 文件 | 内容 |
|------|------|
| `src/cpp/include/pipeline/FrameSource.h` | FramePacket, FrameSource 接口 |
| `src/cpp/src/pipeline/FrameSource.cpp` | 实现 |
| `src/cpp/include/pipeline/TrackCoordinator.h` | TrackView, TrackCoordinator |
| `src/cpp/src/pipeline/TrackCoordinator.cpp` | 纯函数实现 |
| `src/cpp/include/pipeline/FrameAnnotator.h` | 声明 |
| `src/cpp/src/pipeline/FrameAnnotator.cpp` | 实现 |
| `src/cpp/include/pipeline/ResultPublisher.h` | DomainEvent, ResultPublisher |
| `src/cpp/src/pipeline/ResultPublisher.cpp` | 回调节流 + 渲染 |
| `src/cpp/include/pipeline/PerfReporter.h` | 声明 |
| `src/cpp/src/pipeline/PerfReporter.cpp` | 异步统计 |

### 4.6 修改文件

| 文件 | 改动 |
|------|------|
| `src/cpp/src/Engine.cpp` | 1294→~150 行，委托给管线 |
| `src/cpp/include/Engine.h` | 213→~40 行，外部接口保留，内部替换 |
| `src/cpp/CMakeLists.txt` | 新增 pipeline 子目录 + 5 个 .cpp |

### 4.7 与 3B 的量化对比

| 指标 | 3B | 3C |
|------|:--:|:--:|
| `Engine.cpp` 降至 | ~500 | ~150 |
| `processFrame` 降至 | ~80 | 0（消亡）|
| `run()` 降至 | ~40 | ~15 |
| 可加入 `core_unit_tests` 的阶段 | 0 | 3 |
| 新增检测器需改 Engine | 是 | 否 |
| JNI 改动 | 0 | 0 |

---

## 5. 回归范围

### 5.1 模块 1：FramePipeline

- 服务启动 → 读取配置 → 初始化运行时 → 打开摄像头 → HTTP 预览可用
- 修改模型路径 → 触发 RuntimeBootstrap 重建 → 结果恢复稳定
- 注册流程 → enrollFromFrame → 样本数递减 → 落库
- 首帧超时 → CameraSession 自动回滚 → 清晰错误原因

### 5.2 模块 2：HttpFacesServer

- `GET/PUT /api/v1/settings` 返回格式和错误码一致
- `GET /api/v1/cameras` + `PUT /api/v1/camera/flip` + `GET /api/v1/preview.mjpeg` 联动
- `POST /api/v1/actions/enroll` + `POST /api/v1/actions/db/clear` + `GET /api/v1/models`
- Method 不匹配返回 405、非法 JSON 返回明确错误
- SSE/MJPEG 断连不泄漏线程、空帧正常、恢复继续

### 5.3 模块 3：Engine

- Android 相机连续采集 → Engine 取帧 → 人脸识别 → Java 收到 VERIFIED/AUTH_FAIL/NO_FACE
- 外部帧输入 → pushExternalFrame → 引擎消费 → 预览刷新
- Motion gate 开启时无运动只渲染不识别、恢复运动后恢复识别
- 多脸场景 trackId/stableId 连续稳定
- 单人进出、交叉遮挡、长时间无人时 track TTL 不回退
- flipX/flipY、节流开关切换与当前一致
- 性能 CSV 输出格式兼容

---

## 6. 实施顺序

```
Phase 1: FramePipeline 拆分 (影响面最小，验证重构方法论)
  ├── 1a: RuntimeBootstrap 抽出
  ├── 1b: CameraSession 抽出
  ├── 1c: FrameProcessor + SideEffectSink 抽出
  └── 1d: win_local_service_main.cpp 改用 ReloadPolicy

Phase 2: HttpFacesServer 拆分 (依赖 Phase 1 的 ReloadPolicy)
  ├── 2a: EndpointRegistry + ResponseFactory
  ├── 2b: JsonEndpointHandlers 逐域迁移
  └── 2c: StreamSessionRunner 统一流式端点

Phase 3: Engine 值流管线 (改动最大，最后做)
  ├── 3a: TrackCoordinator 纯函数化（可立即加测）
  ├── 3b: FrameSource 统一输入
  ├── 3c: ResultPublisher + PerfReporter 副作用收口
  └── 3d: EngineRuntime 装配 + JNI 验证
```

每个 phase 内部独立可测，phase 之间无硬依赖。
