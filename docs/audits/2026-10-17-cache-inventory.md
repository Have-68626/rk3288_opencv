<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-17
-->

# 本地缓存盘点与保留清单

## 范围与结论

本次只扫描仓库根的 `cache/`，未删除、移动或改写其中的既有产物；未扫描WSL的构建树、外部SDK/NDK、模型目录或Web的node_modules缓存。扫描前HEAD为 `43f0c6d`，Git仅有既存未跟踪项 `pt`。本报告和关联说明为本轮新增修改，不提交、不推送。

扫描计数为52,032个普通文件、约3.75 GiB逻辑大小；跳过364个重解析点，不跟随到目标目录。统计不含本次随后生成的盘点文件，不是实际占用或承诺可回收空间。重解析点均须单独核对，不能通过递归删除扩大到链接目标。

关键问题：旧缓存README把所有内容视为可重建，现已不成立。缓存包含唯一的构建适配脚本、正式验收原始记录和变异证据，不能整目录清理。下列“可删除候选”仅表示内容可重建，执行批量删除仍须检查使用者退出并逐项获得确认。

## 可删除候选

| 仓库根相对位置 | 逻辑大小 | 判据与限制 |
|---|---:|---|
| `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_*/oversize.jpg` | 550 MiB，11文件 | `tests/cpp/test_mock_preflight_guards.cpp` 的超大输入测试通过seek写入50 MiB+1字节生成。属于合成测试夹具，不是照片；确认测试进程退出后可重建。可能为稀疏文件，不能把逻辑大小当作可释放空间 |
| `cache/review-495-test-temp/node-compile-cache/` | 11.87 MiB | Node编译缓存，停止相关进程后可重建 |
| `cache/review-495-test-temp/v8-compile-cache/` | 7.29 MiB | V8编译缓存，同上 |
| `cache/windows-profile/webtmp/` | 8.40 MiB | 本轮抽样最大项均为Node编译缓存；仅其已核对的 `node-compile-cache/` 可进删除批次，目录其余内容先保留 |
| `cache/review-submitted-2026-10-17/temp/` | 随复验变化 | 本轮沙箱临时目录；测试结束后可清理，先保留审查结果 |

不把整个 `review-495-test-temp/` 列为删除项：它还有数据库、路径安全测试和重解析点。其余 `rk_profile_runtime_*` 以及storage相关目录仅凭名称不足以证明无用，保持原位。

## 必须保留与有条件清理

| 位置或类别 | 扫描大小 | 处理结论 |
|---|---:|---|
| `cache/ffmpeg-kit-6.0.LTS.zip` | 11.40 MiB | 用户提供的原始输入，未与远端归档逐字节比对；保留，并建立仓库外备份 |
| `cache/ffmpeg-kit-build-2026-10-16/` | 796.19 MiB | 混合目录，不整体删除。AAR、输入清单、唯一适配/打包脚本及验证记录保留；细分见下节 |
| 其中 `android-ndk-r25c-linux.zip` | 506.51 MiB | 已固定摘要的工具下载，可重新获取；WSL工具安装仍在使用，先核对安装完整且接受未来重新下载成本，再决定是否删除归档 |
| 其中 `native-output/` | 51.19 MiB | 26库及重新打包输入；保留至实机验证与独立备份完成 |
| 其中 `ffmpeg-mirror/`、`mirrors/`、`source/` | 92.35 / 46.09 / 48.75 MiB | 离线源码与镜像，重取成本明显；先保留。未采用依赖须按有效来源清单区分，不能按旧脚本整体重取 |
| `cache/review-495-cypress/13.17.0/` | 602.73 MiB | Cypress解压缓存，已有保留约束，本轮不列入删除批次 |
| `cache/review-495-final/cypress-13.17.0.zip` | 191.32 MiB | 安装归档，可重新下载；先保留，待确认离线恢复需求与解压副本完整性。该目录其余内容是验证日志，不一起删除 |
| `cache/audit-fixes-2026-10-13/` | 1,368.54 MiB | 混有生产链验证、变异、源码副本及构建中间件，正式证据不可整体删除 |
| `cache/windows-profile/build/` | 142.12 MiB | Windows profile验证构建，可重建但CMake绝对路径不宜搬动；保留至相关设备/运行时问题收口 |
| `cache/review-495-fix-core/build/` | 88.07 MiB | 可重建构建目录；先核对正式证据不只存在其中，再决定是否清理 |
| 其他 `builds/`、`review-*`、`runtime-fix-tests/` | 见本地明细 | 按源码、日志、编译产物拆分；不能仅因属于历史轮次或文件名含temp就删除 |
| `cache/web-e2e/` | 约1.97 MiB | 上轮覆盖率计数归档，用于解释跨轮假通过；仅在保留必要对照证据后考虑清理 |
| `cache/tools/` | 很小 | 本机包装脚本；不为节省空间删除 |
| `cache/rtmp-lan-2026-10-17/` | 快照后新增，未计入上述总量 | 本轮真实核心APK、两台覆盖前APK、临时控制器源码和匿名验收结果，先保留。旧APK仅作代码回退输入，不代表数据兼容；APK、原始日志不入Git。临时控制器与驱动验证完成后再选择是否整理为通用脚本，结论入库到局域网验收计划；设备上的临时控制器按每段finally卸载 |

`app/libs/ffmpeg-kit.aar` 不在本次cache扫描范围，是Gradle实际消费件，不是待清理缓存。用途、恢复方法及摘要由[构建记录](../plans/2026-10-16-ffmpeg-kit-build.md)统一说明。Web另有复制的Cypress缓存和原始二进制缓存，均未扫描、未删除。

## 需要入库或独立归档的内容

“本地存在”不等于已入Git。下表列出文本整理目标；原件暂不移动。原始日志、源代码副本和二进制不直接批量加入暂存区。

| 内容 | 原件位置（均为本地缓存） | 建议归宿与处理 |
|---|---|---|
| FFmpegKit用途、消费位置、缺件行为、摘要、许可、恢复步骤 | 本地产物与构建说明 | 已整理至[构建记录](../plans/2026-10-16-ffmpeg-kit-build.md)，本轮补充内容尚待提交确认 |
| 有效来源与工具摘要 | FFmpegKit构建目录的 `build-inputs.json` | 整理为入库来源清单；去除本机镜像绝对路径，保留URL、固定commit、源码及工具摘要。已入库构建记录中的关键摘要继续保留 |
| 适配、原生运行、库收集及AAR打包 | 同目录的 `prepare-native.py`、`run-native.sh`、`collect-native.py`、`bundle.py` | 建议整理至 `scripts/` 专用子目录；现有脚本硬编码本机路径、依赖手工准备的Java输入，不应直接宣称为跨机器一键复现工具。整理并验证后再归档，原件先保留 |
| 最初的依赖获取脚本 | 同目录的 `prefetch.py`、`seed-sources.py` | 含未采用的GnuTLS/gmp/nettle/libiconv路线，不作为最终有效构建入口；保留历史，后续按OpenSSL有效来源重写再入库 |
| Java编译参数、原生库/接口校验、APK校验及集成结果 | 同目录的 `java/`、`artifact-verification.json`、`apk-verification.json`、`android-integration-result.json` | 脱敏后的输入摘要与判据适合小型验收附件；原始日志、classes/JAR及SO作仓库外归档，不把机器路径直接包装成通用脚本 |
| Cypress正式运行、覆盖率、变异及清理证据 | `cypress-main-resume/` 的 `formal-final/`、`formal-expanded-round5/`、`mutations/`、`splash-mutant-round1/`、源绑定及总账 | 已有[整改结论](../plans/2026-10-16-cypress-e2e-remediation.md)；完整日志经脱敏复核后独立归档，建议另入库最小源绑定和汇总结论，不批量提交原始调试输出 |
| Windows、Android、F32等正式回归与变异 | 审计目录的 `resume-2026-10-16/`、`nonweb-followup-2026-10-16/`、`f32-final/`、`integration/` 等 | 保留源绑定、真实退出码和失败变异；从已入库整改总账逐项提取必需证据再归档。日志存在不代表对应平台验收通过 |
| 交接卡、checkpoint及源码快照 | cache根与FFmpegKit构建目录 | 历史HEAD及“未提交”等文字均为当时状态，不按当前指令执行；保留历史快照，后续归档须标注对应commit |

上述脚本整理与完整证据归档尚未实施；不把本报告误认为原件已备份。唯一证据在独立备份校验完成前保持原位。失败日志也有诊断价值，不直接等同临时垃圾。

## 后续删除的执行条件

1. 按选定批次列出绝对路径、逻辑大小与内容用途，检查目标位于仓库cache内，排除重解析点。
2. 确认本次及其他会话没有使用者；停止来源不明的进程不属于清理步骤。
3. 先备份唯一脚本、原始输入及验收证据，复算摘要。
4. 报告风险与恢复方式后取得批量删除确认，再使用原生PowerShell字面路径操作；本报告不提供整目录删除命令。
5. 删除后核对Git状态、消费AAR摘要及必要证据仍在，记录实际释放空间。

本轮本地机器明细位于 `cache/review-submitted-2026-10-17/cache-inventory.json`，它是辅助盘点记录，不是克隆仓库可获得的附件；长期分类结论以本文为准。

反思：先按内容与证据用途分类，再判断重建成本；缓存根的忽略规则不能替代保留策略。此次没有删除或搬移缓存，避免破坏接续构建和既有验收证据。
