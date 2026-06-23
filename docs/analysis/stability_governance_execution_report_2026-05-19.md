# 稳定性治理分阶段执行报告（2026-05-19）

## 范围
- 依据 `README Todo #1` 与 `docs/analysis/stability_governance_remediation_plan_2026-05-19.md` 分阶段执行。
- 保持 `src/win/app/webroot/*` 隔离，不纳入本轮修复内容。

## Phase 1（WP-1）交付
- 已修复 `win_unit_tests` 链接缺口：补入 `src/cpp/src/FileHash.cpp`。
- 已修复 `face_infer_unit_tests` 链接缺口：补入 `ModelRegistry`/adapters/win 依赖源码，并修正 adapter 头文件包含冲突（显式 `rk_win/...`）。

### 验证
- `cmake --build build --config Release --target core_unit_tests win_unit_tests face_infer_unit_tests`：通过。
- `ctest --test-dir build -C Release --output-on-failure`：通过（3/3）。

## Phase 2（WP-2 + WP-3）交付
- 新增 Android 50 次前后台验收脚本：
  - `scripts/stability_switch_50_adb.ps1`
  - 输出目录：`tests/reports/stability/<timestamp>/`
  - 产物：`device_info.txt`、`cycles.csv`、`logcat_full.txt`、`summary.json`
- 新增 Mock 夹具：
  - `tests/fixtures/mock/corrupt_magic.jpg`
  - `tests/fixtures/mock/incomplete.jpg`
- 新增 C++ 调用前预检测试：
  - `tests/cpp/test_mock_preflight_guards.cpp`
  - 覆盖损坏魔数、不完整文件、超规格文件三类场景。
- 新增统一拒绝原因码通路（调用前预检）：
  - `MOCK_MAGIC_INVALID`
  - `MOCK_FILE_INCOMPLETE`
  - `MOCK_OVERSIZE`
  - 入口：`VideoManager::preflightMockInput(...)`

### 验证
- `build\bin\Release\face_infer_unit_tests.exe`：7/7 通过（含三条 mock 预检测试）。
- `ctest --test-dir build -C Release --output-on-failure`：3/3 通过。

## Phase 3（WP-4 + WP-5）交付
- `handleAbnormalEvent` 已增加会话级统计维度：
  - 总触发计数
  - 分类型处理计数
  - 分类型抑制计数
  - 冷却抑制日志口径（可用于后续前后对比）
- README 已更新三态口径（已实现 / 未验收 / 待补测试）。
- 文档同步审计已执行：
  - `node scripts/docs-sync-audit.js --out-dir tests/reports`
  - 结果：`high=0, medium=0, low=3`
  - 产物：`tests/reports/docs_sync_audit_2026-05-19T21-55-33-6c3f3be2.{json,md}`

## 未完成验收与风险
- Android 真机“50 次前后台切换 0 黑屏”尚未实际执行，仅脚本与报告链路已落地。
- `handleAbnormalEvent` “下降 >=90%”尚未完成同输入集前后对比，需要补实测日志。
