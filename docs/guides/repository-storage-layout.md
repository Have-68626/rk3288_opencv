<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-19
-->

# 仓库存储布局

适用于[瘦身报告](../audits/2026-10-17-repository-storage-slimming.md)涉及的构建、工具、模型、依赖和证据。保留工具规定名称、ABI名称及历史证据绑定，不强制一次重命名所有历史目录。

| 内容 | 位置 | 管理方式 |
|---|---|---|
| 正式工具和回归 | `scripts/android/`、`scripts/ffmpegkit/`、`tests/scripts/` | 小文本入库，显式输入输出，不依赖历史cache |
| 独立编译工具源码 | `src/cpp/tools/` | CMake登记并真实重建，入口见scripts/README |
| 公共参数夹具 | `tests/fixtures/opencv/` | 不含私有媒体/身份数据；脚本显式消费 |
| 来源、许可、结论及复现 | `docs/` | 脱敏入库，二进制及完整日志不入库 |
| InsightFace父模型 | `deps/insightface/*.onnx` | 三份已引用父图原位保留，绑定来源与摘要 |
| 上游发行包 | `deps/insightface/archives/` | 五份原包保留，不按单个识别器重复删除整包 |
| 用途未明模型 | `deps/insightface/optional/` | 仅分类，未引用不等于可删除 |
| 转换中间件 | `cache/model-conversion/insightface/{det-10g,w600k-mbf,w600k-r50}/` | 同族成组保留原名与摘要 |
| 固定PNNX包 | `cache/tools/pnnx/20260526/` | 原包与版本身份保留 |
| 离线依赖源码 | `cache/dependency-sources/<依赖>/<提交前缀>/` | 保留Git身份，先确认无现行消费者 |
| 正式模型消费件 | `models/` | 路径、manifest、模板空间及摘要一起维护 |
| FFmpegKit消费AAR | `app/libs/ffmpeg-kit.aar` | Gradle固定入口，本地忽略，不随缓存清理 |
| 新建独立构建 | `cache/builds/<用途-版本或工具链>/` | 已有CMake树含绝对路径，不直接搬移 |
| 跨会话证据 | `cache/reports/<领域或任务>/` | 结论同时入docs，唯一证据不可随意删除 |
| Android/pnpm托管内容 | `app/.cxx`、`app/build`、`web/node_modules` | 保留标准布局，只淘汰确认失效的配置 |
| 当前知识图谱 | `graphify-out/` | 当前图、标签、manifest、反思及语义缓存留址 |
| 历史图谱归档 | `cache/reports/graphify/` | ZIP保留原路径，逐项SHA与CRC验证后退出展开副本 |

旧cache混合目录保留原名以兼容历史证据和盘符绑定；正式工具提交回读、证据保全与消费者迁移完成后，才能逐项淘汰。Cypress原树、副本及离线包仍有保留要求，本轮不去重。外部SDK/NDK/OpenCV直接引用配置位置，不复制整套进项目。

WSL仅作Android交叉编译宿主时，源码和对象可放其原生文件系统的本次临时目录，避免跨盘元数据瓶颈；固定输入、许可、consumer rules、已验证产物及摘要收回项目分类目录后，再回收自身临时目录。不能据此清理其他WSL工具或历史工作区。

移动后的相对路径与SHA见[资产路径映射](../audits/assets/2026-10-18-storage/asset-paths.json)。旧记录仍描述当时事实，按映射寻找现址，不改写旧日志。遗留转换中间件本轮仅保全身份，分类目录不证明其父链已认证或允许商用。

## 删除与恢复

核对用途和消费者，冻结逐文件路径、大小、mtime、SHA与完整集合，拒绝越界、链接、输出冲突和漂移。确认使用者停止后按字面路径逐文件删除，目录仅在检查为空后删除。中途失败记录已删数量并停止，不提权强删绕过占用。

移动可按映射反向操作，前提是目标无冲突且摘要不变。图谱可从已校验ZIP恢复；旧Android仅保留负向验证所需核心库与白名单配置，**不声称完整构建树可逐字节恢复**。

分别统计删除逻辑字节、归档新增字节、移动字节及卷空间读数。Android原生库存在硬链接，路径减少不等于同量物理空间释放。本轮失败重建中间件不计入“清理原仓库”收益。

## 现行入口与消费导航

本地新树由CMake preset和批处理显式-S/-B进入cache/builds；默认工具报告进入cache/reports对应领域。具体命令见[构建与测试](build-and-test.md)，脚本职责见[工具入口](../../scripts/README.md)，父图/转换/消费模型的区别见[模型消费者](model-storage-and-consumers.md)。工具规定的app/build、app/.cxx及web依赖缓存保留默认布局，隔离验收可以显式新建输出树。
