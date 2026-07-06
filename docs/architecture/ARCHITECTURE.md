# RK3288 OpenCV 架构契约

## 背景

本文档定义所有 C++ 子系统（Core Engine / Windows Service / Android JNI）必须遵守的 5 条架构规则。每条规则附带已有正例（positive example）、违规清单和自动化检查方法。

---

## 契约 1: RAII 资源封装

### 规则

所有 `lock/unlock`、`open/close`、`allocate/free`、`begin/end` 配对必须封装为 RAII 类型后方可使用。裸配对在 code review 中直接拒绝。

### 已有正例

- `SocketGuard`（Windows `HttpFacesServer.cpp`）— 析构自动 `closesocket`
- `WindowReleaser`（`native-lib.cpp`）— 析构自动 `ANativeWindow_release`

### 反例（待修复）

```
// native-lib.cpp — 裸 lock/unlock，中间路径异常则泄漏
ANativeWindow_lock(win, &buf, nullptr);
// ... 操作 ...
ANativeWindow_unlockAndPost(win);
```

### 违规清单

| 文件 | 行号 | 裸调用 | 优先级 |
|------|------|--------|--------|
| `src/cpp/native-lib.cpp` | ~751 | `ANativeWindow_lock` / `ANativeWindow_unlockAndPost` | P0 |
| `src/cpp/native-lib.cpp` | ~653 | `AndroidBitmap_lockPixels` / `AndroidBitmap_unlockPixels` | P0 |

### 自动检查

```bash
scripts/check-raii-violations.sh
```

对所有 `.cpp`/`.h` 文件 grep `_lock(` 和 `_unlock` 裸调用（不在 RAII 类定义中）。

---

## 契约 2: 纯函数管线

### 规则

核心业务逻辑必须实现为纯函数：输入 `const&`，输出通过返回值传递，不修改全局/成员状态。Side effects（I/O、日志、UI 绘制）在调用方边界集中处理。

### 已有正例

```cpp
// TrackCoordinator::update() — const 输入，值返回，无全局副作用
TrackResult TrackCoordinator::update(
    const std::vector<DetectedFace>& detections,
    const std::vector<TrackView>& existing,
    const TrackConfig& cfg);
```

### 待改造

| 位置 | 当前模式 | 目标模式 |
|------|----------|----------|
| `Engine::processFrame()` 中的 `putText`/`rectangle` | 直接修改 frame 像素 | 提取为 `compositeFrame()` 纯函数，返回绘制命令列表 |

---

## 契约 3: 原子状态提交（Transactional Copy）

### 规则

任何涉及多个字段的共享状态更新必须使用局部副本 → 修改 → 一次性 `atomic_store` / `std::move` 模式。禁止在持有锁期间执行 I/O 或回调。

### 模式模板

```cpp
auto snapshot = currentState.load();  // 读原子指针
auto updated = *snapshot;             // 复制到局部
updated.counter += 1;                 // 在副本上修改
currentState.store(
    std::make_shared<State>(std::move(updated)));  // 原子替换
```

### 适用场景

- `g_previewWindow` / `g_previewGeneration` 在 `native-lib.cpp` 中的使用路径
- `Engine::currentTracks_` 当前通过互斥锁保护 → 可演进为 `std::atomic<std::shared_ptr<const std::vector<TrackView>>>`

---

## 契约 4: 错误通道

### 规则

帧处理路径禁止 `throw`。使用 `std::optional<T>` 或 `std::expected<T, ErrorCode>` 传递可恢复错误。仅在初始化/配置阶段允许异常。

### 已有正例

```cpp
// ArcFaceEmbedder — 返回 optional，无异常
std::optional<ArcFaceEmbedding>
ArcFaceEmbedder::embedAlignedFaceBgr(
    const cv::Mat& faceBgr,
    const EmbedParams& params) noexcept;
```

### 待改造

| 位置 | 当前模式 | 目标模式 |
|------|----------|----------|
| `BioAuth::verifyMulti()` | `bool` + 输出参数 | `Result<AuthOutcome>` |

### 已合规（无需修改）

- `Config.h` — header-only 编译期常量，无运行时加载，无异常抛出
- `ArcFaceEmbedder::embedAlignedFaceBgr()` — 返回 `std::optional`，符合规范

---

## 契约 5: 接口隔离与符号隐藏

### 规则

跨子系统公共 API 必须是纯抽象接口（头文件仅有纯虚函数 + 工厂函数）。实现完全隐藏在 `.cpp` 中。非导出符号使用 `-fvisibility=hidden`。

### 已有正例

```cpp
class IRecognizer {
public:
    virtual ~IRecognizer() = default;
    virtual RecognitionResult recognize(const cv::Mat& face) = 0;
};
std::unique_ptr<IRecognizer> CreateArcFaceRecognizer(const RecognizerConfig&);
```

### 待改造

| 位置 | 问题 | 目标 |
|------|------|------|
| `FaceInferencePipeline` 类 | 具体类暴露全部内部方法 | 拆出 `IFacePipeline` 接口 |
| `Engine` 类 | 公共头文件暴露初始化顺序依赖 | 增加 `Engine::Options` 配置对象 |

---

## 合规总表

| 契约 | 违规数 | 修复状态 |
|------|--------|----------|
| #1 RAII 资源封装 | 2 处 (P0) | 待修复 |
| #2 纯函数管线 | 1 处 (P1) | 待修复 |
| #3 原子状态提交 | 2 处 (P1) | 待修复 |
| #4 错误通道 | 2 处 (P1) | 待修复 |
| #5 接口隔离与符号隐藏 | 2 处 (P2) | 待修复 |

---

*本文件依据 `docs/superpowers/specs/2026-07-06-architecture-governance-design.md` 生成。*
*检查脚本: `scripts/check-raii-violations.sh`*
