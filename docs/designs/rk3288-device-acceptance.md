# RK3288 Android 设备验收清单（人工执行）

**更新日期**: 2026-09-12

**执行状态**: **待真机执行**。本文是手工验收清单，步骤存在不等于场景已通过；无论断结果不得标记为“已通过”。

**适用对象**: RK3288（`armeabi-v7a`）与 ARM64 高精度机型；Android App 主链路。

**自动化边界**: 数据采集可用 `scripts/run-android-face-regression.ps1`（私有媒体回归，仅聚合并匿名化报告）；**判定与留档仍由人工完成**。性能口径亦可参考 `docs/windows-web-spa/performance.md` 的留档要求。

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
- **证据**：`StatusService` 的 FPS 序列或 `PerfReporter` 汇总；记录原始值。

### A2. 人脸框首现 p95 ≤ 800ms

- **步骤**：固定机位，人脸从画面外进入至框首次出现；重复 ≥ 30 次（或用回归脚本驱动），记录每次首现延迟。
- **判据**：**p95 ≤ 800ms**。
- **证据**：逐次延迟原始值 + p50/p95 汇总。

### A3. ARM64 高精度检测 p95 ≤ 250ms，否则必须拒绝并显示原因

- **步骤**：在 ARM64 构建上使用 `arm64_accuracy` profile 激活；观察 `RealtimeFacePipeline` 基准（默认 warmup 10 / timed 50）。
- **判据**：
  - p95 **≤ 250ms** → 允许激活；
  - p95 **> 250ms** → 管线**必须拒绝激活**并显式给出原因（`基准超预算: p95_ms=...` 或等价 reason），**不得静默启用**该 profile。
  - 说明：`RealtimeBenchmarkConfig.p95BudgetMs=250.0`，`enforceBudget` 仅在 `__aarch64__` 下为 `true`；非 ARM64 只测量不强判。
- **证据**：基准输出与最终 `effective`/`reason`。

### A4. 6.25% 面积标准样本稳定检出

- **步骤**：标准样本中，人脸框面积占画面面积约 **6.25%**（约宽高各 25%）；在标准距离/光照下连续测试 ≥ 30 帧。
- **判据**：稳定检出（无持续漏检；允许的抖动需记录比例，不得出现整段丢失）。

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
