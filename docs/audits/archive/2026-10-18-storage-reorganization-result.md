<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-10-18
-->

# 仓库存储整理执行报告

## 范围与结论

落实[瘦身报告](2026-10-17-repository-storage-slimming.md)及[实施计划](../../plans/2026-10-18-storage-reorganization.md)。用户确认范围限于报告涉及内容、使用者已停止；准备合入master，实际提交、合并另行确认，不推送。保留规定目录名、ABI名、上游文件名和历史证据绑定。

原有内容的两批清理已完成，共 **4,228文件、4,767,569,956逻辑字节（约4.44GiB）**。32个模型/工具文件及一棵离线OpenCV源码已归类移动；可维护FFmpegKit重建链已进入正式源码目录。本轮自建失败/重复目录另回收85,661个普通文件、792,751,041逻辑字节及8个文件链接，不计入主清理收益。

## 删除、保全与空间口径

| 批次 | 实际结果 | 逻辑字节 |
|---|---|---:|
| 首批 | 23个已核对的合成oversize夹具、29个Vite缓存文件；共52文件 | 1,223,815,206 |
| 后批 | 旧Android ncnn关闭配置、同配置cxx中间件、旧式cmake输出及20份已归档图谱快照；共4,176文件 | 3,543,754,750 |
| 主清理合计 | 两批均exit0，目标及逐文件集合后验通过 | 4,767,569,956 |

后批只涉及 `app/.cxx/Debug/5m54494f/`、`app/build/intermediates/cxx/Debug/5m54494f/`、`app/build/intermediates/cmake/`，以及 `graphify-out/` 下早于2026-10-16的20个日期目录。当前ncnn开启配置 `3m4t543l`、其他配置及最近两份日期快照保留。

- 旧Android两ABI核心库逐字节保全到 `cache/reports/android-builds/ncnn-off-5m54494f/`；完整目录32,187,601字节。配置仅保存验证器需要的白名单定义，同时记录原配置SHA256，**不能宣称完整旧构建树可逐字节恢复**。归档负向输入会被正式APK构建绑定检查拒绝，当前开启配置仍通过。
- 旧图谱107文件压缩到 `cache/reports/graphify/2026-10-18-history.zip`，14,579,600字节；全部成员SHA256和ZIP CRC逐项通过。成员保留原路径，旁边JSON记录摘要，可按原路径恢复。当前图、标签、manifest、反思和语义缓存保留。
- 首批D卷可用空间读数为232,620,400,640→233,844,383,744字节，后批233,262,415,872→236,281,860,096字节。后台构建、归档及卷内其他活动会影响读数，Android又存在硬链接，**不把读数差或逻辑字节当作同量物理净回收保证**。
- 上述归档和负向输入新增约44.60MiB。工具重建的新增产物另计；移动不计回收；本轮自己生成后回收的失败中间件也不叠加为清理原仓库的收益。

本轮临时副本的首次清理在只读Git对象处中止，已准确记录10,337文件；剩余32,990文件重新冻结摘要、mtime和集合，只处理自建克隆对象只读位后续批。为切换预检实现，另一次精确停止后对账已删11,967文件，再将21,023文件重新核对摘要续批，最终全部回收；中断空回执未当作完成证据。另一棵已停止NTFS构建先核对8个文件符号链接，整批验证链接目标和mtime，移除链接本身后确认目标摘要不变，再冻结42,334普通文件，最终全部回收。未放宽通用清理器的重解析点拒绝，未改主项目Git对象或原始镜像。详细结果见[执行摘要](../assets/2026-10-18-storage/cleanup-results.json)。


清理后八个重点目录复扫无读取错误，按普通文件逻辑字节统计（GiB）：

| 目录 | 原盘点 | 当前 |
|---|---:|---:|
| `app/` | 6.432 | 3.398 |
| `build_fp15_gtest/` | 3.469 | 2.346 |
| `cache/` | 3.297 | 4.311 |
| `deps/` | 2.312 | 1.723 |
| `web/` | 0.832 | 0.815 |
| `graphify-out/` | 0.394 | 0.145 |
| `_deps/` | 0.280 | 0.000 |
| `models/` | 0.249 | 0.249 |

cache增长来自转换中间件、离线源码的归类及已验证重建产物，不是移动回收收益；完整计数见[目录复扫](../assets/2026-10-18-storage/final-directory-sizes.json)。未据八目录合计声称整个卷净释放量。

## 分类与规范存放

| 内容 | 新位置或保留位置 |
|---|---|
| 5个InsightFace发行包 | `deps/insightface/archives/` |
| 24个PNNX/NCNN中间件与生成脚本 | `cache/model-conversion/insightface/{det-10g,w600k-mbf,w600k-r50}/` |
| PNNX固定工具包 | `cache/tools/pnnx/20260526/` |
| 用途未确认的两个模型 | `deps/insightface/optional/`，分类保留 |
| 离线OpenCV源码 | `cache/dependency-sources/opencv/71d3237a093b/` |
| GNU config固定源码 | `cache/dependency-sources/gnu-config/805517123cbf/` |
| 三份父ONNX | 原位 `deps/insightface/` |
| 正式模型消费件 | 原位 `models/`，本轮不去重或改模板空间 |
| 消费AAR | 原位 `app/libs/ffmpeg-kit.aar`，摘要保持 `07dd0dcffbe52023b614e7455694093e9d17003626247f3ac9361b28aaf57eda` |

32文件移动2,258,915,623字节；OpenCV源码7,525文件、301,006,937字节。共2,559,922,560字节的路径重新分类，移动后整批后验通过；OpenCV保留完整Git身份，HEAD仍为 `71d3237a093b60a27601c20e9ee6c3e52154e8b1`，工作区干净。未被当前Android/Windows配置消费的该源码可离线复用；没有直接搬已有CMake构建树。旧空 `_deps/` 仅非递归移除。

逐文件相对路径与摘要见[资产映射](../assets/2026-10-18-storage/asset-paths.json)，父图和消费AAR见[保留输入](../assets/2026-10-18-storage/retained-inputs.json)。三个活树注释已同步det_10g中间件位置。历史日志不改写；遗留中间件按移动前SHA保全，分类不代表父链重新认证或商用许可通过。

存放与恢复规范见[布局说明](../../guides/repository-storage-layout.md)。原始源码ZIP、NDK离线包、Cypress原树/副本/离线ZIP、实机失败及资源缺测证据、未知用途模型、现行模型部署副本和昂贵Windows构建均保留。旧cache混合目录仍承担历史绑定，不因正式脚本已暂存或新工具已创建就整删。

## 可复用工具与真实重建

正式入口位于 `scripts/ffmpegkit/`：`prepare-native.py`、`run-native.sh`、`collect-native.py`、`build-java.py`、`bundle-aar.py`，配套最小共享检查、来源锁、说明和23项回归。复用已迁入的Android ELF/输出校验，显式指定输入与新输出；不引入运行依赖或通用任务框架。

固定主ZIP、FFmpeg/x264/cpu-features/OpenSSL/GNU config五项提交及两份smart-exception源码摘要。真实ZIP补丁前提、Java8类、许可证、两ABI、DT_NEEDED闭包、TEXTREL及系统桩拒绝均检查。Java从42份源码重建为45个major52类。

初次NTFS跨盘构建的Perl停在 `p9_client_rpc`；精确核验脚本、源目录和进程组后停止，保留exit15记录，不当作通过。改用仅本轮创建的WSL临时ext4目录，以相同固定输入在 **264.79秒**完成三个原生变体、双ABI正式收集，exit0后自动回收临时树。WSL是Android交叉编译宿主，不是新增Linux产品目标；既有SDK/NDK和其他WSL工作区未移动。

最终候选：`cache/builds/ffmpegkit-6-lts-validation/ffmpeg-kit-6.0-lts-android-gpl-armv7-arm64.aar`，26,387,708字节，SHA256 **`6b46a2a6718b72cb3a7ebf164033213535dd58d6ff26e57457dc66eca3bf61f1`**。26个库、API21、ARMv7 NEON/ARM64、x264/GPL/OpenSSL和许可静态检查通过，consumer rules与固定输入随验证结果保留。

新旧26个代码段均不同，只登记为新的独立重建候选。没有替换消费AAR、组装新消费APK或做新设备/推流验收，`runtime_verified=false`；旧APK实机结论不能转记到新候选。来源与原生摘要见[重建记录](../assets/2026-10-18-storage/ffmpegkit-rebuild.json)，使用步骤见[工具说明](../../../scripts/ffmpegkit/README.md)。

## 验证、审查与合入准备

- Windows CI既有pytest步骤已注册六份工具回归，八文件合计 **105项通过**；Node **202项通过**。
- 移动/清理安全及规模回归 **24项通过**，junction先确认真实创建。曾因创建链接的非终止错误产生的假阳性已修正；越界、集合/大小/摘要/mtime漂移、占用和部分失败计数继续覆盖。另用2,000合成文件确认预检在20秒预算内完成；仅只读预检并行，实际删除保持顺序复核和准确计数。
- 五组定点变异全部检出：放行TEXTREL、放行系统桩、跳过源码摘要、Bearer脱敏残留、AAR/报告路径重叠。独立只读复核唯一发现为最后一项；先复现留下半成品，再补前置拒绝，完整105项复跑通过。
- 当前真实APK的静态构建绑定仍通过，归档ncnn关闭输入明确拒绝；父模型和消费AAR摘要核验通过。没有执行相机、设备写入或推流。
- 已批准CI补丁和gitignore注释纠偏已应用，忽略匹配规则不变。文档审计/仓库卫生检查均exit0，H0/M0/L10；图谱已按AST更新，少数既有解析警告保留，不将其当成编译或文档语义全量更新。
- 原18项暂存内容的binary diff摘要仍为 `b7ca7fc46e80754fb2b268ef04fdf1239031951242e51a44af823a680113cfbb`。本轮新增内容未暂存、未提交，未切换master、合并或推送。文本高置信度凭据格式扫描未命中；不保证该扫描能识别所有敏感信息。

建议提交：`chore(storage): 迁入构建验证工具并整理本地资产`。候选同时包含既有18项Android验证工具/文档迁入与本轮FFmpegKit、CI、存储说明、相对路径映射及三处注释更新；二进制及完整本机清单不入库。提交须展示内容后获确认；合入master另行确认，并在届时读取PR评论、处理冲突及完成仓库规定的完整CI，采用squash合并。当前相关验证不冒充所有子系统全量CI。本轮专属工作台账和合成测试现场仍在工具规定的 `.superpowers/sdd/2026-10-18-storage-reorganization/`，保留到提交确认及证据归档阶段；没有据此扩展清理其他会话目录。

反思：边界与确认一致，增加的工作是清理本轮失败构建，避免迁入验证反而长期增大仓库。关键问题为跨盘元数据访问和克隆对象只读位；前者改用临时原生文件系统，后者保留部分失败记录、重新冻结续批。后续先按普通文件、链接、只读对象分层预检，再执行大批回收。
