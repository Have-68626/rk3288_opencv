<!-- DOCSYNC_DOC
type=architecture
status=living
as_of=2026-09-26
-->

# RK3288 OpenCV 架构契约

## 背景

本文档定义所有 C++ 子系统（Core Engine / Windows Service / Android JNI）必须遵守的 5 条架构规则。每条规则附带已有正例（positive example）、违规清单和自动化检查方法。

---

## 契约 1: RAII 资源封装

### 规则

所有 `lock/unlock`、`open/close`、`allocate/free`、`begin/end` 配对必须封装为 RAII 类型后方可使用。裸配对在 code review 中直接拒绝。

### 已有正例

- `SocketGuard`（Windows `HttpFacesServer.cpp`）— 析构自动 `closesocket`
- `WindowReleaser`（`src/cpp/jni/preview.cpp`）— 析构自动 `ANativeWindow_release`
- `ScopedWindowLock`（`src/cpp/jni/preview.cpp`）— 析构自动 `ANativeWindow_unlockAndPost`
- `ScopedBitmapLock`（`src/cpp/jni/preview.cpp`）— 析构自动 `AndroidBitmap_unlockPixels`

### 违规清单

| 文件 | 行号 | 裸调用 | 优先级 |
|------|------|--------|--------|
| （无） | — | — | — |

原 `native-lib.cpp` 的两处裸配对（`ANativeWindow_lock`/`unlockAndPost`、`AndroidBitmap_lockPixels`/`unlockPixels`）已由 `src/cpp/jni/preview.cpp` 的 RAII 类封装（#444）；`native-lib.cpp` 现零裸配对。

> ⚠️ **上表「（无）」的范围限定 —— 请勿读成「契约 #1 整体已闭环」**（2026-09-26 二次审计订正）：
> 机械检查（`scripts/check-raii-violations.sh`）只扫描 **2 个符号族** —— `ANativeWindow_lock`/`ANativeWindow_unlockAndPost`
> 与 `AndroidBitmap_lockPixels`/`AndroidBitmap_unlockPixels`。而**本契约的规则原文还覆盖
> `open/close`、`allocate/free`、`begin/end` 三个配对族**，后三族**没有**机械检查，且**确实存在**裸配对
> （审计逐个读过，均非外包第三方代码；下表的行号为 **2026-09-26 快照**，会漂移，以函数/符号为准）：
>
> | 文件 | 裸配对 | 说明 |
> |---|---|---|
> | `src/cpp/src/pipeline/PerfReporter.cpp` | `fopen`（:57）→ `fclose`（:64） | 函数内手工配对，未封装 |
> | `src/cpp/src/MppDecoder.cpp` | `std::fopen`（:90）→ `std::fclose`（:253） | 成员句柄跨 `open()`/`close()` 两个方法手工配对 |
> | `src/win/src/HttpFacesServer.cpp` | `socket`（:208）→ `closesocket`（:220 / :224 / :236 三个错误出口 + **:253 `stop()` 的正常关闭**）；accept 循环（:299 / :371）亦**手写关闭** | **同文件 :44 的结构体 `SocketGuard`**（:47 是其析构函数）**只在一个位置用上**（:728 的 client socket）⇒ 上面那些 socket（含监听与 accept）**都**是手写配对 |
>
> ⇒ 正确读法：「**#444 已把 `ANativeWindow`/`AndroidBitmap` 两族清零**」，**不是**「契约 #1 已闭环」。
> `open/close` 族的收口（补 RAII 或把扫描面扩到该族）已登记为独立待办。

### 自动检查

```bash
scripts/check-raii-violations.sh
```

对全部 tracked `*.cpp`/`*.h` 做**块级结构判定**（非 grep 文本匹配）：先剔除注释与字符串/字符字面量，再以花括号平衡定位 `ScopedWindowLock` / `ScopedBitmapLock` 的类体区间，落在该区间之外的裸 `ANativeWindow_lock` / `AndroidBitmap_lockPixels` 计为违规。

> 已知边界（审计实测；逐条与 `scripts/check-raii-violations.sh` 头部登记保持同一口径）：
> - `};` 与紧随的 `{` 落在**同一行**时（如 `}; {` / `} void g(){`），该行花括号净变化为 0 ⇒ **该行之后新开块的区间内**豁免边界失效、**该区间内的裸调用漏报**；区间结束后检查恢复正常。⚠️ **不是**「整文件免检」——若豁免区因此一直不闭合，下面的安全网会报 WARNING 并计入违规（合成件实测：`}; {` 触发 rc=4、脚本判 **FAIL**）。**未修**（需更完整的作用域模型，超出本文本工具的射程）。
> - ⚠️ **另一条静默漏报（2026-09-26 二次审计实测复现，未修）**：裸调用与封装类的闭合 `};` **同行**时（如 `}; ANativeWindow_lock(a,&b,0);`），该行被判为仍在豁免区内 ⇒ 真违规漏报；且豁免区随后正常退出，下面的安全网**也不触发**。成因与上一条同源：判定发生在花括号计数**之前**。
> - ⚠️ **安全网**：若豁免区扫到**文件末尾仍未闭合**（无论何因），脚本会输出「RAII 豁免区未闭合」告警并**计入违规**（退出码非 0）⇒「静默整文件免检」已被改成**可见信号**。
> - 曾与上两条同族的形态「类体内存在**多行 raw string** 导致花括号平衡漂移、豁免永不退出」**已修**（字面量状态跨行保持，raw 串内部不再参与花括号计数）。

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

- `g_previewWindow` / `g_previewGeneration` 的**定义与释放**在 `src/cpp/native-lib.cpp`（`stopAndJoinEngineThreadIfRunning` 释放），⚠️ **释放点不止一处**：`src/cpp/jni/preview.cpp` 在 Surface 重建时也会 `ANativeWindow_release(g_previewWindow); g_previewWindow = nullptr;` 并递增代次（`preview_nativeSetPreviewSurface`）；**设置/取用**在 `src/cpp/jni/preview.cpp`（`preview_nativeSetPreviewSurface` / `preview_nativeRenderFrameToSurface`），**声明**在 `src/cpp/include/jni_shared.h`——多落点分离，使用时须核对全部落点
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
| #1 RAII 资源封装 | 0 处（**仅** `ANativeWindow`/`AndroidBitmap` 两符号族；`open/close` 族未收口，见上节范围限定） | 部分口径已闭环（#444） |
| #2 纯函数管线 | 1 处 (P1) | 待修复 |
| #3 原子状态提交 | 2 处 (P1) | 待修复 |
| #4 错误通道 | 2 处 (P1) | 待修复 |
| #5 接口隔离与符号隐藏 | 2 处 (P2) | 待修复 |

---

*本文件依据 `docs/specs/2026-07-06-architecture-governance-design.md` 生成。*
*检查脚本: `scripts/check-raii-violations.sh`*
