<!-- DOCSYNC_DOC
type=audit
status=snapshot
as_of=2026-09-15
-->

# 文档审计报告（全项目）

**审计范围**: 仓库中全部被 git 跟踪的 `.md`（74 份）+ 2 份未跟踪的根级工作文档，共 76 份
**审计工具**: `scripts/docs-sync-audit.js`

---

## 1. 结论摘要

审计触发于门禁连续报红（**45 个 high 缺陷**）。调查后确认：**这些缺陷绝大多数是门禁自身的基准失效导致的假阳性，而非文档质量问题**。

| 指标 | 审计前 | 审计后 |
|---|---|---|
| 门禁退出码 | **2（阻断）** | **0（通过）** |
| high 缺陷 | 45 | **0** |
| medium 缺陷 | 0 | 0 |
| low 缺陷（复核清单） | 0 | 37 |
| 头部时间戳被正确识别的文档 | 3 | **22** |
| `docs-sync-audit` 相关测试数 | 12 | **57** |
| `tests/scripts/` 全量测试数 | 59 | **103** |

**核心判断**：时间戳类检查无法证明文档内容陈旧，不应当阻断门禁；门禁应当只拦截**可证伪的事实错误**（链接失效、必需章节缺失、BSP 配置不一致、口径冲突）。

---

## 2. 缺陷分类

### 2.1 假阳性（45 个中的绝大多数）

**根因**：滞后基准取 `git log master --merges -n 1`，而本项目使用 squash merge。

| 事实 | 数据 |
|---|---|
| master 上的 merge commit 总数 | **6 个，全部来自 `git pull` 同步**，无一 PR 合并 |
| 最近一次 merge | 2026-08-15 |
| 该 merge 之后的提交数 | **46 个，无一 merge** |
| 基准冻结时长 | 31 天 |

由于基准是「上一次手动 `git pull` 的时间」，任何在此之后提交的文档都得到**负的**滞后值（实测 9 份文档为 -23 ~ -32 天），而更早提交的文档则被判定为滞后 8~42 天 —— 这些数字与文档内容是否过时**统计无关**。

**关键反证**：仅把基准换成「最近一次代码变更」会让缺陷数从 **45 升到 60**，且**没有任何一个现有缺陷消失**（15 份此前 `lag=1` 恰好卡在阈值下的文档会越过阈值）。这证明基准必须连同豁免与逐份核对一起改，单独改只会更糟。

**抽样核验**（8 份缺陷文档，逐条比对其事实声明与代码现状）：

| 文档 | 内容准确性 |
|---|---|
| `docs/windows-web-spa/config.md` | 准确（环境变量、`.bak` 轮转、DPAPI 路径、AES-256-GCM 全部核对通过） |
| `docs/windows-web-spa/architecture.md` | 准确 |
| `docs/designs/int8-quantization-design.md` | 准确（6/6 条声明确认，含 CI 传 `--size` 而脚本未定义该参数这一细节） |
| `web/README.md` | 准确（React/Vite/AntD 版本、产物路径、代理目标逐项一致） |
| `docs/RK3288_CONSTRAINTS.md` | 准确 |
| `docs/architecture/cpp-engine.md` | 基本准确，仅一处行号漂移 |
| `docs/plans/2026-07-03-triple-refactor-plan.md` | 计划勾选状态失真（0/55 勾选 vs 交付物已 100% 落地） |
| `README.md` | **存在实质错误**（见 2.2） |

**5/8 内容完全准确，仅 1 份有会被读者误用的错误** —— 这 45 个 high 中绝大多数是假阳性。

### 2.2 真实错误（已修复）

| 文档 | 问题 | 处置 |
|---|---|---|
| `README.md` | Gradle 版本写 `9.0-milestone-1`，实际为 `9.0` | 更正 |
| `README.md` | CI 表写「windows \| push only」，实际为 PR + push（`workflow_dispatch` 时跳过） | 更正为实际触发条件 |
| `README.md` | CI 写「6 个 job」，实际 7 个（含 `changes` 分类 job），且未提及独立 workflow `docs-audit.yml` | 补全为 7 个 job + 第二 workflow |
| `docs/index.md` | 把 `RK3288_CONSTRAINTS.md`、`capability-matrix.md` 列为**根目录**文件，实际位于 `docs/` 下 | 移入「docs/ 根级」小节 |
| `docs/index.md` | 漏收 13 份文档（`safety.md`、`security/*` 5 份、`patterns/*` 2 份、`SECURITY.md`、`CHANGELOG.md`、`AGENTS.md`/`CLAUDE.md`、loop 脚手架等） | 全部补收录 |
| `docs/index.md` | 正文自称「最近更新 2026-08-10」，但其提交时间是 2026-09-12 | 更正，并补机器可读的 `**更新日期**` 字段 |
| `docs/architecture/cpp-engine.md` | `Engine.cpp:559-645` 行号漂移（`Engine::run()` 实际在 636 行） | 移除行号，改结构性描述 |
| `docs/research/appendix-f.md` | 证据目录 `ErrorLog/evidence_20170115_035627/` 已不存在 | 标注路径失效与目录未被跟踪 |
| `docs/research/appendix-g.md` | 引用 `README.md` 的「[P0] 核心稳定性治理」章节，该章节已不存在 | 标注为计划当时的基线 |
| `REMOTE_BRANCH_AUDIT.md` | 称「共 19 个远程分支」，实际 22 个 | 保留原始数字（时点记录）并加归档说明 |

### 2.3 结构性缺口（本轮修复）

| 缺口 | 问题 | 处置 |
|---|---|---|
| 头部时间戳识别 | `extractHeadUpdateTime` 的正则 `[\s]*[:：]` 被 `**` 卡住，**仓库里 15 个文件写的 `**更新日期**: YYYY-MM-DD` 完全不被识别** | 匹配前归一化（剥 `**`、引用符、列表符），并新增 `最后复核` 标签 |
| 时区偏差 | 日期用 `new Date(\`${d}T00:00:00\`)` 按**本地时区**解释，与 git 的 UTC 基准恒偏 8 小时，导致同一文档在 Windows 本地与 ubuntu CI 上判定不一致 | 改用 `Date.UTC` |
| 格式检查形同虚设 | `scanTextConventions` 结果只写进 `formatInfo`，**从不产生缺陷**；且把正常中文全角标点误判为问题（全仓 5086 行） | 拆分计数：仅「全角字母数字 + 全角空格 U+3000」记为 low 缺陷 |
| 死代码 | `const sampleText = ...` 计算后从未使用 | 移除 |
| 豁免匹配越界 | 子串匹配会把 `docs/bsp/kernel-config-notes.md` 连带豁免；裸 `rel.includes("")` 恒真，会让豁免清单静默失效 | 改为段匹配并拒绝空模式 |
| 测试盲区 | `version`/lagDays、全部 `bsp`、`content`、`--fix`、`link`、xref 的 medium 分支**零覆盖**；`docs-audit.yml` 只点名一个测试文件，而 `ci.yml` 的 `repo-hygiene` job 带 `docs_only != 'true'` 条件 → **docs-only PR 上两个测试文件均不执行** | 本轮补齐 lagDays/基准、路径引用、全角分类、BSP 解析与 3% 容差边界、FORBIDDEN 阻断、`content`、`--fix` 四条规则与 `link` 三档严重级别；新增 selfcheck 断言真实仓库门禁为绿；workflow 改用 glob |

---

## 3. 门禁改造

### 3.1 基准语义修正

`getLastMergeTs`（`git log --merges -n 1`）→ `getCodeBaselineTs`（取最近一次触及代码路径的提交：`src/ app/ web/src/ tests/ scripts/ cmake/ .github/ gradle/ config/ CMakeLists.txt`）。

语义从「距上一次 git pull」变为「**代码最近何时变过**」，这才与「文档是否跟上代码」相关。保留旧实现仅供测试对照；`runGit` 可注入，使测试能断言命令中不再出现 `--merges`。

### 3.2 阻断与提示的分离

改造后门禁的职责边界：

**阻断（high / medium）—— 可证伪的事实错误**
- `agent-guide`：`AGENTS.md` 与 `CLAUDE.md` 的事实块漂移、共享区间镜像不一致
- `xref`：`.md` 交叉引用目标缺失
- `content`：README / DEVELOP / RK3288_CONSTRAINTS 必需章节缺失
- `consistency`：README 与 CONSTRAINTS 的架构口径冲突
- `bsp`：defconfig / 内核快照与策略的 MUST / FORBIDDEN 一致性
- `link`：外部链接失效（CI 以 `--skip-links` 关闭）
- `markdownlint`：仅在 `--markdownlint-cli2` 时启用

**提示（low）—— 启发式信号，进报告不阻断**
- `version`：距最近一次代码变更超过 7 天（复核清单）
- `format`：全角字母数字 / 全角空格
- `refpath`：反引号引用的仓库路径不存在

### 3.3 关于新增 `refpath` 检查的调整

原计划将 `refpath`（路径存在性）设为 **high 阻断**。实测后降为 **low**，依据：

该检查在本仓库上**真阳性为零**。命中的全部是合法引用 ——
- `CREDITS.md` 的 `src/paramdict.cpp` 指的是**上游 ncnn 仓库**的同名文件；
- `capability-matrix.md` 用的是 `{engine,camera,...}.cpp` **花括号展开写法**；
- `CHANGELOG.md` 引用的是**变更发生时存在、此后被删除**的文件（变更日志的本职）；
- 计划 / 设计文档引用**尚未创建或已重构掉**的路径。

反引号里的路径本质上是有歧义的，不像 `.md` 链接那样可判定。把它设为阻断条件会重蹈 `version` 时间戳检查的覆辙。因此改为 low，并配套实现：只接受已知顶层目录前缀、排除通配/花括号/占位/绝对路径/裸文件名、时点性文档整体豁免。

### 3.4 时点性文档豁免

新增 `TIMEPOINT_DOC_PATTERNS`，对 `docs/superpowers/`、`docs/research/`、`deps/`、`src/win/third_party/`、`CHANGELOG.md` 与根目录审计快照，不做滞后与路径存在性检查 —— 这些文档按定义就是「某日的快照」，要求它们跟随代码窗口是范畴错误。

---

## 4. loop 脚手架填充

根目录 5 份文件此前是 `loop-init` 生成的**未定制模板**（占位项目名 `My Project` / `YOUR_PROJECT`，run-log 零条记录，55 天无人使用）。本轮填充为项目实际内容：

| 文件 | 处置 |
|---|---|
| `STATE.md` | 项目名改为 `rk3288_opencv`；如实记录「已配置、从未运行」 |
| `LOOP.md` | 补全指向 constraints / budget / run-log 的链接 |
| `loop-budget.md` | 项目名；标注预算数值为脚手架默认值、尚未按实际用量校准 |
| `loop-constraints.md` | **`main` → `master`**；补齐与 `CLAUDE.md` 一致的 PR 合并规则；移除两处死引用 |
| `loop-run-log.md` | 项目名；保留格式说明 |

**核实发现的死引用（已修）**：`loop-constraints.md` 引用的 `loop-ledger.json` 与 `loop-guard` 技能**均不存在**（`@cobusgreyling/loop-cost` 存在，v1.2.0）。

---

## 5. 遗留问题

### 5.1 已处理（2026-09-15 追加）

**原第 3 项 —— 零测试覆盖的规则：已补齐**

新增 10 个用例，覆盖此前完全无测试的三组规则：

| 规则 | 覆盖内容 |
|---|---|
| `content` | `findRequiredSections` 的 anyOf 命中语义；并**锁定其过宽匹配的现状** —— anyOf 含 `"test"` 时 `## Contest` 也会通过「测试命令」检查，避免日后误以为该检查是严格的 |
| `--fix` | 四条规则各一例：围栏补语言（含无法识别时回退 `bash` 的启发式现状）、重复标题追加后缀并插入锚点、裸露 URL 包裹为 `<>`、表格/列表/标题行不折行 |
| `link` | 真实探测依赖网络，无法在测试中稳定复现三档分支，故把映射抽为纯函数 `classifyLinkResult` 并导出；覆盖 `null`（成功）/ `low`（不可校验、缓存失效）/ `medium`（真实 4xx-5xx）三档，以及「low 不阻断、medium 阻断」的门禁语义 |

**原第 4 项 —— markdownlint 是否纳入：决定不纳入**

实测依据（2026-09-15，83 个文件，markdownlint-cli2 默认规则集）：

| 规则 | 违规数 | 性质 |
|---|---|---|
| MD060 表格管道紧凑样式 | 2194 | 风格偏好，与仓库既有表格写法冲突 |
| MD013 行长 80 | 2173 | 风格偏好，中文长行是既有惯例 |
| MD050/032/022/031/058 等其余规则 | 约 1500 | 结构性风格 |
| **合计** | **约 5900** | 启用前需先禁用 28 条规则中的 22 条 |

同时评估了「把脚本内置的四条规则改为报告模式」：实测会新增 362 条 low（MD013 197 / MD024 127），同样以风格偏好为主。

**结论**：不纳入 CI，也**不**启用内置规则的报告模式。项目已有更克制的等价机制 —— `--fix` 按需修复这四条规则。判断依据已写入 `scripts/docs-sync-audit.js` 中 `runMarkdownlintCli2` 的注释，避免后人重复调查；`--markdownlint-cli2` 开关保留供手工排查。

**原第 5 项 —— 根目录审计快照归档：已归档**

6 份历史快照（合计 1580 行）经 `git mv` 移入 `docs/audits/archive/`，git 记录为 rename、历史保留：

`AUDIT_REPORT.md`、`AUDIT_RESULT.md`、`GITHUB_SCAN_AUDIT.md`、`GOVERNANCE_VERIFICATION.md`、`GOVERNANCE_EXECUTION_AUDIT.md`、`REMOTE_BRANCH_AUDIT.md`

同步更新引用：`README.md`（2 处 markdown 链接）、`DEVELOP.md`（1 处）、`docs/index.md`（新增 `audits/archive/` 小节并从「仓库根目录」移出）；`TIMEPOINT_DOC_PATTERNS` 中 6 个已失效的根级条目改由 `docs/audits/` 通配覆盖。仓库根目录的 `.md` 由 19 份降至 13 份，门禁仍为绿。

### 5.2 仍然遗留

1. **被 gitignore 的文档完全不受审计**：`.claude/skills/**`、`.agents/skills/**`（同一内容的**两份副本，均不入库**）、`.remember/**`、`.Jules/**`。
2. **两份未跟踪的大型工作文档**（`FULL_PROJECT_AUDIT_2026-07-22.md` 230 KB、`CLAUDECODE_DEEPSEEKV4FLASH_GOVERNANCE_TASKS_2026-07-22.md` 151 KB）：文件头自述不进入版本控制，但**未被 gitignore**，导致 `git status` 长期有噪音，且因未跟踪而完全绕过门禁（现已加入 `TIMEPOINT_DOC_PATTERNS` 以免产生噪声，但**未解决根本问题**：要么纳入版本控制，要么加 ignore 规则）。
3. **`ErrorLog/README.md` 引用的 `docs/runbooks/rk3288-android-uvc-camera2-camerax-acceptance.md` 不存在**，且 `ErrorLog/` 整体未被 git 跟踪 —— 超出审计范围。

