<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-17
-->

# cache分批清理执行方案

> 用户已批准A1、A2并确认使用者全部停止；本轮按原清单完成清理，见[执行记录](../audits/2026-10-17-cache-cleanup-A.md)。B批及其他缓存未获删除授权；不自行扩大批次或推送。

**目标**：优先清理有明确重建依据的临时夹具与编译缓存，保留复用源码、正式证据、原始输入和工具依赖。

**方式**：精确白名单、独立批次、执行前重新验证；不整目录清空cache、不跟随重解析点、不按mtime/名称猜用途。正式工具迁入另见[并入项目方案](2026-10-17-cache-tools-integration.md)，批准整理工具不等于批准清理。

**当前基线**：既有9份文档已提交为 `4b39536`，未推送。新只读扫描52,272个普通文件、3,952.72MiB逻辑大小，跳过364个重解析点、读取错误0，排除本轮方案输出目录；不是物理可回收空间承诺。旧盘点保留为[历史快照](../audits/2026-10-17-cache-inventory.md)。

## 首批A：可直接报批的精确候选

本机完整清单在 `cache/reuse-cleanup-plan-2026-10-17/cleanup-batch-A.json`，SHA256为 `f2350b033cb81b2014fede49a35f3ada5d50ba9ffb7b0425aa5e380c73467e4d`。它记录每个文件的绝对路径、大小、mtime_ns和SHA256；是本地辅助清单，不是Git附件。下表仅在本机有效，换机器须重新盘点，不照抄绝对路径。

共14个目标、3,048个文件、577.56MiB逻辑大小：A1为11文件、550.00MiB；A2为3目录内3,037文件、27.56MiB。清单生成时逐个检查目标及祖先均在仓库cache内且无重解析点；这不能代替删除前的再次核验或无使用者确认。

| 批次 | 仓库相对目标路径 | 普通文件数 | 逻辑MiB |
|---|---|---:|---:|
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_12641947909700/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_1851628905200/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_20330460256300/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_21141942376300/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_21593007858800/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_28982079619000/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_3349385988600/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_37488369814600/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_47777799271200/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_6509662000900/oversize.jpg` | 1 | 50.00 |
| A1 | `cache/review-495-test-temp/rk3288_opencv_mock_preflight_tests/oversize_9627347786300/oversize.jpg` | 1 | 50.00 |
| A2 | `cache/review-495-test-temp/node-compile-cache` | 1783 | 11.87 |
| A2 | `cache/review-495-test-temp/v8-compile-cache` | 6 | 7.29 |
| A2 | `cache/windows-profile/webtmp/node-compile-cache` | 1248 | 8.40 |

### 判据、风险与恢复

| 批次 | 为什么可重建 | 风险 | 恢复方式 |
|---|---|---|---|
| A1 | `tests/cpp/test_mock_preflight_guards.cpp` 的oversize用例seek到50MiB后写零字节；11文件均为50MiB+1字节，是合成无效输入而非照片 | 若相关测试仍运行会影响测试；删除不可恢复历史原件 | 在原有测试环境重跑face_infer_unit_tests的mock_preflight用例会生成新的等价夹具，临时路径可能变化；不需要恢复历史文件名。缺少已配置的OpenCV测试环境时先保留，不为清理临时搭环境 |
| A2 | 目录实际内容为Node/V8编译缓存，已按3处精确子目录枚举 | 首次运行变慢；若其他会话正在使用则不执行 | 使用相同缓存配置的既有Node命令会重新生成；不修改系统环境变量，不承诺逐字节恢复旧缓存 |

如要求可逐字节回退，必须先指定仓库外备份位置并复制/复算摘要；否则本批的回滚口径是“按已有环境重建等价临时产物”。当前没有仓库外备份授权或位置，本轮未写入外部目录。

A1当前文件属性未标为稀疏文件；仍不据此保证释放550MiB，压缩/共享及文件系统统计应在执行后测量。不要删除11个父目录或整个mock_preflight目录，其中还有其他边界测试文件。

## 后续B：目前不具备删除条件

以下只列待评估项，不计入A批授权，也不能把它们的大小加到A批可回收量中。

| 位置 | 旧盘点逻辑大小 | 当前处理与删除前提 |
|---|---:|---|
| `cache/ffmpeg-kit-build-2026-10-16/android-ndk-r25c-linux.zip` | 506.51MiB | 先核对安装的实际版本/摘要、原下载可获得性，以及用户是否接受失去离线安装包；否则保留 |
| `cache/review-495-final/cypress-13.17.0.zip` | 191.32MiB | 已有保留约束，先确认离线恢复需求及解压副本完整性，单独批准后才可安排 |
| `cache/windows-profile/build/` | 142.12MiB | 与运行时回归/变异有关；先归档源码绑定、命令、退出码、判据和必要报告，再确认不再复现相关问题 |
| `cache/review-495-fix-core/build/` | 88.07MiB | 先确认唯一回归证据不在目录内，并有可验证的重建命令，不能只因含build而删除 |
| `cache/builds/` | 新扫描约22.92MiB | 按子项目核对输入/工具依赖，不能整体移走含绝对路径的CMake构建树当备份 |

B表大小是相应快照值，真正报批时逐项重新列清单、大小、恢复命令与摘要。本方案不承诺这些目录都无用。

## 明确保留

- 用户原始输入 `cache/ffmpeg-kit-6.0.LTS.zip`；消费件 `app/libs/ffmpeg-kit.aar` 在扫描范围外，不是缓存清理对象。
- FFmpegKit成品缓存、有效来源/工具清单、原生输出、离线镜像、Java输入、许可证和适配/打包源；在正式链与独立备份未完成前不清理混合目录。
- 整个 `cache/rtmp-lan-2026-10-17/` 暂留：候选/旧APK回退输入、控制器与工具源码快照、失败/成功证据、资源采样和取消标记。临时工具不入库不等于它们的历史证据可以删。
- `cache/audit-fixes-2026-10-13/` 的正式回归、F32来源/转换链、源绑定和变异证据；重建代价与模型许可边界另审，不整个删除。
- `cache/review-495-cypress/13.17.0/`：用户既有保留要求仍有效；本轮RTMP接收FFmpeg也实际位于该Cypress树内，删除会同时破坏接收工具。不得为了清缓存顺手替换工具或下载新依赖。
- 任意重解析点及其目标、来源不明目录、数据库/路径安全测试目录、原始日志与唯一证据先保留。失败日志不是默认垃圾。
- 新方案清单目录、正式Git文档和既存 `pt` 不在A批；`.git`、其他工作树、Web复制缓存、外部SDK/NDK、WSL构建树均不在清理范围。

## 执行步骤与停止条件

- [x] 报告要执行A1、A2还是两者，以及上表风险/恢复方式，取得明确确认。使用者退出也须确认，不能以本会话测试结束代替其他会话无使用者；不擅自终止未知进程。
- [x] 删除前核对当前仓库、分支和Git状态，记录消费AAR摘要与正式文档状态；不暂存 `pt` 或缓存二进制。
- [x] 对每个目标检查解析后的绝对路径位于仓库 `cache/` 内；检查自身和祖先均非重解析点。目录目标枚举出的文件集合必须与已批准清单完全相等。
- [x] 逐文件重新比对大小、mtime和SHA256；发现新增、缺失、修改、链接或访问错误，整批停止，重新生成清单并报差异，不自动接受新文件。
- [x] 按已批准目标用原生PowerShell字面路径操作，文件逐项处理；仅A2三处精确目录允许递归，删除前再检查其路径/祖先。不使用通配符、字符串拼接shell命令或跨shell管道删除。
- [x] 每个目标记录成功/失败；遇到权限或使用中错误即停止，保留余项，不自动提权强删、不扩大清理到父目录。
- [x] 复核只有批准目标消失、AAR摘要未变、Git文档未受损、唯一证据及源码仍在；记录执行前后卷可用空间，但说明其他进程活动会影响差值。
- [x] 需要回归验证时，选择与删除对象对应的原有测试或命令，不为了恢复临时文件重启实机相机、联网构建或重跑全量CI；明确回归可能重新产生同类缓存。
- [x] 将实际删除清单、失败项、空间差值与回归结果写入新的执行记录；执行记录不是自动commit许可。

本轮最小删除实现和12项合成安全测试位于方案专属忽略目录 `.superpowers/sdd/2026-10-17-cache-cleanup/`，不作为正式维护工具迁入。执行前验证越界、重解析点、文件集合/摘要漂移和未批准目标均会拒绝；另以锁文件复现并修正部分失败计数。真实cache未做故障注入。逐个文件删除后只非递归移除空目录，未使用递归强删。

## 可分别确认的动作

1. 实施[工具并入方案](2026-10-17-cache-tools-integration.md)中的任务1–3；任务4的FFmpegKit重建链可另行批准。
2. 执行A1、A2或两者；批准迁入工具不等于批准这些删除，批准A批不等于批准B批。
3. 如希望B批或唯一产物也可清理，先提供仓库外备份路径和离线恢复要求，再制作对应批次。当前不需要为A批强行选择备份位置。

反思：清理收益主要来自明确可重建的合成夹具与编译缓存；大目录虽占空间，却混合工具、输入和证据。用精确清单分批执行，比按名称或日期判“无用”更容易核对和回退。
