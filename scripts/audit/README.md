<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-10-18
-->

# 审计工具

正式用途、命令与限制见[使用说明](../../docs/guides/audit-tools.md)。三个入口均只使用 Python 标准库，不依赖外部临时目录；历史来源与摘要见 `sources.json`。

工具不会运行构建、改写源代码或更新台账。输出必须是新文件；参数或输入错误返回2，取消返回130。回归：`python -m pytest tests/scripts/test_audit_tools.py -q`。
