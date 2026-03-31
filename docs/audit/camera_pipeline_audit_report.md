# 相机调用全链路审计报告（audit-camera-pipeline）

## 0. 范围与结论

### 0.1 审计范围
- Java 代码：`src/java/**`（被 [app/build.gradle](file:///d:/19842/Documents/GitHub/rk3288_opencv/app/build.gradle#L39-L47) 通过 `sourceSets.main.java.srcDirs` 引用参与构建）
- Manifest：`app/src/main/AndroidManifest.xml`
- JNI 桥接：`src/cpp/native-lib.cpp`（用于线程模型/资源释放审计与最小修复）

### 0.2 项目现状（与 spec 预期的差异）
- 当前工程未使用 `app/src/main/java/**`（该目录不存在/为空），相机相关 Java 入口集中在 `src/java/**`。
- 当前“相机预览帧回调/拍照/录像”并非 Camera2/CameraX：应用通过 JNI 启动 Native 引擎，由 Native 侧打开视频源并产出渲染帧；Java 侧以固定节流频率拉取渲染帧并显示。
- 当前未实现“应用内拍照/录像状态机”，仅提供：
  - 文件选择作为 Mock 源（`ACTION_OPEN_DOCUMENT`）
  - 调用系统相机 App 拍照作为 Mock 源（`ACTION_IMAGE_CAPTURE`）

### 0.3 结论摘要
本次审计发现并已修复的高风险点集中在权限最小化、生命周期释放、主线程负载与 Native 线程管理：
- 权限：此前动态权限与 Manifest 过度申请（音频/存储/媒体/定位），与“按需申请”要求不符。
- 生命周期：此前仅在 `onDestroy()` 停止监控，App 退后台后仍可能占用相机资源，违反“前后台切换必须释放”要求。
- 预览帧处理：此前以主线程 `Handler` 30fps 拉帧并进行格式转换/拷贝，存在 UI 卡顿风险，且缺乏明确背压策略。
- 多线程：此前 Native 侧 `nativeStart()` 每次启动都 `detach()` 新线程，可能形成并发 `run()`、资源争用与不可控生命周期。

修复后：权限收敛到 `CAMERA`，退后台释放并支持回前台自动恢复，拉帧搬离主线程并采用“UI 确认后再继续下一帧”的背压策略，Native 引擎线程可 join 且具备“防重复启动”保护。

## 1. 入口盘点与数据流

### 1.1 Java 入口（控制面）
- 主入口 Activity：`src/java/com/example/rk3288_opencv/MainActivity.java`
  - 启停：`startMonitoring()` / `stopMonitoring()`
  - 权限状态机：`PermissionStateMachine` 回调 `onPermissionStateChanged(...)`
  - 相机枚举：`discoverCameras()`（Camera2 `CameraManager` + `CameraCharacteristics`）

### 1.2 权限入口
- `src/java/com/example/rk3288_opencv/PermissionStateMachine.java`
  - 运行时权限：仅 `CAMERA`
  - 拒绝/永久拒绝：通过 `shouldShowRequestPermissionRationale` 区分并引导到设置页

### 1.3 JNI/Native 入口（数据面）
- JNI：`src/cpp/native-lib.cpp`
  - `nativeInit(...)` / `nativeInitFile(...)`：初始化引擎与视频源
  - `nativeStart()` / `nativeStop()`：启动/停止引擎线程
  - `nativeGetFrame(Bitmap)`：从引擎拉取渲染帧写入 `Bitmap`
- 引擎：`src/cpp/include/Engine.h` + `src/cpp/src/Engine.cpp`
  - `Engine::initialize(...)` 打开视频源（cameraId 或 filePath）
  - `Engine::run()` 循环拉帧 → 处理 → 更新 `renderFrame`
  - `Engine::stop()` 关闭视频源

### 1.4 数据流（端到端）
1) UI 触发“开始监控”  
2) `MainActivity.startMonitoring()` → `nativeStart()`  
3) Native 线程执行 `Engine::run()`（内部打开/拉取视频源并持续处理）  
4) Java 侧通过 `nativeGetFrame(Bitmap)` 拉取 `Engine::renderFrame` 并显示到 `ImageView`  
5) UI/生命周期触发停止 → `nativeStop()` → `Engine::stop()` → 释放视频源/相机句柄

## 2. 问题清单（风险分级）

| ID | 风险 | 类别 | 位置 | 触发/复现 | 影响 |
| :-- | :-- | :-- | :-- | :-- | :-- |
| P0-01 | 高 | 权限合规 | [AndroidManifest.xml](file:///d:/19842/Documents/GitHub/rk3288_opencv/app/src/main/AndroidManifest.xml) / [PermissionStateMachine.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/PermissionStateMachine.java) | 首次进入页面申请权限 | 过度权限申请导致合规风险与转化下降 |
| P0-02 | 高 | 生命周期/释放 | [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java) | 监控运行时按 Home/切后台 | 后台持有相机导致黑屏/占用/系统回收异常 |
| P0-03 | 高 | 多线程并发 | [native-lib.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/native-lib.cpp) | 连续快速 start/stop 或多次 start | Engine 线程重复启动、资源争用、不可预期崩溃 |
| P1-01 | 中 | 性能/背压 | [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java) | 运行监控 + UI 交互 | 主线程拉帧导致卡顿；无明确背压策略时易抖动 |
| P1-02 | 中 | 兼容性 API | [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java) / [StatsRepository.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/StatsRepository.java) / [AppLog.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/AppLog.java) | minSdk=21 设备运行/CI lint | API 级别不兼容导致 lint 阻断与潜在运行时崩溃 |
| P2-01 | 低 | 产品能力缺口 | 全链路 | spec 期望拍照/录像状态机 | 现阶段仅支持 Mock；若需要量产拍照/录像需补齐能力 |

## 3. 已落地修复（最小变更优先）

### 3.1 权限最小化（P0-01）
- 动态权限：收敛为仅申请 `CAMERA`  
  - 变更： [PermissionStateMachine.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/PermissionStateMachine.java)
- Manifest 权限：移除与当前功能无关的音频/存储/媒体/定位权限，仅保留 `CAMERA` 与 `SYSTEM_ALERT_WINDOW`  
  - 变更： [AndroidManifest.xml](file:///d:/19842/Documents/GitHub/rk3288_opencv/app/src/main/AndroidManifest.xml)

验收：
- 首次进入仅弹出相机权限；拒绝进入安全模式，不循环弹窗；永久拒绝可跳转设置页。

### 3.2 前后台切换释放并可恢复（P0-02）
- 退后台自动停止监控并释放资源：`onStop()` 中 stop  
- 回前台自动恢复：若退后台前处于运行态，则 `onStart()` 自动 start（前置权限评估）
  - 变更： [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java)

验收：
- 运行中按 Home：相机释放；回到前台自动恢复，不崩溃不黑屏。

### 3.3 拉帧脱离主线程 + 明确背压（P1-01）
- 将 `nativeGetFrame()` 拉帧放到 `HandlerThread` 执行，主线程仅做 `ImageView` 更新  
- 背压策略：只有当 UI 完成一次“交换/显示”后才会调度下一次拉帧（避免回调队列无限增长）
  - 变更： [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java)

### 3.4 Native 引擎线程可控（P0-03）
- `nativeStart()`：检测线程是否已运行，避免重复启动  
- `nativeStop()`：触发 stop 并 join，保证一次启动对应一次退出  
- `nativeInit/nativeInitFile()`：初始化前若线程在跑，先 stop+join，避免重入初始化与资源争用  
  - 变更： [native-lib.cpp](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/cpp/native-lib.cpp)

### 3.5 minSdk=21 兼容与 lint 阻断修复（P1-02）
- 替换 `ThreadLocal.withInitial`（API 26）为兼容写法： [AppLog.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/AppLog.java)
- `Settings.canDrawOverlays`（API 23）增加版本保护： [MainActivity.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/MainActivity.java) / [PermissionStateMachine.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/PermissionStateMachine.java)
- `Debug.MemoryInfo.getMemoryStat`（API 23）增加版本保护： [StatsRepository.java](file:///d:/19842/Documents/GitHub/rk3288_opencv/src/java/com/example/rk3288_opencv/StatsRepository.java)
- `local.properties` 转义修复以通过 lint： [local.properties](file:///d:/19842/Documents/GitHub/rk3288_opencv/local.properties)

### 3.6 构建稳定性（外部 Native 构建易抖动）
- 增加 `org.gradle.workers.max=1`，减少 Windows 环境下 externalNativeBuild 并发导致的 `ninja.exe` 异常退出概率  
  - 变更： [gradle.properties](file:///d:/19842/Documents/GitHub/rk3288_opencv/gradle.properties)

## 4. 后续改进建议（分阶段）

### 4.1 短期（不改架构）
- 将“帧尺寸”从固定 640×480 改为跟随 Native 输出协商（避免 resize 带来的额外开销与画质损失）。
- 为 `nativeInit/nativeStart/nativeStop` 引入更明确的状态机与失败原因上报（Java 侧可提示“相机被占用/无权限/无帧超时”）。

### 4.2 中期（可控演进）
- 将“数据面拉帧”抽象为独立组件（FramePuller），与 UI 生命周期解耦，便于 instrumentation 测试与替换实现。
- 将“设备能力枚举”结果持久化输出到 `errorlog/`（或测试报告目录），形成设备能力矩阵与黑名单策略基础。

### 4.3 长期（对齐 spec 完整能力）
- 若需要量产“拍照/录像”：建议明确采用 CameraX（Preview + ImageAnalysis + VideoCapture），并将算法处理保持在 Native 侧；补齐录像前台服务与 Android 13/14 FGS 类型适配。

