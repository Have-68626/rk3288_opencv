<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-18
-->

# 缓存可复用工具并入项目方案

> 用户已批准实施任务1–3，按 `superpowers:executing-plans` 顺序执行；任务4与清理未获批准。不得自动提交或推送。使用方式见[工具指南](../guides/android-validation-tools.md)。

**目标**：从已验证缓存提取可长期维护的构建、静态校验与只读采样工具，排除一次性调试入口。

**方案**：复制并参数化11份已核对的脚本，合并重复采样逻辑；正式工具不依赖日期目录、本机盘符、固定设备地址或旧测试标签。缓存原件保留至正式工具验证和归档完成，不直接移动原件。

**技术栈**：沿用Python标准库、PowerShell、已有ADB/Android SDK/NDK/Java及FFmpegKit构建链；不引入新运行时依赖。WSL仅为Android交叉编译宿主，不新增Linux目标平台或Linux验收要求。

**依据**：[缓存盘点](../audits/archive/2026-10-17-cache-inventory.md)、[实测反馈](../audits/2026-10-17-rk3288-runtime-feedback.md)、[FFmpegKit构建记录](2026-10-16-ffmpeg-kit-build.md)。整理后再执行[独立清理方案](2026-10-17-cache-cleanup.md)，两者可分别审批。

当前操作统一见[验证工具](../guides/android-validation-tools.md)、[FFmpegKit重建工具](../../scripts/ffmpegkit/README.md)和[存储布局](../guides/repository-storage-layout.md)。本页保留迁入取舍、来源映射和未勾选的交付/验收条件；任务5未完成前不能按已迁入推定缓存原件可删。

## 决策与边界

9份既有文档已提交至 `4b39536`，未推送。本方案及其索引更新是后续新增内容，尚未提交。

新扫描52,272个普通文件、3,952.72MiB逻辑大小，跳过364个重解析点、读取错误0。扫描排除本次方案输出目录；逻辑大小不代表物理占用。163项脚本初筛已逐项登记本地 `cache/reuse-cleanup-plan-2026-10-17/tool-decisions.json`，含源文件SHA256和拟定归宿。并非163份都属于本项目维护代码：

| 决策 | 数量 | 处理 |
|---|---:|---|
| 已核对、参数化后可迁入 | 11 | 见下表，实施前复核源摘要 |
| 一次性操作入口 | 11 | 历史保留，不作为正式入口 |
| 第三方工具 | 22 | NDK/Node/下载辅助等，不复制入库 |
| 生成产物 | 20 | Web打包JS及模型转换输出，不作为工具入库 |
| 已有正式同内容副本 | 1 | 不重复维护；当前命中审计门禁测试副本 |
| CI步骤摘录 | 5 | ncnn固定提交构建和精度测试等已在现有CI，不再建平行入口 |
| 历史专项或本机包装 | 93 | 本轮不迁入；并不因此判为可删除，待具体复用需求再审 |

不迁入本次临时Instrumentation控制器、固定设备的 `run-controller.py`、`run-with-firewall.ps1`、`admin-*.ps1`、自动提交脚本、历史checkpoint改写器或一次性变异源码生成器。保留其历史证据，不将本方案变成正式隐藏预览/后台运行功能。

## 迁入映射

原件路径均相对仓库根；映射表示整理目标，不表示原样复制即可投用。

| 缓存原件 | 拟定正式位置 | 必须处理的限制 |
|---|---|---|
| `cache/tools/rk_docsgate.bat` | `scripts/run-docs-checks.ps1` | 去掉固定盘符；保留审计和卫生扫描的真实退出码，不吞失败 |
| `cache/rtmp-lan-2026-10-17/collect-runtime-metrics.py` | `scripts/android/collect-runtime-metrics.py` | ADB、serial、package、采样时长/间隔、输出显式传入；只读、不启动相机或应用 |
| 同目录 `collect-memory-diagnostic.py` | 合并到上行工具的PSS采样 | meminfo正值优先；零值/缺测不得当有效数据，smaps补充标明来源和窗口 |
| 同目录 `summarize-runtime-metrics.py` | `scripts/android/summarize-runtime-metrics.py` | 参数化输入文件清单，保留各窗口，不能把补采写回缺测窗口 |
| 同目录 `summarize-validation.py` | `scripts/android/validate-stream-evidence.py` | 去掉A/B标签与固定案例；纯离线判定，不安装/启动/卸载应用 |
| 同目录 `verify-real-apk.py` | `scripts/android/verify-apk-native.py` | 分离ELF/ZIP校验与本机构建绑定；不自动复制APK、不依赖旧设备备份 |
| 同目录 `verify-final-devices.py` | `scripts/android/verify-device-state.py` | 参数化候选摘要和设备；检查退出、桌面、控制器存在性，不卸载或清数据；去掉硬编码APK长度 |
| `cache/ffmpeg-kit-build-2026-10-16/prepare-native.py` | `scripts/ffmpegkit/prepare-native.py` | 固定受支持来源摘要，检查补丁前提与命中数，不默默改未知源码 |
| 同目录 `run-native.sh` | `scripts/ffmpegkit/run-native.sh` | SDK/NDK/工作目录参数化；保留最小进程环境，不转储环境列表 |
| 同目录 `collect-native.py` | `scripts/ffmpegkit/collect-native.py` | 参数化收集目录，分别核对两ABI能力与依赖，不覆盖既有产物 |
| 同目录 `bundle.py` | `scripts/ffmpegkit/bundle-aar.py` | 参数化readelf、输入和输出；保留ELF机器类型、依赖闭包、TEXTREL和Java类校验 |

`rk_android_gate.bat`、`rk_javatest.bat`、`rk_probe.bat`仅包装已有Gradle命令，且均含固定工作区路径，本轮不为它们再增加入口。现有 `scripts/bench_camera_adb.ps1` 会启动应用/清logcat，不适合作为新只读采样器的隐式前置；保留现有脚本行为，不顺手改造。

## 全局约束与质量要求

- 不改生产应用、已有CI、核心配置、系统环境变量或Git配置；如后续确需修改核心配置，另列补丁、风险和回滚方式报批。
- 不自动扫描并选择设备，不使用固定LAN地址；所有设备操作必须明确serial。无设备参数时只显示用法并退出，不试连历史地址。
- 采样器不执行install/uninstall、am start、force-stop、push/rm、logcat清空、提权、相机启动或防火墙修改。
- 只输出白名单数值、固定错误码和必要哈希。完整异常、环境、命令输出、映射地址/路径、画面和身份数据不落盘、不回传。
- 输出路径已存在时拒绝覆盖；报告区分有效值、缺测、系统返回零、不同PSS来源及不同采样窗口。
- Android与Windows是目标平台。合成资产测试可在Windows离线执行；真实工具链缺失须明确标未验证，不能凭语法检查宣称构建可复现。
- 提交须展示完整信息并再次确认；本方案不授权后续实现自动commit或push。

## 审查重点

| 风险 | 应有行为 | 所属任务 |
|---|---|---|
| meminfo为0、CPU时钟未知、进程中途退出 | 缺测明确，不能填0伪造通过；CPU计算使用已核验实际时钟 | 任务2 |
| 错设备、输出覆盖、ADB失败 | 无默认连接目标，无隐式设备写操作；不泄露原始异常 | 任务2/3 |
| JSON损坏、零帧、短时长、重试接收器错误退出 | 离线判定失败，不能靠报告字段 `passed=true` 通过 | 任务2 |
| 错ABI、缺原生依赖或依赖桩APK | 静态校验拒绝；不能将编译宏或静态校验冒充真实后端生效 | 任务3 |
| 来源漂移、补丁静默失效、手工classes.jar缺来源 | 停止构建并标未验证，不直接使用旧二进制冒充重建 | 任务4 |

## 任务1：文档门禁包装（可独立交付）

文件：新增 `scripts/run-docs-checks.ps1` 与 `tests/scripts/test_docs_check_driver.py`；不改现有Node门禁。

- [x] 先用合成子命令复现“审计失败/卫生通过”“审计通过/卫生失败”及任意工作目录启动，断言包装脚本退出码正确；不调用网络或真实构建。
- [x] 从脚本位置定位仓库根，用参数传报告目录；保持两步均执行、任一步失败整体失败，不增加通用任务框架。
- [x] 执行 `python -m pytest tests/scripts/test_docs_check_driver.py -q`，再真实运行 `pwsh -NoProfile -File scripts/run-docs-checks.ps1`，分别保留两项真实退出码。

## 任务2：只读数值采样与离线汇总（可独立交付）

文件：新增上表3份采样/汇总/流证据脚本，以及 `tests/scripts/test_android_runtime_metrics.py`、`tests/scripts/test_stream_evidence.py`。对外CLI至少明确 `--adb`、`--serial`、`--package`、`--output`；离线工具使用 `--inputs` 和 `--output`，不默认读取cache日期目录。

- [x] 先写合成输入测试：meminfo正值/零值/smaps不可读；32/64位辅助向量有/无AT_CLKTCK；PID变化/退出；拒绝输出覆盖和无serial启动。ADB使用本地替身，断言没有写设备的命令。
- [x] 提取最少必要的解析函数，复杂行为按失败测试→最小实现→通过执行；不建立跨项目采集框架。字宽无法确认时CPU标缺测，不猜100或将32位解析套在64位上。
- [x] 汇总按来源和窗口分别计算数量、min/median/p95/max；A原300秒PSS仍缺测，60秒smaps只能单列。静态报告不记录设备连接地址或原始输出。
- [x] 流证据门禁保留现有时长、帧数、退出、重启事件及清理判据；复现零帧、接收错误退出、短相机和未回桌面四组变异，另测输入损坏，全部拒绝。
- [x] 执行 `python -m pytest tests/scripts/test_android_runtime_metrics.py tests/scripts/test_stream_evidence.py -q`；再用既有匿名本地JSON离线交叉核对数值，不能重新启动相机来“验证”只读工具。

## 任务3：APK与设备状态校验（可独立交付）

文件：新增上表2份校验工具，以及 `tests/scripts/test_apk_native_inspection.py`、`tests/scripts/test_device_state_verification.py`。

- [x] 先用合成ZIP/ELF测试错机器类型、缺失依赖、损坏ZIP、桩核心拒绝，以及构建绑定缺证据时不宣称真实ncnn启用。
- [x] 分离纯静态检查与可选的构建绑定输入；APK签名工具/SDK路径显式解析，不读取本机旧包备份作为隐式信任根。
- [x] 状态检查使用明确候选SHA256；设备缺sha256sum时只在主机内存摘要已装代码包，不保存副本、不要求固定长度；测试ADB异常、非桌面、helper仍在以及只读命令集。
- [x] 执行 `python -m pytest tests/scripts/test_apk_native_inspection.py tests/scripts/test_device_state_verification.py -q`，再离线核对既有真实候选APK。新实机检查另按当时设备操作范围确认，不作为本轮迁入的隐式相机测试。

## 任务4：FFmpegKit构建链（已迁入并完成静态重建）

文件：新增上表4份工具、`scripts/ffmpegkit/build-java.py`、`scripts/ffmpegkit/sources.lock.json`、`scripts/ffmpegkit/README.md`、`tests/scripts/test_ffmpegkit_build_tools.py`。`build-java.py`是必须补齐的缺口，不是现有完整脚本。

- [x] 从现有 `build-inputs.json` 提取有效来源URL、固定提交及摘要，剔除本机镜像绝对路径和未采用的GnuTLS/gmp/nettle/libiconv路线。仅支持本项目已批准的GPL/x264/OpenSSL、API21、ARMv7 NEON与ARM64，不新增变体选择或依赖。
- [x] 对源ZIP、补丁前提、重复运行、错误ABI、缺失Java类和系统桩SO先写失败测试；在独立工作目录实现准备/收集/打包，禁止覆盖原始输入或消费AAR。
- [x] 补齐Java源码输入、smart-exception来源/许可、Android bootclasspath、javac参数和classes.jar重建；现有 `java/javac-android.args` 含本机路径，不能直接当可移植构建接口。
- [ ] 执行 `python -m pytest tests/scripts/test_ffmpegkit_build_tools.py -q`，再在已具备工具链的宿主独立构建两ABI、校验ELF/Java/许可证并组装APK；不替换 `app/libs/ffmpeg-kit.aar`，不在此阶段重做设备推流。
- [x] 重建输出有新的工具链/打包差异时分别登记摘要与等价性，不能承诺ZIP/AAR逐字节相同；证据缺失则保留未验证，原始库和来源不删。

## 任务5：正式交付与缓存原件退出

- [ ] 只归档来源明确的最小JSON（构建输入、源绑定、功能和资源汇总）；除去本机绝对路径、设备地址、历史执行指令和任何敏感字段。拟放 `docs/audits/assets/2026-10-17-rk3288/`，旧失败与窗口缺测不得丢失。
- [ ] 用正式工具离线重算，并核对样本数、判据、输入文件摘要；历史数据若无法用整理后的接口重算，保留旧源绑定并注明局限，不捏造可复现性。
- [ ] 同步README、验收指引和文档索引；代码有变化运行 `graphify update .`，执行文档门禁、相关测试与差异/凭据检查。
- [ ] 展示按可独立交付单元生成的提交信息，用户确认后提交；不自动推送。
- [ ] 只有“正式源码已提交且回读验证”和“唯一二进制/原始证据已有独立备份并核对摘要”均满足，才为对应缓存副本安排新的删除批次。首批可重建清理A另见独立方案，不依赖本任务完成。

## 待确认范围

上一轮仅实施任务1–3。任务4已由[2026-10-18存储整理](2026-10-18-storage-reorganization.md)承接：Java源码重建、双ABI原生构建、26库静态检查与独立候选AAR组装通过，见[执行报告](../audits/archive/2026-10-18-storage-reorganization-result.md)。上方含“组装APK”的复合项仍未勾选：新消费APK集成和设备验收不在本轮范围，不能套用旧APK结论。任务5的正式提交、完整历史证据归档与原件退出仍按各自确认边界执行；旧来源ZIP、镜像和唯一证据继续保留。

## 本轮实现裁定与验证

- 新增最小共享模块 `scripts/android/_tool_support.py`，统一独占输出、固定错误与ADB调用；不建立通用任务框架。收益是各入口使用相同的拒绝覆盖和脱敏规则，后续须把该模块与入口一起维护。
- 设备校验新增显式launcher包参数，按精确包名判桌面，不沿用旧脚本包含launcher的模糊匹配；调用者须提供实际桌面包名。
- 签名校验通过显式Java与apksigner.jar调用，避免将路径拼接进cmd/bat；不会修改JAVA_HOME或隐式选择本机SDK。
- 只在现有codex功能分支工作区实施；默认Git Bash路径不可用时用同等的方案专属记录目录，不安装工具或改系统环境。不提交、不推送，既存pt与缓存原件保留。
- 任务1：5项失败测试确认入口缺失后实现；追加调用方开启原生命令异常模式时的退出码回归，先复现误报再修正，现6项通过。真实文档审计和卫生扫描各返回0。
- 任务2/3：初始28项与16项测试分别因入口缺失失败，再实现并通过。作者追加4条边界回归，先复现退出样本混入、参数错误触发ADB和ET_EXEC误收，再修正。独立复核发现旧PSS零值混入及文档变量冲突，已修复并补5项PSS回归；五个测试文件合计59项通过。
- 独立只读复核已完成，其余范围未发现确定的实质问题。保留零帧、错误退出、短时长、清理不全、错误ABI/依赖/桩/TEXTREL等拒绝用例；不将合成回归或历史证据复判冒充新实机验收。
- 用现有匿名JSON离线重算16项数值与旧汇总完全一致；6份稳定/重启/重试证据通过正式门禁。真实候选APK通过双ABI/AAR代码/原生依赖/签名/构建绑定核对，runtime_verified仍为false，没有重跑设备或精度验收。

反思：缓存中复用价值主要在只读数值处理、静态校验与已固定的构建适配；一次性操作入口和第三方文件数量较多，按后缀批量迁入会引入重复入口及错误默认行为。
