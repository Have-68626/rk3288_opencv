<!-- DOCSYNC_DOC
type=plan
status=living
as_of=2026-10-20
-->

# 测试套件统筹实施计划

依据[已批准设计](../specs/2026-10-20-test-suite-coordination-design.md)，在codex/test-suite-coordination内联执行TDD，不提交或推送。正式源码不依赖cache历史命令，Windows为本轮运行宿主；不改CI或覆盖率配置。

## 1. 可信报告

文件：scripts/testing/reports.py，tests/scripts/test_suite_reports.py。

- [x] 先写缺失/旧/畸形/零测试/计数矛盾JUnit、嵌套JUnit、跳过和CTest单位回归，运行确认失败。
- [x] 实现read_junit(paths, unit)、报告快照和本次产出校验，汇总保留证据与原退出码。
- [x] 运行 `python -m pytest tests/scripts/test_suite_reports.py -q`；不允许无效证据得到通过状态。

接口：`read_junit([path], unit="case")`返回total/passed/failed/skipped/unit；无效报告抛ValueError。`snapshot(patterns)`记录文件身份；`verify_junit(patterns, before, unit)`拒绝无变化旧件。

## 2. 执行器与所有权

文件：scripts/testing/runner.py，tests/scripts/test_suite_runner.py。

- [x] 用真实受控子进程测试退出7、睡眠超时、Ctrl+C、后代进程、两进程抢锁、含空格路径和日志脱敏，先看到失败。
- [x] 实现Step数据、Windows互斥及受控进程树；子进程在加入所有权边界后才开始执行实际命令。
- [x] 实现30秒心跳、失败/超时/取消传播及异常清理；普通成功也关闭自身后台后代。
- [x] 运行 `python -m pytest tests/scripts/test_suite_runner.py -q`。

接口：`Step(name, command, cwd, timeout=600, junit=(), unit="case", env=None, audit=None)`；`execute_step(step, output, heartbeat=30)`返回status/raw_exit_code/reason/elapsed/counts等结果；`RepoLock(root)`作为非等待上下文管理器。

## 3. 固定目录与CLI

文件：scripts/testing/catalog.py，scripts/run-test-suites.py，tests/scripts/test_suite_catalog.py。

- [x] 先测试7个套件选择/排序/去重、前置缺失、非法超时、只计划不运行、默认失败即停及不选择项。
- [x] 注册docs/tooling/windows/android-mock/android-real/web/web-e2e；所有机器路径来自参数或已有环境。
- [x] 独占创建run目录，写开始/逐步/结束JSON和Markdown；记录源码绑定，保留失败与未运行，输出内容脱敏。
- [x] 执行 `python scripts/run-test-suites.py list`、`plan --suite docs --suite tooling`及对应回归；计划不访问设备或创建执行结果。

## 4. E2E兼容接入

文件：scripts/run-web-e2e.ps1，web/scripts/e2e-run.mjs及原有测试。

- [x] 先用受控harness测试可选目录/工具/SkipInstall/JUnit参数，旧调用保持可用。
- [x] 启动器透传本次Cypress参数，保留心跳、环境隔离和覆盖率归档；不修改cypress.config或vite.config。
- [x] 运行原Node驱动测试及Web启动器单测，验证失败路径无自有作业/进程残留。

## 5. 实际验收与文档

以下为验收判据，运行结果另存报告，不以本计划勾选替代当前证据。

- 至少六组隔离行为变异检出：吞退出码、接受旧报告、接受无效证据、覆盖前一失败、绕互斥、漏清后代；不原地破坏生产源码。
- 通过新入口实际执行docs/tooling、Windows选定目标、Android桩、Web与E2E；真实Android在明确工具链满足时另列运行。
- 修复发现后复验，已有Node/Python/Web单测保持通过；文档门禁H0/M0，敏感扫描与diff检查通过。
- 更新scripts/README、构建指南、使用指南与索引，记录每套件的输入、执行和限制。不自动将本地组合签署为远端全量CI通过。

计划执行状态及具体判定留在cache/reports/test-suite-coordination-2026-10-20，不覆盖此前设计或历史验收。
