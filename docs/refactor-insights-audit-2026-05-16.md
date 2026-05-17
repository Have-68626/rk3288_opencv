# 重构洞察落地审计报告（2026-05-16）

## 范围与输入

本次审计按 [.trae/specs/audit-refactor-insights-completion/tasks.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/.trae/specs/audit-refactor-insights-completion/tasks.md) 执行 Task 1–4。

### 输入发现结果

- 预期主输入：`documents/重构洞察-*.md`
  - 已发现 2 条洞察：
    - [重构洞察-MainActivity监控编排耦合问题.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/documents/%E9%87%8D%E6%9E%84%E6%B4%9E%E5%AF%9F-MainActivity%E7%9B%91%E6%8E%A7%E7%BC%96%E6%8E%92%E8%80%A6%E5%90%88%E9%97%AE%E9%A2%98.md)
    - [重构洞察-runFaceInferOnce巨型函数与JSON混用问题.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/documents/%E9%87%8D%E6%9E%84%E6%B4%9E%E5%AF%9F-runFaceInferOnce%E5%B7%A8%E5%9E%8B%E5%87%BD%E6%95%B0%E4%B8%8EJSON%E6%B7%B7%E7%94%A8%E9%97%AE%E9%A2%98.md)
- 补充输入（用户提供的“历史列表/已发现问题”）：
  - `重构洞察-runFaceInferOnce巨型函数与JSON混用问题.md`（与上方 documents 条目合并）

## 洞察条目清单（去重后）

| ID | 条目 | 来源 | 状态 |
|---|---|---|---|
| INS-001 | 去重并统一 MainActivity UI 绑定（MainScreenBinder），修复 HotRestart 重复绑定覆盖 | [重构洞察-MainActivity监控编排耦合问题.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/documents/%E9%87%8D%E6%9E%84%E6%B4%9E%E5%AF%9F-MainActivity%E7%9B%91%E6%8E%A7%E7%BC%96%E6%8E%92%E8%80%A6%E5%90%88%E9%97%AE%E9%A2%98.md) | DONE |
| INS-002 | 引入 MonitoringCoordinator：收敛监控状态与决策，Activity 仅做 Effects 执行与 UI 渲染 | [重构洞察-MainActivity监控编排耦合问题.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/documents/%E9%87%8D%E6%9E%84%E6%B4%9E%E5%AF%9F-MainActivity%E7%9B%91%E6%8E%A7%E7%BC%96%E6%8E%92%E8%80%A6%E5%90%88%E9%97%AE%E9%A2%98.md) | DONE |
| INS-003 | runFaceInferOnce 阶段化拆分 + 统一 JsonWriter（避免手写 JSON 拼接与分散错误处理） | [重构洞察-runFaceInferOnce巨型函数与JSON混用问题.md](file:///d:/19842/Documents/GitHub/rk3288_opencv/documents/%E9%87%8D%E6%9E%84%E6%B4%9E%E5%AF%9F-runFaceInferOnce%E5%B7%A8%E5%9E%8B%E5%87%BD%E6%95%B0%E4%B8%8EJSON%E6%B7%B7%E7%94%A8%E9%97%AE%E9%A2%98.md) | PARTIAL |

## 条目详情

### INS-001：去重并统一 UI 绑定（MainScreenBinder）+ HotRestart 绑定修复

**摘要**

将 `MainActivity` 中重复的 `findViewById + setOnClickListener` 抽离为独立绑定层 `MainScreenBinder`，并让 `onCreate()` 与配置变化后的重绑复用同一绑定入口；同时将 HotRestart 行为固化为“保存偏好 + 热重启”，避免因重复绑定导致行为被覆盖。

**可验证建议清单（可检查动作表述）**

1) 新增并使用 `MainScreenBinder.java` 作为唯一的 View 绑定与（静态）监听注册入口  
2) `MainActivity.onCreate()` 与 `rebindViewsAfterConfigChange()` 均调用统一的绑定方法（避免复制粘贴）  
3) `btnHotRestart` 监听只绑定一次，并通过封装方法保证“保存偏好 + 热重启”一起执行  
4) 仓库搜索层面不存在 `btnHotRestart` 的重复 `setOnClickListener` 残留

**证据（仓库定位）**

- 绑定层实现： [MainScreenBinder.java:L20-L216](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainScreenBinder.java#L20-L216)
- 统一绑定入口与复用：
  - [MainActivity.java:L333-L399](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L333-L399)（`bindMainScreen(...)`：统一绑定与监听注册）
  - [MainActivity.java:L734-L805](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L734-L805)（`onCreate()` 调用 `bindMainScreen(true)`）
  - [MainActivity.java:L919-L965](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L919-L965)（`rebindViewsAfterConfigChange()` 调用 `bindMainScreen(true)`）
- HotRestart 行为固化与调用链：
  - [MainActivity.java:L244-L331](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L244-L331)（`MainScreenBinder.Callbacks#onHotRestartClicked -> performHotRestartWithPrefsPersist()`）
  - [MainActivity.java:L666-L673](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L666-L673)（`performHotRestartWithPrefsPersist()`）
- 重复绑定残留检查（搜索结果）：
  - `btnHotRestart.setOnClickListener` 仅出现在 [MainScreenBinder.java:L153-L171](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainScreenBinder.java#L153-L171)

**状态：DONE**

理由：建议项 1–4 均可在仓库中定位到明确实现证据，且重复绑定残留已消失。

**下一步建议（不在本次审计中直接实施）**

- 若后续 UI 控件继续增加，建议把 `switchOverlay/switchFlipX/switchFlipY` 等“动态监听 + 状态同步”也定义为 Binder 的一组可复用绑定函数，避免 Activity 进一步膨胀。

---

### INS-002：引入 MonitoringCoordinator（决策收敛 + UiState/Effects）

**摘要**

将“是否启动/停止/重启/失败恢复/权限安全模式/采集方案切换”等决策从 `MainActivity` 收敛到纯 Java 的 `MonitoringCoordinator`，由 Coordinator 输出 `UiState`（Activity 渲染）与 `Effects`（Activity 执行并回传结果），降低 Activity 复杂度并提高可测性。

**可验证建议清单（可检查动作表述）**

1) 新增 `MonitoringCoordinator.java`，包含 `UiState/Decision/Effect/Inputs` 等数据结构与决策入口  
2) `MainActivity` 以“dispatch -> applyEffects -> renderUi”的单一通路驱动监控 UI 与动作执行  
3) 为 Coordinator 增加可运行的单元测试（最少覆盖：无输入、权限不足、Mock 启动、失败恢复、权限丢失、overlay 可见性）

**证据（仓库定位）**

- Coordinator 本体： [MonitoringCoordinator.java:L7-L509](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MonitoringCoordinator.java#L7-L509)
- Activity 集成与单一通路：
  - [MainActivity.java:L144-L144](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L144-L144)（持有 `monitoringCoordinator`）
  - [MainActivity.java:L401-L441](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L401-L441)（`dispatchMonitoringDecision / renderMonitoringUi`）
  - [MainActivity.java:L443-L565](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java#L443-L565)（`applyMonitoringEffects(...)`：Effects 执行层）
- 单元测试覆盖： [MonitoringCoordinatorTest.java:L14-L150](file:///d:/19842/Documents/GitHub/rk3288_opencv/tests/unit/java/com/example/rk3288_opencv/MonitoringCoordinatorTest.java#L14-L150)

**状态：DONE**

理由：Coordinator + 集成 + 单测均可在仓库内定位到证据，且单测已存在可复查的断言覆盖。

**下一步建议（不在本次审计中直接实施）**

- 将 `MonitoringCoordinator.syncFromActivity(...)` 的输入字段与 Activity 侧状态字段建立更明确的数据契约（例如：哪些字段必须一致、何时调用、允许的空值），减少未来维护时的“状态双写”风险。

---

### INS-003：runFaceInferOnce 阶段化拆分 + 统一 JsonWriter

**摘要**

洞察建议将 `runFaceInferOnce` 从“巨型函数 + 手写 JSON 拼接”演进为“阶段化拆分 + 统一 JsonWriter 构建 + 集中式错误处理”。当前仓库实现已明显向该方向演进：引入了 `FaceInferContext/FaceInferMetrics`，并将 JSON 输出收敛为 `buildOutcomeJson/buildImageLoadFailureJson` 等构建函数；但主流程仍以单个 `runFaceInferOnce` 内的长链路 + `stage/msg` 控制为主，阶段化拆分尚未完全完成。

**可验证建议清单（可检查动作表述）**

1) `runFaceInferOnce` 主流程拆分为多个小阶段函数（load/detect/align/embed/loadGallery/search/decision/json），主函数仅编排与早返回  
2) JSON 输出不再出现“手写字符串拼接（ostringstream + 手工逗号/转义）”的并行路径，统一经 `JsonWriter` 或统一 builder 输出  
3) 错误处理通过集中式 failure builder 早返回，避免在主流程中穿插大量 `stage.empty()` 判定  

**证据（仓库定位）**

- `runFaceInferOnce` 仍为长函数，且主流程使用 `stage/msg` + `do { ... } while(false)` 方式组织分支：
  - [FaceInferencePipeline.cpp:L679-L998](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp#L679-L998)
- 已存在上下文与指标结构，说明阶段化拆分已部分落地：
  - [FaceInferencePipeline.cpp:L301-L335](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp#L301-L335)
- JSON 输出已有集中 builder，且主路径使用 `buildOutcomeJson(...)`（JsonWriter）：
  - [FaceInferencePipeline.cpp:L464-L510](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp#L464-L510)
  - [FaceInferencePipeline.cpp:L920-L951](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp#L920-L951)
- 仍存在“主流程内穿插 stage 判定并在末尾统一决策”的结构，未完全演进为“每阶段函数返回 success/failure 早返回”：
  - [FaceInferencePipeline.cpp:L692-L918](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/src/FaceInferencePipeline.cpp#L692-L918)

**状态：PARTIAL**

理由：
- 建议项 2（统一 JSON builder/JsonWriter）基本达成：主输出通过 `buildOutcomeJson` 完成，未见第二套“手写 JSON 拼接”路径。  
- 建议项 1/3（彻底阶段化拆分 + 早返回失败 builder）尚未完全达成：主流程仍为单个大函数，并依赖 `stage/msg` 状态穿插控制。

**下一步建议（不在本次审计中直接实施）**

- 若要完成闭环，建议按洞察文档的阶段列表继续拆分为 `loadImage/detectFaces/alignFace/computeEmbedding/loadGallery/searchTopK/makeDecision` 等函数，并将 `stage/msg` 控制收敛为“失败即 return makeFailureOutcome(...)”。

## 抽样回归（构建/测试）

### 抽样 1：默认环境直接构建（失败，但错误可解释）

执行命令：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
.\gradlew.bat --no-daemon :app:assembleDebug :app:testDebugUnitTest
```

结果：失败。核心错误为 OpenCV CMake 优化参数与当前 Android Clang 工具链不兼容（示例输出片段）：

- `OpenCVCompilerOptimizations.cmake:654: Compiler doesn't support baseline optimization flags`

影响：在当前机器的 `OPENCV_ROOT=D:\ProgramData\OpenCV\opencv-4.10.0` 配置下，无法直接完成 native 构建；但该问题与本次两条 Java 重构洞察的落地证据无直接冲突。

### 抽样 2：跳过 OpenCV（验证 Java 重构相关路径可编译+单测可跑）

执行命令：

```powershell
cd D:\19842\Documents\GitHub\rk3288_opencv
.\gradlew.bat --no-daemon -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest
```

结果：`BUILD SUCCESSFUL`。说明在“跳过 OpenCV”配置下，Android 模块可编译、并可执行单元测试（覆盖 `MonitoringCoordinatorTest`）。

## 总体结论

- 本次审计覆盖 `documents/重构洞察-*.md` 的 2 份洞察文档，并结合历史列表去重合并，最终得到 3 条洞察条目。
- 其中 2 条洞察（MainActivity 绑定去重、MonitoringCoordinator 收敛编排）可在仓库中定位到清晰证据（代码位置 + 行范围 + 单测/构建验证），完成度判定为 DONE。
- `runFaceInferOnce` 洞察已部分落地（引入 Context/Metrics 与集中 JSON builder），但主流程仍为巨型函数且依赖 `stage/msg` 控制，完成度判定为 PARTIAL。
- 构建回归层面：默认 OpenCV 配置在当前环境失败，但可通过 `-PRK_SKIP_OPENCV=true` 抽样验证 Java 重构相关路径（编译 + 单测）可复现。

## 待办清单（按优先级）

1) **补齐 runFaceInferOnce 阶段化拆分**：继续按洞察方案将 `runFaceInferOnce` 拆成阶段函数，并统一 failure builder 早返回，消除 `stage/msg` 状态穿插。  
2) **澄清 OpenCV Android 构建契约**：为 Android 构建明确“应使用的 OpenCV 形态”（Android SDK / 源码交叉编译产物 / 预编译 AAR 等）与对应 `OPENCV_ROOT` 设置方式，避免误用 Windows OpenCV 源码配置导致 CMake 失败。  
