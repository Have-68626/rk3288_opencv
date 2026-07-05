# 稳定性治理审计修复计划（对应 README Todo #1）

## 1. 背景与目标

- 计划日期：2026-05-19
- 范围：`README.md` 中“### 1. [P0] 核心稳定性治理 (Stability & Bug Fixes)”
- 审计基线：
  - 代码现状核查（Android 生命周期、Mock 三阶段防护、Engine 异常事件节流）
  - 流水线执行结果（C++/Android/Web/docs-sync-audit）
  - 外部基线资料（Android 生命周期与 Surface 回调、OpenCV 超时属性、文件上传防护实践）

本计划目标是把“已实现但未闭环”和“尚未实现”的项拆分为可执行任务，并在每个任务上定义可验证的完成标准，最终满足 README 的验收口径：

1. 前后台切换 50 次无黑屏
2. Mock 损坏/超规格文件在调用阶段快速拒绝
3. `handleAbnormalEvent` 触发频率降低 90%+

---

## 2. 审计结论摘要（需修复项）

### 2.1 P0 阻断项（必须先修）

1. **C++ 测试闭环中断（链接失败）**
   - `win_unit_tests` 链接失败：`calculateSHA256` 未解析
   - `face_infer_unit_tests` 链接失败：`ModelRegistry::*` 未解析
   - 结果：`ctest` 仅 `core_unit_tests` 可执行，稳定性回归链路不完整

2. **Web E2E 无法执行真实用例**
   - `lint`/`build` 已通过
   - Cypress 在启动阶段失败（`Cypress.exe: bad option: --smoke-test`），导致 E2E 证据链断点

### 2.2 P1 风险项（功能在，但未达验收）

1. **Android 预览恢复机制已落地但未形成自动化验收**
   - 已有 `surfaceCreated/surfaceDestroyed` 重绑逻辑与预览 watchdog
   - 但尚未固化“50 次前后台切换”自动化测试与报告产物

2. **Mock 防护描述与代码现状不一致**
   - 实际已有魔数校验、尺寸/首帧校验、超时属性配置
   - README/分析文档未同步，影响治理决策准确性

3. **`handleAbnormalEvent` 已有节流，但验收指标缺新证据**
   - 已有冷却与限频逻辑
   - 当前“55 次触发”证据源是历史样本，需重采样验证“降低 90%+”

---

## 3. 修复工作包（Work Packages）

## WP-1（P0）：恢复 C++ 测试可执行性

### 目标

恢复 `win_unit_tests` 与 `face_infer_unit_tests` 的可链接/可执行状态，使 `ctest -C Release` 可完整运行。

### 改动项

1. 修复 `win_unit_tests` 的链接源集合
   - 在 `CMakeLists.txt` 的 `win_unit_tests` 目标内补入 `src/cpp/src/FileHash.cpp`（或将其抽为共享库后统一链接）
2. 修复 `face_infer_unit_tests` 的 ModelRegistry 依赖
   - 补齐 `ModelRegistry` 相关实现源文件进入 `face_infer_unit_tests` 目标
   - 若存在特定编译开关导致符号裁剪，显式对测试目标对齐相同编译定义
3. 重新执行：
   - `cmake --build build --config Release --target core_unit_tests win_unit_tests face_infer_unit_tests`
   - `ctest --test-dir build -C Release --output-on-failure`

### 完成标准

- `ctest` 三个目标均可执行，不再出现 “Could not find executable” 与 LNK2019/LNK1120
- 失败若存在，仅允许为断言失败（业务问题），不允许构建/链接失败

---

## WP-2（P0）：稳定性验收自动化（Android 50 次前后台）

<a id="目标-2"></a>
### 目标（2）

把“前后台切换 50 次无黑屏”从人工经验转为可复现脚本和归档证据。

<a id="改动项-2"></a>
### 改动项（2）

1. 新增/扩展 ADB 测试脚本（建议放入 `scripts/`）
   - 自动执行 50 次：`am start` / `HOME` / 回前台
   - 每轮采集关键日志关键词：
     - `previewRecovery`
     - `surfaceCreated/surfaceDestroyed`
     - `首帧推入 ok`
     - `renderStalled`/异常关键字
2. 统一产物目录
   - 输出到 `tests/reports/stability/`（时间戳分目录）
   - 包含：执行摘要、关键计数、原始 logcat
3. 给出明确判定逻辑
   - 黑屏判定：存在“运行中 + 无首帧更新 + 连续恢复失败>=3”计为失败

<a id="完成标准-2"></a>
### 完成标准（2）

- 50 次循环中 0 次黑屏终态
- 报告落盘可追溯（命令、时间戳、设备信息、统计项完整）

---

## WP-3（P0）：Mock 三阶段防护补齐“可测试性”

<a id="目标-3"></a>
### 目标（3）

确保“调用前快速拒绝”不只靠实现，还要有夹具测试覆盖损坏/超规格输入。

<a id="改动项-3"></a>
### 改动项（3）

1. 新增测试夹具目录（建议 `tests/fixtures/mock/`）
   - 损坏头文件（错误魔数）
   - 合法头但不完整文件
   - 超规格大小文件（可通过脚本生成）
2. 新增单测/集成测试：
   - Java 侧验证 `handleMockFileSelection` 快速拒绝路径
   - C++ 侧验证 `VideoManager` 打开失败时状态机转移（`FAILED`）一致性
3. 验证错误路径可观测
   - 日志中包含稳定原因码（建议统一错误标签，如 `MOCK_MAGIC_INVALID`、`MOCK_OVERSIZE`）

<a id="完成标准-3"></a>
### 完成标准（3）

- 新增测试可在 CI 跑通
- “损坏/超规格”在调用阶段拒绝率 100%，且不触发崩溃

---

## WP-4（P1）：`handleAbnormalEvent` 治理闭环

<a id="目标-4"></a>
### 目标（4）

对“触发频率降低 90%+”给出新证据，而非历史日志复用。

<a id="改动项-4"></a>
### 改动项（4）

1. 明确事件分类
   - 区分真正异常 vs 可忽略警告（在调用点增加类型细分或原因码）
2. 增加统计输出
   - 每会话输出事件总数、按类型计数、抑制计数（cooldown drop）
3. 压测采样
   - 在同一输入集上做修复前/后对比，输出到 `docs/analysis/` 新证据文档

<a id="完成标准-4"></a>
### 完成标准（4）

- 对比报告中 `handleAbnormalEvent` 总触发数下降 >=90%
- 抑制与真实异常比例可解释、可复现

---

## WP-5（P1）：文档与现状对齐

<a id="目标-5"></a>
### 目标（5）

清理“已实现但文档仍写缺失”的失真，避免误导后续治理优先级。

<a id="改动项-5"></a>
### 改动项（5）

1. 更新 `README.md` 的 Todo #1 现状描述
   - 将“缺少项”改为“已实现/未验收/待补测试”三态
2. 在 `docs/analysis/` 增补本计划执行记录与结果链接
3. 保持 `docs-sync-audit` 低风险项可控（本次为 low=3，不阻断）

<a id="完成标准-5"></a>
### 完成标准（5）

- README 与代码状态一致
- 审计报告可一跳定位到测试结果与证据路径

---

## 4. 执行顺序与里程碑

### M1（当天）

- 完成 WP-1：修复链接问题，恢复 C++ 回归链路

### M2（+1~2 天）

- 完成 WP-2 + WP-3：把 Android 50 次切换与 Mock 夹具测试固化为可执行脚本/测试

### M3（+2~3 天）

- 完成 WP-4 + WP-5：产出异常事件对比证据并同步文档口径

---

## 5. 验收命令清单（统一口径）

```powershell
# C++ tests
cmake --build build --config Release --target core_unit_tests win_unit_tests face_infer_unit_tests
ctest --test-dir build -C Release --output-on-failure

# Android
.\gradlew.bat -PRK_SKIP_OPENCV=true :app:assembleDebug :app:testDebugUnitTest :app:lintDebug --no-daemon

# Web
pnpm -C web lint
pnpm -C web build
pnpm -C web run e2e:run

# Docs audit
node scripts/docs-sync-audit.js --out-dir tests/reports/docs-sync-audit
```

---

## 6. 当前状态标记（2026-05-19）

- 已验证通过：
  - Android 构建/单测/lint
  - Web lint/build
  - docs-sync-audit（high=0）
- 待修复后复验：
  - `win_unit_tests` 链接失败
  - `face_infer_unit_tests` 链接失败

[← 返回目录](#5-深度研究与专项文档-research-deep-dive)
