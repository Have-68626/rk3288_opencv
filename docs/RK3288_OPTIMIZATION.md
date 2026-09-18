<!-- DOCSYNC_DOC
type=reference
status=living
as_of=2026-09-18
-->

# RK3288 基准口径与优化方向

本文是 RK3288 目标设备**基准口径与优化方向**的唯一事实源。默认基线的定位不是「最慢」，
而是「最可控、最易定位、最易回滚」。设备画像（外设、节点、BSP）的细节**不在本文重述**，
统一引用 `research/rk3288-device-capability-survey-2026-09-17.md` 与 `RK3288_CONSTRAINTS.md`。

## 1. 适用范围与证据边界

本文把每一处结论按证据强度分为三档，写作时必须显式标注，不得混列：

| 档位 | 含义 | 允许的表述 |
|---|---|---|
| **实测值** | 有原始命令与输出、可在同条件下复现 | 直接给数字，附采集方式与受控状态 |
| **目标值** | 工程设定的门槛/预算，尚未被真机数据证实 | 写作「目标」「判据」，不得写成「已达成」 |
| **未核实** | 无取证、或仅有假设 | 一律写「未核实/未取证」，不得推算补全 |

- 本文是**基准口径与优化方向**的权威处；`research/appendix-*.md` 等 snapshot 只作**历史出处引用**
  （它们记录的是研究期的设想，不是当前口径），引用时注明「详见 `<路径>`」。
- 设备事实以 **2026-09-18 一轮只读取证**为准，落地结论写在 `RK3288_CONSTRAINTS.md` §0.4 与
  `research/rk3288-device-capability-survey-2026-09-17.md`；不引入该范围之外的设备数字，也不自行推算。
  该范围之外的设备侧数字若被引用，必须注明其仓库出处。（本轮取证的事实集为本机临时件、不入库。）
- 时间窗口一律用**主机时钟**测量（见 §2）。设备时钟不可信。

## 2. 基准前提条件（受控条件）

本节补齐的正是「有数据但口径不成文」的缺口：**没有受控前提，任何数字都不可比较**。

### 2.1 `scaling_governor` 必须为 `performance`（四核全设）

- 依据 ncnn issue #3662：有人在**两块 RK3288** 上实测均达不到文档速度，最终发现需**同时**满足
  `scaling_governor` 设为 `performance`（四核全设）**且供电电压足够**（该案例为 PoE 供电不足）。
- 该 issue 的诊断信号：**「4 线程比 2 线程慢是不正常的」**——出现该现象先查 governor 与供电。
- 相关背景见 `research/opensource-benchmark-2026-09-16.md` 的「RK3288 基准必须在受控条件下测」。

⚠️ **写 governor 是对设备的写操作**：

```bash
# 对设备写；须单独授权后执行，不得在只读采集流程中顺手执行
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo performance > /sys/devices/system/cpu/cpu1/cpufreq/scaling_governor
echo performance > /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor
echo performance > /sys/devices/system/cpu/cpu3/cpufreq/scaling_governor
```

- ⚠️ **未受控（governor 非 `performance`）下测得的值必须显式标注「未受控」，不得与受控值混列。**
- ⚠️ **当前状态（实测）**：两台设备四个核**全部是 `interactive`**。
  → **本文启用之时，任何既有/新增性能数字都属于「未受控」**，直到 governor 被授权改为 `performance` 并复测。
  （`device_id_hash`：机 A `69d83c2ce659`、机 B `f09acaca146d`；实测口径见 `research/rk3288-device-capability-survey-2026-09-17.md` §7.4 与 `RK3288_CONSTRAINTS.md` §0.4。）

### 2.2 供电、温度与负载

- **供电充足**：须记录供电方式（电源规格/是否 PoE/是否电池），并确认不因欠压降频。欠压是 #3662 的并列根因。
- **温度与背景负载受控**：记录环境温度与空载基线；排除同期运行的摄录/上传/OTA 进程。
- **长稳与瞬时分开记录**：soak（≥8h）与瞬时基准（单次 P50/P95）是两类数字，不可混引。

### 2.3 显示上限的前提

- 面板 **`1920×1080 @ 60.000004 fps` 为唯一支持模式**（两台一致，`density 160`，触摸 `INTERNAL`）。
  60fps 是**显示上限**的前提，任何「≥60 FPS」的吞吐目标都无意义。
- 机 B 的 app 可用尺寸为 `1920×1024`（`real` 仍 `1920×1080`），高度被状态栏/导航栏占去 56px（出自 `RK3288_CONSTRAINTS.md` §0.4，非冻结事实集）。

### 2.4 时钟前提

- **设备时钟不可信**：两台分别停在 `2017-01-15 22:40:48 CST` 与 `2017-01-02 00:23:47 CST`，
  **两台彼此相差约 13 天**。用 `logcat` 时间戳比对机号会互相污染 → **采集窗口一律用主机时钟测量**。

## 3. 指标定义与口径

本节是**降级/性能相关日志字段的唯一事实源**。

- **稳态 FPS**：采集窗口内、剔除暖机段后的采样中位数（口径见 §4 的 `run-android-face-regression.ps1`）。
- **TTFF**：从「触发打开摄像头」到「第一帧可见/可分析」的耗时（ms）。
- **P50/P95 延迟**：关键链路（检测/识别/端到端）的百分位延迟，单位 ms。

⚠️ **一个必须写清的易误用点（已核实）**：logcat 侧的 **FPS 来源只有一个**——
`MainActivity` 的 `inferenceAutoTune … ui_fps=`。以下两者**都不是** logcat 侧 FPS 来源：

- `StatusService` 的 `FPS: … | CAP: …` **只 `setText` 到悬浮窗、不进 logcat**；
- `PerfReporter` **也没有 fps 字段**（它输出 p50/p95）。

> 依据：`guides/rk3288-device-acceptance.md` A1「证据」段。统计 FPS 时若去 logcat 里找
> `StatusService` 的 `FPS:` 行会得到空集，那不是「设备没跑」，是口径用错。

## 4. 测量方法与工具

三个**现成**脚本，各自用途与判据：

| 脚本 | 用途 | 关键判据/接缝 |
|---|---|---|
| `../scripts/run-android-face-regression.ps1` | 私有媒体回归（稳态 FPS 判据） | `steady_state`/`fps_criterion`；`-LogcatFile` 离线接缝 |
| `../scripts/bench_camera_adb.ps1` / `.sh` | TTFF 采集 | 要求应用输出 `BENCH_CAMERA …` 标记 |
| `../scripts/stability_switch_50_adb.ps1` | 50 轮前后台切换稳定性 | 切换后仍可恢复、重启计数 |

- `run-android-face-regression.ps1` 是**验收项 A1（≥15 FPS）唯一自动判据的实现者**：`summary.json` 的
  `fps_criterion` 给出 `satisfied` / `not_satisfied` / `insufficient_steady_samples` / `missing_fps_samples` /
  `steady_state_disabled`，**采不到样本或稳态样本不足时显式标注，绝不静默判通过**。它是**采集与聚合**，
  判定与留档仍须人工（场景条件、温度与负载、原始值留档）。
- `bench_camera_adb.ps1` / `.sh` 要求应用稳定输出：`BENCH_CAMERA TTFF_MS=<n>`、
  `BENCH_CAMERA CAPTURE_OK=<n> CAPTURE_FAIL=<n>`、`BENCH_CAMERA CAMERA_SERVICE_RESTART=<n>`。
- 性能留档要求另见 `guides/windows-service/performance.md`。

## 5. 判据阈值表（唯一事实源）

逐条注明来源。**任何引用这些阈值的下游文档都应回到本表。**

| 项 | 阈值 | 出处 |
|---|---|---|
| A1 稳态 FPS | `≥ 15` | `guides/rk3288-device-acceptance.md` A1 |
| A2 人脸框首现 | `p95 ≤ 800ms` | `guides/rk3288-device-acceptance.md` A2 |
| A3 ARM64 高精度检测 | `p95 ≤ 250ms`，否则**必须拒绝激活并显式给出原因** | 同 A3 |
| TTFF | `P50 < 600ms` / `P95 < 900ms` | `research/appendix-c.md:483` |
| 连拍 | `30/30 成功`，内存峰值 `≤ 基线 + 120MB` | `research/appendix-c.md:484` |
| 前后台切换 | 50 次后仍可恢复；`CameraService 重启次数 = 0` | `research/appendix-c.md:485` |
| 内存预算 | `≤ 512MB` | `RK3288_CONSTRAINTS.md` §1.2 |

- **A3 的机制**：常量 `RealtimeBenchmarkConfig.p95BudgetMs=250.0`，`enforceBudget` 仅在 `__aarch64__`
  下为 `true`；非 ARM64 只测量不强判。超预算时管线**必须拒绝激活**并给出 `基准超预算: p95_ms=…`
  或等价 reason，**不得静默启用**该 profile。
- ⚠️ **`≤ 512MB` 内存预算当前没有任何自动断言**——它只在 `RK3288_CONSTRAINTS.md` §1.2 作为建议值存在，
  没有对应的验收脚本或 CI 检查在跑。引用时不得假定它「已被验证」。

### 5.1 P95 降幅门槛的冲突（待统一）

既有文档里 **P95 降幅门槛出现五个互不相同的数字**——四个阶段门槛（10 / 15 / 25 / 30 %）外加一个
Qualcomm 变体（≥ 20 %），来源分散在 snapshot，本条并列呈现并指出冲突：

| 阶段/来源 | 门槛 | 出处 |
|---|---|---|
| 阶段 2（CPU/内存/线程） | `≥ 10%` | `research/appendix-e.md:82` |
| 阶段 3（OpenCL，通用 GPU） | `≥ 15%`（Qualcomm `≥ 20%`） | `research/appendix-e.md:97`、`:144` |
| 阶段 4（Qualcomm 专用） | `≥ 25%` | `research/appendix-e.md:113` |
| 真机验收矩阵（ncnn 推理） | `P95 降幅 ≥ 30%` | `research/appendix-b.md:363` |

**冲突说明**：这五个数字互不相同（阶段门槛 10 / 15 / 25 / 30 %，另有 Qualcomm 变体 ≥ 20 %），
且分别出自不同阶段的 snapshot，没有统一口径。
本文给出**统一口径（待确认）**：**以阶段门槛为主线**——阶段 2 用 `≥10%`，阶段 3 用 `≥15%`；
`appendix-b.md:363` 的 `≥30%` 是 ncnn 推理单点目标，作为**上界参考**而非通用门槛。
**在项目负责人签字确认前，本口径标注「待统一」，引用方须同时引用本表而非单取一处。**

## 6. 降级阶梯的触发条件与验证

`RK3288_CONSTRAINTS.md` §1.6 现有 6 级降级顺序（降检测输入尺寸 416→320→256 / 降检测频率
每 3 帧→每 5 帧→每 10 帧 / 只识别最大或中心人脸 / 关闭关键点与对齐高成本路径 / 关闭识别只留
检测+抓拍 / 进入不可用模式）。**当前它无默认档位、无触发阈值、且没有任何验收项验证它。**
本节补齐这三项。

### 6.1 默认档位

- **默认档 = 满档（不降级）**：启动自检通过且稳态指标在预算内时，一律跑满档。
- 降级档位由运行时控制器按 §6.2 阈值**自动**推进，且**每一级独立可回滚**（关闭后行为回到上一级）。
- 任何降级/升级事件都必须**落日志并标注当前档位字段**，使 A1/A2/A3 的测量能带档位上下文。

### 6.2 触发阈值（保守、可回滚）

以 A1/A2/A3 为唯一触发依据。保守设计：**需连续 N 个窗口越界才降级**，且**需高出阈值带裕量+M 个窗口才升级**（迟滞，防抖动）。

| 触发条件（连续 3 窗口） | 动作（向下一级推进） | 窗口定义 |
|---|---|---|
| 稳态 FPS `< 15` | 降检测输入尺寸 `416 → 320` | 每 30s 采样窗（见 A1） |
| 仍 `< 15` | 降检测输入尺寸 `320 → 256` | 同上 |
| 仍 `< 15` | 降检测频率 `每 3 帧 → 每 5 帧` | 同上 |
| 仍 `< 15` | 降检测频率 `每 5 帧 → 每 10 帧` | 同上 |
| 仍 `< 15` | 只识别最大/中心人脸 | 同上 |
| 仍 `< 15` | 关闭关键点与对齐高成本路径 | 同上 |
| 仍 `< 15` | 关闭识别，仅检测 + 抓拍 | 同上 |
| 自检失败/能力不足 | 进入不可用模式，输出明确原因并阻断算法链路 | 启动自检 |
| 人脸框首现 `p95 > 800ms`（A2） | 优先降检测输入尺寸，再降检测频率 | 见 A2 步骤 |

- **升级（回滚）条件**：连续 3 窗口稳态 FPS `≥ 17`（阈值 +2 FPS 裕量）且 A2 `p95 ≤ 700ms`
  时，逐级回退一格。**回滚不得跳级**，每级单独观察。
- 阈值刻意取保守值（沿用 A1 的 `15`），**宁可多降一级也不要抖动**；所有裕量与窗口数均可配置且在报告中留痕。

### 6.3 验证方式

- **A1**：`run-android-face-regression.ps1` 的 `fps_criterion` 结论行 + 当前档位字段，验证降级后能回升到 `≥15`。
- **A2/A3**：按 `guides/rk3288-device-acceptance.md` 的步骤测；A3 在 ARM64 上必须验证「超预算即拒激活」不变。
- **尚无自动化**：降级控制器本身**当前没有验收项**。补齐前，本节的档位/阈值是**目标口径**，不得写成「已实现」。

## 7. 优化优先级与论证框架

按**设备实测能力**排序，给证据不给感觉。实测能力基线：

- **无 NPU**（设备无 `rknpu*` 节点）。
- **CPU**：四核 Cortex-A17（`armeabi-v7a`）。
- **GPU**：驱动自报名 `Mali-T760`；芯片实际型号 `Mali-T764 MP4`；`OpenGL ES 3.2`（两台逐字一致）。
- **VPU 硬解通路存在且可用**：`/dev/vpu-service`、`/dev/hevc-service` 均 `crw-rw-rw-`（**免 root**）；
  `1080p ≈ 31fps / 720p ≈ 55fps`，解码时 `media.codec` 仅约 1% CPU（这两个速率与 CPU 值出自
  `RK3288_CONSTRAINTS.md` §0.4，非冻结事实集）。
- **RAM ≈ 1.95GiB**（`MemTotal` 2045240 kB / 2045216 kB）。
- **面板 1080p60 唯一模式**（§2.3）。

排序与 Go/No-Go：

- **P0 观测与基准统一** —— 没有可复现基准则「收益」不可判定。先做本文 §2 的受控条件与 §4 的工具接缝。
- **P1 CPU/内存/线程/拷贝消除** —— 设备确为 CPU-bound，收益确定性最高、风险最低；对应 `appendix-e.md` 阶段 2。
- **P1′ 条件性硬解接入** —— VPU 可用且便宜，**但设备侧是旧 `vpu_service` 驱动、不是 MPP 框架**
  （无 `/dev/mpp*`、无 `/proc/mpp_service`），与本项目 `RK_MPP_HOME`（`mpp-1.0.11`）的匹配性
  **从未真机验证**。→ 先做 spike（能否真在设备上打开 `/dev/vpu-service` 解码）再决定；
  **Go/No-Go = 解码 CPU 是否真降且 P95 不退化**。⚠️ **本机能编过 ≠ 设备上能跑**。
- **P2 GPU/OpenCL** —— OpenCL 在目标设备上**是否存在未验证**，只能作可选、`AUTO`、可回滚；
  对应 `appendix-e.md` 阶段 3。
- **No-Go：NPU** —— 项目约束严禁（`RK3288_CONSTRAINTS.md` §1.5），设备亦无节点。

### 7.1 一处必须指出的范围冲突（Qualcomm）

既有路线图（`research/appendix-e.md:44`、`:152`、`:160`、`:170`、`:179`）把设备最小集/测试矩阵/评审材料
定义为「**RK3288 1 台 + Qualcomm 1 台**」。但**本项目当前没有 Qualcomm 目标设备**：

- 能力矩阵 `capability-matrix.md` 的 9 个终端里**无任何 Qualcomm 项**；
- 项目开发目标只有 Android 与 Windows。

→ 这条路线图是按**一个不存在的设备集**写的。**收窄口径**：所有 Qualcomm 相关阶段（阶段 4、
`appendix-e.md:113` 的 `≥25%`、`:144` 的 `≥20%`、评审材料里的 Qualcomm 位）
**标为「当前不适用/条件性」**，不纳入本阶段 Go/No-Go；待有 Qualcomm 目标设备时再解锁。

## 8. 坑清单

每条均可追溯到已核实证据（多为冻结事实集与 `research/rk3288-device-capability-survey-2026-09-17.md`）。

- **设备侧不是 MPP 框架而是旧 `vpu_service` 驱动**；`mpp-1.0.11` 匹配性**未验证**（P1′ 的前置坑）。
- **基准须控 governor 与供电两项**（§2.1、§2.2）；否则数字不可复现。
- **`screenrecord` 看不到走 HWC 的视频层** → **不能用录制帧数反推播放帧率**，须走 `/proc/interrupts`
  中断法（survey §3.4）。
- **`getevent` 输出到管道是块缓冲**（用 `kill` 终止会丢缓冲）→ 采字节要用 `dd` 直读设备节点；
  **`input tap` 注入的事件不经过 evdev** → 阳性对照必须用**真实手指触摸**（survey §4.2）。
- **设备时钟未校准且两台差 13 天**（§2.4）；`/dev`、`/sys` 里大量节点的 mtime 显示 2017
  （内核挂载时刻）**不可作为安装/变更时间**，但 `/data`、`/system/vendor` 下的时间戳**能反映真实写入时间**
  （例如两个指纹相关 APK 是 `2021-08-25`，晚于 ROM 的 `2021-08-05`）。
- **机 B 的 `/data` 只剩 624M / 已用 87%** → 日志/审计落盘规划**不能按机 A 的 1.7G**。
- **`yx_daemon.sh` 的 `check_boot()`**：开机动画 `≥120` 秒即会删 `/data/camera` 与
  `/etc/media_profiles.xml` 并**主动 reboot**（正常路径分支）；异常分支（`/data` 未挂载、只读、
  或 `ro.crypto.state=encrypted`）会 `rm -rf /data/*` + `mke2fs` 重建 userdata。
  **`check_boot()` 是已核实的潜在风险**（开机动画 ≥120 秒会主动 reboot），但 **2026-09-17 那次重启
  已确认是用户手动断电，与之无关**（依据：survey §7.5）。
  - ⚠️ **一处容易被误读的点**：脚本里的 `unit` 判据取的是 `df -h` 第 2 列（**Size 列**）的末字符，
    **不是剩余空间**——两台 Size 均为 `4.5G` 故不触发清空。
    **不要写成「剩余空间小就会清空」。**（`unit=="M"` 只在分区**总容量** < 1G 时才成立。）
- **同一脚本的 `get_yxadb()`**：`find /mnt/media_rw/ -name yx_adb.sh` → `cp` 到 `/system/bin/` → **`exec`**，
  即**插入含该脚本的 U 盘即可让设备以 root 执行它**（ROM 固有设计，非本会话引入）。
- **`grep -a` 看不到 APK 里 deflate 压缩的 `classes.dex`** → 扫字符串必须
  `busybox unzip -p <apk> classes.dex | strings`。
- **不要对 `/sys` 做递归 `grep -r`**（会挂在 sysfs 上不返回）。

## 9. 不做什么

已评估但不采纳的技术方向（ANN 库 / 替换加密库 / 替换视频传输层 / `libfacedetection` /
跨语言重写）——**理由与出处详见 `../AGENTS.md` 的「已评估但不采纳的技术方向」一节，本文不重新论证。**

## 10. 未核实项

以下条目本文**不做结论**，须单列跟踪：

| 项 | 状态 |
|---|---|
| Qualcomm 相关门槛（`appendix-e.md:113/:144` 等） | **当前不适用**（无 Qualcomm 目标设备，见 §7.1） |
| OpenCL 在目标设备是否存在 | **未验证**（只能作可选、`AUTO`、可回滚） |
| `mpp-1.0.11` 与设备驱动（旧 `vpu_service`）的匹配性 | **未验证**（P1′ 的 Go/No-Go 前置） |
| `/data/camera` 与 `/etc/media_profiles.xml` 当前是否存在 | **未取证**（若不存在，说明该机已触发过 `check_boot` 的 `res==0` 分支） |
| P95 降幅统一口径（阶段门槛 10/15/25/30% 及 Qualcomm ≥20%） | **待统一**（见 §5.1，待负责人签字） |
| 降级控制器（§6）的默认档位/阈值 | **目标口径，尚无验收项验证** |
