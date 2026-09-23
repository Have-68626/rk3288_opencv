<!-- DOCSYNC_DOC
type=runbook
status=living
as_of=2026-09-18
-->

# RK3288 Android 设备验收清单（人工执行）


**执行状态**: **待真机执行**。本文是手工验收清单，步骤存在不等于场景已通过；无论断结果不得标记为“已通过”。

**适用对象**: RK3288（`armeabi-v7a`）与 ARM64 高精度机型；Android App 主链路。

**自动化边界**: 数据采集可用 `scripts/run-android-face-regression.ps1`（私有媒体回归，仅聚合并匿名化报告；其中 A1 的稳态 FPS 判据已能自动给出显式结论行，口径见 A1）；**判定与留档仍由人工完成**。性能口径亦可参考 `docs/guides/windows-service/performance.md` 的留档要求。

## 0. 前置条件

1. 目标设备已连接：`adb devices` 可见（RK3288 常用 `adb connect 192.168.5.2:5555`；设备时钟可能未校准，注意时间戳解读）。
2. 已安装 debug APK：`gradlew.bat --no-daemon :app:assembleDebug`，产物 `app/build/outputs/apk/debug/app-debug.apk`。
3. 记录构建信息：commit 哈希、构建类型（debug/release）、ABI、是否 ARM64、模型 profile（`rk3288_light` / `arm64_accuracy`）。
4. 记录设备画像（脱敏）：`getprop ro.product.model`、`ro.build.version.sdk`、`ro.product.cpu.abi`、`/proc/meminfo`、`df /data`。
5. 模型准备：按 `docs/designs/model-acquisition-import.md` 导入 ZIP；对照组（第 11 项）需先保持“未导入”态。
6. 采集通道：`adb logcat` 关键来源包括 `RK3288_JNI`、`StatusService`（`FPS: ... | CAP: ... | LAT: ... | CPU: ... | MEM: ...`）、`MainActivity`（`p95=%.0fms`、`lat_ms=`、`ui_fps=`）、`RealtimeFacePipeline`（`基准超预算: p95_ms=...`）与 `PerfReporter` 的 p50/p95 输出。

## 1. 性能指标

### A1. RK3288 轻量档 ≥ 15 FPS

- **步骤**：使用 `rk3288_light` profile，前摄 720p 预览，画面含 1 张大脸；稳定运行 ≥ 60s。
- **判据**：稳态处理帧率 **≥ 15 FPS**（`ui_fps`/`FPS` 序列的稳态中位数；剔除启动前 5s）。
- **证据**：logcat 中 `MainActivity … inferenceAutoTune … ui_fps=<v>` 行（经 `AppLog.i` 落入 logcat），记录逐次原始值。**注意**：`StatusService` 的 `FPS: … | CAP: …` 只 `setText` 到悬浮窗、不进 logcat，`PerfReporter` 也无 fps 字段——这两者都不是 logcat 侧的 FPS 来源。

**自动化边界（本项已部分自动化）**：`scripts/run-android-face-regression.ps1` 已把「暖机剔除 + 稳态中位数 + 判据结论」落地。报告 `summary.json` 新增 `steady_state`（稳态样本数、每指标稳态 `count/min/p50/p95/max`）与 `fps_criterion`（`metric`/`threshold_fps`/`steady_sample_count`/`steady_p50`/`satisfied`/`conclusion`）；stdout 另输出 `steady_sample_count=`、`fps_steady_p50=` 与显式结论行 `fps_criterion=`。`conclusion` 取值 `satisfied` / `not_satisfied` / `insufficient_steady_samples` / `missing_fps_samples` / `steady_state_disabled`——**采不到样本或稳态样本不足时显式标注，绝不安静地判为通过**。

- **稳态口径（可复现，但为采样计数近似）**：暖机窗口按**采样计数近似**剔除——`warmup_fraction = WarmupSec / 采集窗口`，对每个指标按采集顺序丢弃前 `floor(count × warmup_fraction)` 个样本，其余为稳态样本。**采集窗口由主机时钟（Stopwatch）测量，不使用设备 wall-clock**：RK3288 测试设备时钟未校准（停在 2017 年），且 `adb logcat -d -v brief` 输出不含时间戳、App 日志没有帧计数等单调量，逐行时间映射不可得，故只能按采样计数近似（采样密度不均匀时会有偏差，已在报告 `steady_state.excluded_note` 写明）。相关参数：`-WarmupSec`（默认 `5`，对齐本判据的“剔除启动前 5s”）、`-FpsThreshold`（默认 `15`，对齐轻量档）、`-MinSteadySamples`（默认 `5`，稳态样本下限）、`-NoSteadyState`（显式关闭稳态块，回到仅全量口径）。脚本另有 `-LogcatFile <路径>` 离线测试接缝（默认空 = 走 adb 真机采集），供无设备时验证该口径。
- **采样节奏与前置条件（决定脚本是否“看起来坏了”）**：`ui_fps` 仅由 `MainActivity` 的 `inferenceAutoTune` 日志产生，**且仅在识别节流为 auto 模式**下才有——非 auto 模式会移除该 updater（`MainActivity.java` 在 `!"auto".equals(recMode)` 时 `return`），**一个样本都不产生**。其节奏为**约 30s 一个样本**（同处心跳阈值 `30_000ms`，进入 auto 时初始化、之后每满 30s 输出一次）。据此以默认 `-MinSteadySamples 5` 反推：需要采集窗口 **≳ 150s**（默认 `SettleSec=3` 时 `-DurationSec ≳ 147s`；因 5s 暖机小于单个 30s 采样间隔，暖机剔除通常为 0 个样本，稳态数 ≈ 窗口/30，故 5 个稳态样本 ≈ 150s）。考虑 auto 激活延迟与抖动，建议 `-DurationSec ≥ 180s` 留裕量。对照：默认 `-DurationSec 60` 只有约 2 个样本，脚本会**正确地**报 `insufficient_steady_samples`；**未开启 auto 模式则报 `missing_fps_samples`**。这两种结论都是**显式标注、不是脚本故障**——是本项自动化的已知前置。
- **短窗口会硬失败（`exit 7`，本项自动化的行为变更）**：当 `-WarmupSec`（默认 `5`）**≥** 采集窗口（默认 `SettleSec 3 + DurationSec`）时，脚本以 `Fail 7` 退出——即 `-DurationSec ≤ 2` 这样短到暖机窗口都装不下的运行，**不再产出报告**。这是刻意的：稳态样本数为 0 时若仍出报告，等于用一个空集合的中位数冒充「稳态帧率」，正是本项目零容忍的静默假绿。需要旧行为（只出全量口径）时显式传 `-NoSteadyState`。
- **仍须人工**：profile/预览分辨率/画面内容（1 张大脸）等场景条件、设备温度与后台负载控制、原始数值留档与最终签署。脚本只做采集与聚合，`fps_criterion` 结论行只是辅助，**不替代人工验收判定**。

### A2. 人脸框首现 p95 ≤ 800ms

- **步骤**：固定机位，人脸从画面外进入至框首次出现；重复 ≥ 30 次（或用回归脚本驱动），记录每次首现延迟。
- **判据**：**p95 ≤ 800ms**。
- **证据**：逐次延迟原始值 + p50/p95 汇总。

### A3. ARM64 高精度检测 p95 ≤ 250ms，否则必须拒绝并显示原因

- **步骤**：在 ARM64 构建上使用 `arm64_accuracy` profile 激活；观察 `RealtimeFacePipeline` 基准（默认 warmup 10 / timed 50）。
- **判据**：
  - p95 **≤ 250ms** → 允许激活；
  - p95 **> 250ms** → 管线**必须拒绝激活**并显式给出原因（`基准超预算: p95_ms=...` 或等价 reason），**不得静默启用**该 profile。
  - 说明：`RealtimeBenchmarkConfig.p95BudgetMs=250.0`；`enforceBudget` 的**平台默认值由 `RealtimeFacePipeline.cpp` 的 `kDefaultEnforceBudget` 在构造时决定**（未注入构建期显式选项 `RK_BENCH_ENFORCE_BUDGET` 时回落 `__aarch64__`），头文件里的字段默认值**恒为 `false`** 以保 ODR 安全（不在头文件按架构分支）。故 ARM64 默认强判、非 ARM64 只测量不强判。
- **证据**：基准输出与最终 `effective`/`reason`。

### A4. 6.25% 面积标准样本稳定检出

- **步骤**：标准样本中，人脸框面积占画面面积约 **6.25%**（约宽高各 25%）；在标准距离/光照下连续测试 ≥ 30 帧。
- **判据**：稳定检出（无持续漏检；允许的抖动需记录比例，不得出现整段丢失）。

### A5. 冷启动首帧（TTFF）P50 < 600ms / P95 < 900ms

- **步骤**：冷启动（`force-stop` 后首次）应用并触发打开前摄；重复 ≥ 20 次（或用脚本驱动），记录每次首帧耗时。
- **判据**：**P50 < 600ms**，**P95 < 900ms**（目标设备：RK3288 工控机 + 指定摄像头模组）。
- **证据**：`scripts/bench_camera_adb.ps1`（Windows）/ `scripts/bench_camera_adb.sh`（Linux/macOS）采集；应用须稳定输出日志标记 `BENCH_CAMERA TTFF_MS=<n>`（Tag/键名须稳定）与 `BENCH_CAMERA CAPTURE_OK=<n> CAPTURE_FAIL=<n>`。
- **出处**：`docs/research/appendix-c.md` §5.6.2。

### A6. 内存预算 ≤ 512MB

- **步骤**：稳定运行 ≥ 60s（满档），读取应用进程内存峰值（`dumpsys meminfo <pkg>` 的 PSS，或 logcat 的 `MEM:` 字段）。
- **判据**：**应用常态运行内存峰值 ≤ 512MB**（给系统与相机栈留余量）。
- **⚠️ 无自动断言**：本项**当前没有任何自动断言**——`≤ 512MB` 仅在 `docs/RK3288_CONSTRAINTS.md` §1.2 作为建议值存在，**无对应验收脚本或 CI 检查在跑**；引用与留档时**不得假定它「已被验证」**，必须人工测量并记录原始值。
- **出处**：`docs/RK3288_CONSTRAINTS.md` §1.2。

### A7. 前后台切换稳定性（50 次切换）

- **步骤**：用 `scripts/stability_switch_50_adb.ps1` 执行 50 轮前后台切换（或手工等价操作）。
- **判据**：**50 次切换后仍可恢复预览与分析；CameraService 重启次数 = 0**。
- **证据**：脚本输出 + logcat 关键词统计（`cameraserver` / `CameraService` / `restarting`）；应用须输出 `BENCH_CAMERA CAMERA_SERVICE_RESTART=<n>`。
- **出处**：`docs/research/appendix-c.md` §5.6.2。

> A5 / A6 / A7 的判据值集中口径与出处另见 [RK3288_OPTIMIZATION.md](../RK3288_OPTIMIZATION.md) §5「判据阈值表（唯一事实源）」；本清单保留可执行的步骤与数字。

## 2. 检测与跟踪正确性

### B1. 单脸无重复框；2–3 人各自身份

- **步骤**：先单人正脸；再 2–3 人同框。
- **判据**：单脸时**恰好 1 个框**（无重复/嵌套框）；多人时框数与人数一致，身份标签稳定、不串人。

### B2. 固定背景无 3 连确认幽灵轨迹

- **步骤**：镜头对准**无人**的固定背景，持续 ≥ 30s。
- **判据**：不出现被**连续 3 帧确认**的幽灵轨迹（track 确认门槛见 `TrackCoordinator` 的 IoU/stableId/TTL 逻辑）。

### B3. 四边缘翻转恒定

- **步骤**：人脸依次靠近画面上/下/左/右四边缘并停留。
- **判据**：翻转/跟踪行为恒定，框不跳变、不漂移、不产生重复 ID；离开再进入时新 ID 合理。

### B4. 前摄默认镜像持久

- **步骤**：确认前摄预览为默认镜像；冷启动重启 App 后再次检查。
- **判据**：镜像状态**跨重启保持**；不清零、不反转。

## 3. 注册闭环（任务 18 接线后执行）

> 前置：注册 UI 接线与冷启动恢复属任务 18；未接线前第 C 组应记为“未就绪/阻塞”，不得判通过。

### C1. 注册 → 重启 → 识别 → 删除 完整闭环

- **步骤**：注册 1 人（8 样本，正4/左2/右2）→ 强制停止并冷启动 → 识别该人 → 删除该人 → 再次识别应失败。
- **判据**：注册成功（模板落盘）；重启后注册库与激活态恢复；识别命中；删除后不再命中。
- **证据**：每步状态与日志（不含模板内容）。

### C2. 无明文模板 / 照片 / 注册库泄露

- **步骤**：注册后检查应用私有目录（`files/face_templates/`、`files/face_models/`）与 `adb logcat`。
- **判据**：
  - 模板文件为密文（`FeatureTemplateEncryptedStore`），注册表 `face_registry_v1.dat` 仅含元数据、**无特征向量**；
  - 无任何原图/照片落盘；
  - 日志不含模板、向量、userId 明文或密钥材料。

### C3. 低版本（API < 23）只读降级（如适用）

- **判据**：写操作返回 `UNSUPPORTED_PLATFORM` 并解释；`list/get` 只读预览放行；注册入口禁用并说明。

## 4. 模型缺失降级（禁止静默回退 LBP）

### D1. 未导入模型时降级预览 + 显式 `missing_model`

- **步骤**：清除已导入模型（或全新安装，不导入任何模型），进入预览。
- **判据**：
  - 应用保持**降级预览**（不崩溃、不黑屏）；
  - 显式报告 **`missing_model`**（`getRuntimeStatus` 的 `effective=false`、`reason="missing_model"`，UI 显示“缺失原因=…”）；
  - **禁止静默回退 LBPH**：不得在未导入模型时启用 LBP/LBPH 识别路径，也不得把 `effective` 报成 true。
- **证据**：`getRuntimeStatus` 输出、UI 文案、logcat（不应出现 LBP 回退成功日志）。

## 5. 结果留档要求

每次正式验收至少记录：

- commit 哈希、构建类型、ABI（是否 ARM64）、模型 profile 与模型 SHA-256；
- 设备画像（脱敏序列号、SDK、CPU ABI、内存/存储）；
- 每项的原始测量值、p50/p95、通过的判据与阈值；
- 失败项的失败证据（日志、截图/录屏、复现步骤）。

未附上述信息的截图或口头结论不能标记为“已通过”。

## 6. 覆盖对照

| 验收项 | 本文位置 |
|---|---|
| RK3288 轻量 ≥15 FPS | A1 |
| 人脸框首现 p95 ≤ 800ms | A2 |
| ARM64 高精度 p95 ≤ 250ms，否则拒绝并显原因 | A3 |
| 6.25% 面积标准样本稳定检出 | A4 |
| 单脸无重复框 / 2–3 身份 | B1 |
| 固定背景无 3 连确认幽灵轨迹 | B2 |
| 四边缘翻转恒定 | B3 |
| 前摄默认镜像持久 | B4 |
| 注册/重启/识别/删除完整 | C1 |
| 无明文模板/照片/注册库泄露 | C2 |
| 未导入模型时降级预览 + 显式 `missing_model` | D1 |
| 冷启动首帧 TTFF（P50 < 600ms / P95 < 900ms） | A5 |
| 内存预算 ≤ 512MB（无自动断言） | A6 |
| 前后台切换稳定性（50 次，CameraService 重启次数 = 0） | A7 |

**采集工具对照**（判据由人工判定，脚本只做采集与聚合）：

- A1 稳态 FPS 判据：`scripts/run-android-face-regression.ps1`（**A1 的唯一自动判据实现者**，结论落在 `summary.json` 的 `fps_criterion`；`-LogcatFile` 为离线接缝）。
- A5 TTFF 采集：`scripts/bench_camera_adb.ps1` / `scripts/bench_camera_adb.sh`。
- A7 前后台切换：`scripts/stability_switch_50_adb.ps1`。

A1 / A2 / A3 / A5 / A6 / A7 的判据值集中口径与出处见 [RK3288_OPTIMIZATION.md](../RK3288_OPTIMIZATION.md) §5。
