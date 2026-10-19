<!-- DOCSYNC_DOC
type=process
status=living
as_of=2026-09-15
-->

# 文档规范

本文件规定本仓库文档的**元数据、放置位置、单一事实源与引用纪律**。适用于所有被 `git ls-files --cached --others --exclude-standard "*.md"` 枚举到的 `.md` 文件，并由 `scripts/docs-sync-audit.js` 门禁强制。

> 新增文档前请先读完本文件。文档不合规会让 CI 直接阻断。

---

## 1. 元数据块

每份文档（豁免项见 §1.4）**必须**在文件开头包含且仅包含一个 `DOCSYNC_DOC` 块：

```
<!-- DOCSYNC_DOC
type=design
status=living
as_of=2026-09-15
-->
```

### 1.1 键与取值

| 键 | 必需 | 取值（闭集，写错即阻断） |
|---|---|---|
| `type` | 是 | `guide` · `architecture` · `design` · `plan` · `audit` · `research` · `security` · `runbook` · `reference` · `process` |
| `status` | 是 | `living` · `snapshot` · `frozen` · `placeholder` |
| `as_of` | 是 | `YYYY-MM-DD`，该文档所描述内容的**基准日期** |

### 1.2 取值含义

**`type` —— 这份文档是什么**

| 值 | 含义 | 典型位置 |
|---|---|---|
| `guide` | 面向使用者的说明：怎么装、怎么配、怎么跑 | `docs/guides/` |
| `runbook` | 可照做的操作步骤与验收清单 | `docs/guides/` |
| `architecture` | 架构与跨子系统契约 | `docs/architecture/` |
| `design` | 设计：是什么、为什么这样设计 | `docs/designs/` |
| `plan` | 实施计划（时点性） | `docs/plans/` |
| `audit` | 审计/验证报告（时点性） | `docs/audits/` |
| `research` | 调研与分析（时点性） | `docs/research/` |
| `security` | 安全模型、加密、供应链台账 | `docs/security/` |
| `reference` | 事实性参考：约束清单、能力矩阵 | `docs/` 根级 |
| `process` | 流程与规范本身（如本文件） | `docs/` 根级 |

**`status` —— 这份文档现在算什么**

| 值 | 含义 | 门禁行为 |
|---|---|---|
| `living` | **长期有效**，跟着代码演进，内容应保持正确 | 受"滞后"检查：代码变动后长期未更新会进复核清单 |
| `snapshot` | **时点快照**，记录某个日期的事实，不再更新 | 免除"滞后"检查 |
| `frozen` | **已冻结**，明确不再维护，仅作历史参考 | 免除"滞后"检查 |
| `placeholder` | **占位**，内容未填充 | 免除"滞后"检查 |

> **为什么 `status` 重要**：它是"这份文档是否需要跟着代码更新"的**唯一权威声明**。
> 门禁据此决定是否检查滞后——**不再依据文档放在哪个目录**。
> 也就是说，把一份 `living` 文档挪进 `docs/research/` 并不能让它免检；必须如实改成 `snapshot`，而这个改动是会被 review 看到的。

**`as_of`** 是内容基准日，不是文件修改日。改了正文就同步更新；只做排版调整可不改。

### 1.3 块的位置与格式

- 必须**在文件最顶部**（H1 标题之前或紧随其后均可），且在**前 20 行**内。
- 键值对格式为 `key=value`，等号两侧允许空格。
- 不要出现重复键；不要写未定义的键。
- **不要在正文里另外写散文日期行**（`**更新日期**: …`、`日期：…` 等）。历史上仓库用过 9 种写法，是本文件要消除的混乱来源；human 可见的状态信息由 `docs/index.md` 的状态列提供。

### 1.4 豁免

以下文件不受元数据要求约束（它们是第三方内容或工具配置，不是项目文档）：

- `deps/**` —— 第三方依赖的附带说明
- `src/win/third_party/**` —— 第三方源码树
- `.claude/agents/**` —— agent 工具配置

`src/win/third_party/civetweb/LEDGER.md` 例外：它是本仓库自撰的台账，**需要**元数据块（`type=security`）。

---

## 2. 放置规则

文档按**职责**入目录，不按子系统切分：

| 目录 | 放什么 |
|---|---|
| `docs/` 根级 | 索引（`index.md`）、本规范、跨领域的参考（约束清单、能力矩阵）、loop 流程（`safety.md`、`patterns/`） |
| `docs/architecture/` | 架构总览与契约 |
| `docs/designs/` | 设计文档（"是什么/为什么"） |
| `docs/plans/` | 实施计划（时点性，文件名带 `YYYY-MM-DD-` 前缀） |
| `docs/specs/` | 设计规格（时点性，文件名带 `YYYY-MM-DD-` 前缀） |
| `docs/guides/` | 使用与运维说明、验收清单 |
| `docs/audits/` | 审计报告；历史快照进 `archive/` |
| `docs/research/` | 调研与分析附录 |
| `docs/security/` | 安全模型、加密、供应链 |
| `docs/bsp/` | 板级支持包相关 |
| `docs/windows-web-spa/` | Windows 服务 + Web SPA 子系统的设计与接口产物 |

**命名**：除 `docs/plans/`、`docs/specs/`、`docs/audits/` 用 `YYYY-MM-DD-slug.md`（时点性文档，日期前缀让排序即时间线）外，其余用 kebab-case，不加日期前缀。

**位置被工具钉死的例外**（不要移动）：仓库根的 `README.md` / `DEVELOP.md` / `CREDITS.md` / `SECURITY.md` / `CHANGELOG.md` / `AGENTS.md` / `CLAUDE.md`、根级 loop 脚手架（`LOOP.md`、`loop-*.md`、`STATE.md`，被 `.claude/skills/` 按根目录约定读取）、`docs/safety.md`（同上）、`docs/security/enablement-plan.md` 与 `docs/bsp/BSP_EVIDENCE_STATUS.md`（被审计脚本按路径引用）。

---

## 3. 单一事实源

**同一事实只允许一处权威表述，其余一律引用。**

这是本规范里最容易被违反、也最伤人的一条——同一事实写两遍，两边必然漂移，读者不知道该信谁。

正确范式可参考 `docs/security/supply-chain-ledger.md`：它在开头声明"引用而非重复既有账本"，把 ncnn / CivetWeb / Gradle 三处台账指向各自唯一事实源。

**已有过的事故**（写在这里以免重蹈）：

- "5 条架构契约"的状态曾在 4 份文档里有 3 种结论：`README.md` 与 `DEVELOP.md` 说"项目遵循"，`docs/architecture/ARCHITECTURE.md` 自述 5/5 全部待修复，某 specs 文档写"已落地"。读者无从判断。
- 测试框架表在 4 份文档中出现，给出 3 种口径（GTest / 自定义 `bool` / 二者混用），而正确答案是最后一种。

**做法**：权威处写全，其余写一句"详见 `<路径>`"。

---

## 4. 引用纪律

### 4.1 只引用入库内容

`docs/` **不得以稳定路径引用不入库的东西**：`ErrorLog/`、`tests/reports/`、`storage/`、`deps/` 中的生成物。

它们在本机存在，在别人的克隆、CI 和其他机器上不存在——引用即死链。需要长期留存的**结论**请写进 `docs/`，而不是链接原始材料。

### 4.2 引用会被门禁检查

- **`.md` 交叉引用**：目标必须存在且被 git 跟踪，锚点必须真实存在（`xref` 规则，阻断级）。
- **反引号里的仓库路径**：以已知顶层目录开头且带扩展名的路径会被检查存在性（`refpath` 规则，提示级）。

写路径时优先用**相对于引用文件所在目录**的写法（GitHub 语义），不要写 `file:///d:/...` 这类绝对路径——它只对作者本机有效。

### 4.3 相对路径

- 指向仓库内其他文件：用仓库根相对路径（如 `docs/DOC_STANDARD.md`）或相对于当前文件目录的路径，二者由门禁分别解析。
- 指向外部资源：用完整 URL。

---

## 5. 新增文档检查清单

- [ ] 选对目录（§2），文件名符合命名约定
- [ ] 头部加 `DOCSYNC_DOC` 块，`type` / `status` / `as_of` 齐全且取值合法（§1）
- [ ] 内容不与既有文档重复；若确有重叠，确定唯一事实源并把其余改为引用（§3）
- [ ] 交叉引用与路径引用有效（§4）
- [ ] 在 `docs/index.md` 登记（门禁会校验索引完整性）
- [ ] 本地跑一次门禁：`node scripts/docs-sync-audit.js --skip-links`

---

## 6. 门禁如何强制

`scripts/docs-sync-audit.js` 在 CI 的两个 workflow 中运行（`.github/workflows/ci.yml` 的 `docs-audit` job 与 `.github/workflows/docs-audit.yml`）。规则分两档：

**阻断级（high / medium）—— 可证伪的事实错误**

| 规则 | 检查 |
|---|---|
| `agent-guide` | `AGENTS.md` 与 `CLAUDE.md` 的事实块与镜像一致性 |
| `doc-meta` | 每份文档的 `DOCSYNC_DOC` 块存在且取值合法 |
| `doc-index` | 每份文档已登记进 `docs/index.md` |
| `xref` | `.md` 交叉引用的目标与锚点存在 |
| `content` | `README.md` / `DEVELOP.md` / `docs/RK3288_CONSTRAINTS.md` 的必需章节齐全 |
| `consistency` | 特定口径不冲突 |
| `bsp` | 内核配置与策略一致性 |

**提示级（low）—— 启发式信号，进报告不阻断**

| 规则 | 检查 |
|---|---|
| `version` | `status=living` 的文档在代码变动后长期未更新 |
| `refpath` | 反引号引用的仓库路径不存在 |
| `format` | 全角字母数字 / 全角空格 |

**判断原则**：只有**能证伪**的错误才阻断。时间戳、路径存在性这类启发式信号会进复核清单，但不拦人——历史上曾因把时间戳当阻断条件而产生 45 个假阳性（详见 `docs/audits/archive/2026-09-15-documentation-audit.md`）。
