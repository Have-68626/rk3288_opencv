# 1. 问题

`MainActivity.java` 已经从界面入口演变成监控系统的总控点。它同时处理视图绑定、权限流转、引擎初始化、采集切换、预览恢复、热重启和生命周期恢复，导致状态修改点分散，后续加功能时很容易改漏。

## 1.1. **活动类职责过载**

`src/java/com/example/rk3288_opencv/MainActivity.java` 在 `onCreate` 中一次性完成了视图查找、采集器创建、偏好读取、权限申请、广播注册、按钮绑定、帧循环和统计循环。一个入口方法同时覆盖 UI、设备、JNI、后台线程四层逻辑，阅读和修改成本都很高。

这种结构的问题不是“代码长”本身，而是改动一个监控流程时，开发者必须同时确认 UI、线程、权限和 native 调用是否被连带影响。比如同一个 `btnHotRestart` 在 `onCreate` 中被重复绑定了多次，说明职责已经堆叠到很难稳定维护。

## 1.2. **状态切换分散且重复**

`startMonitoring`、`initEngine`、`handleCaptureFailure`、`restartMonitoring`、`onPermissionStateChanged`、`onStop` 共同维护 `isRunning`、`engineInitialized`、`pendingStartAfterInit`、`forcedNextScheme`、`restartMonitoringOnStart` 等多个状态位，但没有统一状态机。

这会带来典型的“状态漂移”问题：同一业务状态由多组标志位间接表达，稍微漏掉一次赋值，行为就会不一致。后面如果再加第三种采集方案或新的恢复策略，分支数会继续膨胀。

## 1.3. **配置变更后的重绑逻辑复制**

`rebindViewsAfterConfigChange` 复制了大量 `onCreate` 的视图查找和监听绑定逻辑，包括相机下拉框、采集模式切换、按钮事件和全屏布局恢复。现在同一套交互规则存在两份实现，任何一处新增控件或修正监听，都要同步改两次。

# 2. 收益

重构的核心收益，是把“监控状态编排”从 Activity 的事件堆里提出来，变成可单测、可推演的独立流程。

- 显著降低状态复杂度：把多个布尔变量收敛成有限状态后，核心流转可以从“读遍多个回调”缩到“读一个状态机”。  
- 减少配置变更与热重启回归成本：`onCreate` 与配置变化重绑不再维护两份监听逻辑。  
- 为新增采集方案与恢复策略预留扩展点：独立的 `MonitoringCoordinator` 可扩展策略而不继续膨胀 Activity。  

# 3. 方案

建议把 `MainActivity` 收缩为“界面壳”，把监控流程拆成编排层和绑定层两部分：

- `MainScreenBinder`：负责控件查找与监听注册（可复用，避免重绑复制）。
- `MonitoringCoordinator`：负责状态迁移与决策输出（`UiState/Effects`），Activity 只负责执行 Effects 与渲染 UiState。

# 4. 回归范围

- 正常启动：选择相机/Mock → 开始监控 → 首帧到达 → UI 状态一致。
- 自动采集切换：采集失败 → 重试与降级策略按预期发生。
- 配置变化：横竖屏切换后 UI 绑定与状态显示一致，不出现重复监听覆盖。
- 权限缺失：选择物理相机时进入安全模式，不误启动引擎；Mock 模式可绕过相机权限。
