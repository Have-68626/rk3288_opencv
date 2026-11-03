<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-20
-->

# 测试统筹第一批加固实施计划

依据[已批准边界与设计](../specs/2026-10-20-test-suite-hardening-design.md)，当前会话内联执行；不提交、推送或修改核心配置。

## R01：终态

- [x] tests/scripts/test_suite_catalog.py参数化复现KeyboardInterrupt、OSError、RuntimeError边界；断言退出码、步骤终态、后续not_run及原因。
- [x] scripts/run-test-suites.py仅收束未完成running项；异常源码绑定纳入固定诊断，不抹去已完成结果。
- [x] pytest对应测试先失败后通过，原失败传播/抢锁/取消回归保持通过。

## R02：复现元数据

- [x] 新增scripts/testing/metadata.py及tests/scripts/test_suite_metadata.py；先测试命令/目录/输入摘要、环境白名单、脱敏及版本探测失败/取消。
- [x] catalog登记已解析工具；CLI独占归档plan.json及受控版本采集，summary绑定计划摘要。
- [x] 运行真实工具探测，验证版本记录及归档摘要，缺版本不得签署成功。

## R03：正式变异

- [x] 新增scripts/testing/mutations.py、scripts/check-test-suite-mutations.py和固定pytest插件tests/scripts/fixtures/suite_mutation_plugin.py。
- [x] tests/scripts/test_suite_mutations.py覆盖分类负向、基线失败、唯一匹配与源码隔离；未实现时确认失败。
- [x] 正式执行六组变异；外层执行器收回自有后代，收集错误不计检出，原源码摘要不变。

## 交付

以下为固定验收判据，最终执行状态和证据以本轮报告为准，不以勾选替代实际运行。

- 同步使用指南、工具入口和索引；真实执行新入口并回放结构化证据。
- 完整相关Node/Python回归、文档H0/M0、格式与敏感扫描通过；更新graphify AST。
- 保留RED/GREEN/变异结果与异常现场，交付完成范围和剩余限制。
