<!-- DOCSYNC_DOC
type=guide
status=living
as_of=2026-11-05
-->

# 本地与 CI 目标映射（B07）

本文件只登记**对应关系与差异**，不复制命令；命令以[构建与测试](build-and-test.md)与[测试统筹](test-suite-coordination.md)、
[构建统筹](build-coordination.md)为准。CI 配置的改动另需报批，本映射不构成修改 CI 的授权。

## 测试 job → 本地入口

| CI job / 检查名 | 门禁边界 | 本地等价入口 | 如实差异 |
|---|---|---|---|
| `changes` / `classify-changes` | 判定 `docs_only`，决定其余 job 是否跳过 | 无 | 本地没有等价物；**不能用本地结果推断 CI 跳过行为** |
| `repo-hygiene` | 卫生扫描、Node 工具回归、量化配对 pytest；纯文档时跳过 | `node scripts/clean-repo-junk.js scan --ci`；`node --test tests/scripts/*.test.js`；`python -m pytest tests/scripts -q`；套件 `tooling` | 本地 pytest 范围更宽，缺工具链的用例会 skip；CI 只在 ubuntu 上跑 |
| `unit-tests` | Linux 核心快通道（不带 OpenCV）+ 3 个 Windows 共享源的 GCC 编译检查；阻断失败 | WSL 快通道命令见[构建与测试](build-and-test.md#linux-wsl) | 只覆盖核心，**不等价**于全量 OpenCV；与 `linux` job 分工不同 |
| `linux` / `linux-build-test` | Ubuntu 24.04 源码构建 OpenCV，构建核心/CLI/测试目标并运行选定 CTest；当前**非必需检查**（稳定后另行授权转门禁） | 套件：无；命令见[构建与测试 Linux 一节](build-and-test.md#linux-wsl) | 本地 WSL 与 runner 同为 GCC 13；本地缺模型资产时对应用例仍 skip |
| `docs-audit` | high=0 且 medium=0；纯文档也运行 | `node scripts/docs-sync-audit.js --skip-links`；套件 `docs` | 判据一致；独立 workflow 另跑审计器单测 |
| `web` / `web-build-test` | pnpm 10.33.0 冻结安装、漏洞审计、lint、Vitest、build、Cypress；E2E 非阻断 | 套件 `web`；E2E 单独用 `web-e2e` | CI 的 E2E 与覆盖率是非阻断步骤；本地 Cypress 二进制可能不可用 |
| `android` / `android-build-test` | 桩 APK、模型禁入、单测、lint；JNI 语法检查非阻断 | 套件 `android-mock`（`android-real` 需另行运行） | **CI 只跑 mock**；真实后端的构建绑定只有本地 `android-real` 覆盖 |
| `windows` / `windows-build-test` | 源码构建 OpenCV 与静态 CRT ncnn、选定目标、CTest、`tests/scripts/` 回归；`rk3288_cli` 与 INT8 步骤非阻断 | 套件 `windows`；构建配置 `windows` / `windows-product` / `core` | 本地 `core` 是 `RK_SKIP_OPENCV` 快通道，**不等价**；INT8 需要模型资产 |

## 构建配置 → 本地入口

| 配置 | 本地入口 | 备注 |
|---|---|---|
| `core` | `python scripts/run-builds.py build --profile core` | 仅 `core_unit_tests`，不能代表 OpenCV/ncnn |
| `windows` | `--profile windows` | 静态 OpenCV + 静态 CRT ncnn + 服务与四个测试目标 |
| `android-mock` | `--profile android-mock` | 与 CI `android` job 同构 |
| `android-real` | `--profile android-real` | CI 不覆盖 |
| `android-cli` | `--profile android-cli` | 不产出 APK，不等价于 Gradle 装配 |
| `web` | `--profile web` | 产物写入本轮独占目录，不覆盖受跟踪的 `src/win/app/webroot/` |
| `windows-product` | `--profile windows-product` | Web→服务→独占装配；不启动相机服务 |

## 判读规则

- **未运行**：报告里该步骤为 `not_run`（上游失败）或根本不在步骤表内；不得读作通过。
- **非阻断失败**：CI 中带 `continue-on-error` 的步骤（`rk3288_cli`、INT8 量化/精度、Web E2E）失败时 job 仍绿，
  必须单独读该步骤日志。`unit-tests` 已恢复阻断；`linux` job 本身阻断，但不是必需检查。
- **缺资产**：`TEST_SKIP_ABSENT` / `TEST_UNVERIFIED` / `GTEST_SKIP` 表示缺少模型、符号链接权限或平台不适用，
  不等于通过，也不等于失败。
- **跳过**：跳过数取本次报告；不要把「跳过」计入覆盖率，也不要把跳过当作回归。

## 维护

新增 CI job 或本地套件/配置时，必须同步本表；`tests/scripts/test_ci_local_mapping.py` 会解析
`.github/workflows/ci.yml` 与 `docs-audit.yml` 的 job 列表，以及 `testing.catalog.SUITES`、
`building.profiles.PROFILES`，检查每个名字都在本文件中出现，缺一即失败。
