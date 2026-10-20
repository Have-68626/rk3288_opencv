<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-10-20
-->

# Claude Code 项目入口

开始任务前完整读取根目录 [AGENTS.md](AGENTS.md)，其为仓库内项目约束、构建边界、证据判读及提交/PR权限的事实源。本文件不再复制整份指南；用户明确指令优先，目录内其他适用指令仍须读取。

<!-- DOCSYNC_AGENT_GUIDE_FACTS
gradle_wrapper=gradle-9.0
android_ci_java=21
windows_ci_events=pull_request,push
windows_ci_workflow_dispatch=false
test_frameworks=custom_bool,googletest
merge_strategy=squash_and_merge
-->
<!-- DOCSYNC_SHARED_GUIDE_START -->
## 文档门禁共享事实

- `gradle_wrapper=gradle-9.0`
- `android_ci_java=21`
- `windows_ci_events=pull_request,push`
- `windows_ci_workflow_dispatch=false`
- `test_frameworks=custom_bool,googletest`
- `merge_strategy=squash_and_merge`

事实块与本共享区间须在 AGENTS.md / CLAUDE.md 同步更新；同时核对真实配置，不以两份文本一致代替事实正确。
<!-- DOCSYNC_SHARED_GUIDE_END -->

## 任务导航

| 工作 | 先读 |
|---|---|
| 确认权限、变更范围与验证标准 | [AGENTS：开始任务](AGENTS.md#开始任务与操作权限) |
| 构建或排查环境 | [构建与测试](docs/guides/build-and-test.md)、[构建约束](AGENTS.md#essential-build-quirks) |
| 模型、依赖或本地资产 | [模型消费者](docs/guides/model-storage-and-consumers.md)、[存储规范](docs/guides/repository-storage-layout.md)、[许可台账](CREDITS.md) |
| 修改文档 | [文档规范](docs/DOC_STANDARD.md)、[文档索引](docs/index.md) |
| 提交或推进 PR | [授权与合并规则](AGENTS.md#提交pr-与合并权限)、[CI 边界](AGENTS.md#ci-pipeline) |

不要从历史记忆直接推定当前代码、分支、设备或测试状态；记忆仅作检索线索，使用前核对当前事实。跨会话记忆入口与 Claude 子目录约定见 [.claude/CLAUDE.md](.claude/CLAUDE.md)，不把个人机器路径或私有记忆内容复制到公共文档。

## graphify

代码库问题按 [AGENTS 的 graphify 规则](AGENTS.md#graphify)先查询本地知识图谱，再读对应源码确认。用户明确调用 graphify 时先读取对应 skill；图谱缺失或过期须说明限制，不将语义缓存当当前事实。AST 更新不代表文档语义已经重建。
