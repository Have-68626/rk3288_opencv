// 门禁测试：缺失目标文档与 markdownlint 非零退出两类缺陷必须使 docs-sync-audit 门禁失败。
// 治理任务 Q-CQ-B08-007：修复前这两类缺陷被静默漏报（脚本返回 0），本测试断言其退出码非零。
//
// 说明：docs-sync-audit.js 的 repoRoot 固定为脚本所在目录的上级（path.resolve(__dirname, "..")），
// 因此测试把脚本复制进夹具的 scripts/ 子目录，使 __dirname 指向夹具根，从而在隔离目录中运行完整 main()。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const SCRIPT_SRC = path.join(__dirname, "..", "..", "scripts", "docs-sync-audit.js");

// 少数规则无法在无 git 的夹具里端到端验证，改为直接调用导出的纯函数（见文末 doc-index 一组）
const { auditDocIndex } = require("../../scripts/docs-sync-audit.js");

const AGENT_FACTS = [
  "gradle_wrapper=unknown",
  "android_ci_java=unknown",
  "windows_ci_events=unknown",
  "windows_ci_workflow_dispatch=false",
  "test_frameworks=unknown",
  "merge_strategy=squash_and_merge",
].join("\n");

function write(root, relativePath, text) {
  const target = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, text, "utf8");
}

// 文档元数据块（docs/DOC_STANDARD.md §1）。夹具里每份需要守卫的 .md 都必须带，
// 否则 doc-meta 规则会让「干净对照」失败，连带影响所有依赖它的用例。
function docMeta(type, status, asOf = "2026-01-01") {
  return `<!-- DOCSYNC_DOC\ntype=${type}\nstatus=${status}\nas_of=${asOf}\n-->\n\n`;
}

function guide(shared, facts = AGENT_FACTS) {
  return (
    docMeta("process", "living") +
    `<!-- DOCSYNC_AGENT_GUIDE_FACTS\n${facts}\n-->\n<!-- DOCSYNC_SHARED_GUIDE_START -->\n${shared}\n<!-- DOCSYNC_SHARED_GUIDE_END -->\n`
  );
}

// 构造“其余全部通过门禁”的干净夹具：唯一可注入的缺陷是缺失目标 / markdownlint 非零。
// 夹具不含 git 元数据，因此 version-lag、agent-guide-facts 均不会产生高/中危缺陷。
function createCleanFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "docs-sync-audit-gate-"));
  write(root, "scripts/docs-sync-audit.js", fs.readFileSync(SCRIPT_SRC, "utf8"));
  write(root, "AGENTS.md", guide("共享规则"));
  write(root, "CLAUDE.md", guide("共享规则"));

  write(root, "README.md", docMeta("guide", "living") + [
    "# README",
    "",
    "## 项目简介",
    "RK3288 多模态人脸识别系统。",
    "## 快速开始",
    "参考 DEVELOP 环境搭建。",
    "## 依赖列表",
    "OpenCV、ncnn。",
    "## 构建命令",
    "cmake -S . -B build_ci -G Ninja。",
    "## 测试命令",
    "node --test tests/scripts。",
    "## 许可证",
    "Apache-2.0。",
    "",
  ].join("\n"));

  write(root, "DEVELOP.md", docMeta("guide", "living") + [
    "# DEVELOP",
    "",
    "## 环境搭建",
    "安装依赖并设置 OPENCV_ROOT。",
    "## 调试步骤",
    "查看日志排查问题。",
    "## 分支策略",
    "master 为主分支。",
    "## 提交流程",
    "squash and merge。",
    "## 代码规范",
    "遵循 lint 与格式规范。",
    "## 发布流程",
    "构建后发布。",
    "## 内核参考",
    "配置文件：`configs/rockchip_defconfig`。",
    "## 运行态快照",
    "快照：`docs/bsp/kernel-config/kernel_cfg`。",
    "",
  ].join("\n"));

  write(root, "docs/RK3288_CONSTRAINTS.md", docMeta("reference", "living") + [
    "# RK3288 约束文档",
    "",
    "## 硬件限制",
    "内存 512M，CPU Cortex-A17。",
    "## 内核配置",
    "",
    "```",
    "DOCSYNC_KERNEL_CONFIG_POLICY",
    "[MUST]",
    "CONFIG_X",
    "[OPTIONAL]",
    "CONFIG_Y",
    "[FORBIDDEN]",
    "CONFIG_Z",
    "```",
    "",
    "BSP Release Note: docs/bsp/BSP_EVIDENCE_STATUS.md",
    "## 驱动补丁",
    "摄像头驱动补丁。",
    "## 已知缺陷",
    "暂无已知缺陷。",
    "",
  ].join("\n"));

  write(root, "configs/rockchip_defconfig", "CONFIG_X=y\nCONFIG_W=n\n");
  write(root, "docs/bsp/kernel-config/kernel_cfg", "CONFIG_X=y\n");
  write(root, "docs/bsp/BSP_EVIDENCE_STATUS.md", docMeta("reference", "living") + "# BSP Release Notes\nv1.0.0：RK3288 平台初始发布。\n");

  // 索引：doc-index 规则要求每份需要守卫的文档都登记在此。遗漏任一条都会阻断门禁。
  write(root, "docs/index.md", docMeta("reference", "living") + [
    "# 索引",
    "",
    "| 文件 | 说明 |",
    "|---|---|",
    "| `index.md` | 本文件 |",
    "| `RK3288_CONSTRAINTS.md` | 约束 |",
    "| `bsp/BSP_EVIDENCE_STATUS.md` | BSP 证据 |",
    "| `../README.md` | 项目简介 |",
    "| `../DEVELOP.md` | 开发指南 |",
    "| `../AGENTS.md` | 助手指南 |",
    "| `../CLAUDE.md` | 助手指南镜像 |",
    "",
  ].join("\n"));
  return root;
}

// 以夹具为 cwd 运行夹具内的审计脚本，返回 spawnSync 结果。
function runAudit(root, extraArgs = [], envPatch = {}) {
  return spawnSync(process.execPath, [
    path.join(root, "scripts", "docs-sync-audit.js"),
    "--skip-links", "--no-md", "--no-json",
    ...extraArgs,
  ], {
    cwd: root,
    encoding: "utf8",
    env: { ...process.env, ...envPatch },
  });
}

// 在夹具内安装伪造的 npx（避免测试依赖网络），按需返回非零退出码以模拟 markdownlint 检查失败。
function installFakeNpx(root, exitCode) {
  const bin = path.join(root, "fake-bin");
  fs.mkdirSync(bin, { recursive: true });
  fs.writeFileSync(path.join(bin, "npx"),
    "#!/bin/sh\necho 'MD047: file-without-final-newline' 1>&2\nexit " + exitCode + "\n", { mode: 0o755 });
  fs.writeFileSync(path.join(bin, "npx.cmd"),
    "@echo off\necho MD047: file-without-final-newline 1>&2\nexit /b " + exitCode + "\n");
  return bin;
}

function patchedPath(bin) {
  const sep = path.delimiter;
  const cur = process.env.PATH || process.env.Path || "";
  return { PATH: bin + sep + cur, Path: bin + sep + cur };
}

test("门禁控制组：干净夹具应通过门禁（退出码 0）", () => {
  const root = createCleanFixture();
  try {
    const r = runAudit(root);
    assert.equal(r.status, 0, `stdout=${r.stdout}\nstderr=${r.stderr}`);
    assert.equal(JSON.parse(r.stdout).ok, true);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("Q-CQ-B08-007：交叉引用指向缺失目标文档时门禁必须失败（退出码非零）", () => {
  const root = createCleanFixture();
  try {
    // 在干净夹具中注入指向不存在 .md 文件的交叉引用。
    fs.appendFileSync(path.join(root, "README.md"), "## 参考资料\n[缺失附录](docs/research/appendix-b.md)\n", "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `修复前此缺陷被静默跳过（漏报），应返回 0 以外退出码；stdout=${r.stdout}\nstderr=${r.stderr}`);
    const printed = JSON.parse(r.stdout);
    assert.equal(printed.ok, false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("Q-CQ-B08-007：markdownlint-cli2 非零退出时门禁必须失败（退出码非零）", () => {
  const root = createCleanFixture();
  try {
    const bin = installFakeNpx(root, 1);
    const r = runAudit(root, ["--markdownlint-cli2"], patchedPath(bin));
    assert.notEqual(r.status, 0, `修复前 markdownlint 非零被吞掉（漏报），应返回 0 以外退出码；stdout=${r.stdout}\nstderr=${r.stderr}`);
    const printed = JSON.parse(r.stdout);
    assert.equal(printed.ok, false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// 按指定的 MUST 清单重写 BSP 三件套：约束文档策略块、defconfig、运行态内核快照。
// defconfigIds 少于 mustIds 即人为制造 MUST 缺失，用于验证 3% 容差边界。
function writeBspPolicy(root, mustIds, defconfigIds, kernelIds) {
  write(root, "docs/RK3288_CONSTRAINTS.md", docMeta("reference", "living") + [
    "# RK3288 约束文档",
    "",
    "## 硬件限制",
    "内存 512M，CPU Cortex-A17。",
    "## 内核配置",
    "",
    "```",
    "DOCSYNC_KERNEL_CONFIG_POLICY",
    "[MUST]",
    ...mustIds,
    "[OPTIONAL]",
    "CONFIG_Y",
    "[FORBIDDEN]",
    "CONFIG_Z",
    "```",
    "",
    "BSP Release Note: docs/bsp/BSP_EVIDENCE_STATUS.md",
    "## 驱动补丁",
    "摄像头驱动补丁。",
    "## 已知缺陷",
    "暂无已知缺陷。",
    "",
  ].join("\n"));
  write(root, "configs/rockchip_defconfig", `${defconfigIds.map((id) => `${id}=y`).join("\n")}\n`);
  write(root, "docs/bsp/kernel-config/kernel_cfg", `${kernelIds.map((id) => `${id}=y`).join("\n")}\n`);
}

// 容差是严格大于（diffPct > 0.03）：33 项缺 1 为 3.03% 必须阻断，
// 100 项缺 3 恰好 3% 不得阻断。这组边界极易被误改成 >=。
test("BSP 容差边界：MUST 缺失 3.03% 必须阻断", () => {
  const root = createCleanFixture();
  try {
    const must = Array.from({ length: 33 }, (_, i) => `CONFIG_M${i}`);
    writeBspPolicy(root, must, must.slice(1), must);
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `1/33 ≈ 3.03% 超过 3% 阈值，必须阻断；stdout=${r.stdout}\nstderr=${r.stderr}`);
    assert.equal(JSON.parse(r.stdout).ok, false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("BSP 容差边界：MUST 缺失恰好 3% 不得阻断", () => {
  const root = createCleanFixture();
  try {
    const must = Array.from({ length: 100 }, (_, i) => `CONFIG_M${i}`);
    writeBspPolicy(root, must, must.slice(3), must);
    const r = runAudit(root);
    assert.equal(r.status, 0, `3/100 = 恰好 3% 不应阻断；stdout=${r.stdout}\nstderr=${r.stderr}`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("BSP：运行态内核出现 FORBIDDEN 项必须阻断", () => {
  const root = createCleanFixture();
  try {
    fs.appendFileSync(path.join(root, "docs/bsp/kernel-config/kernel_cfg"), "CONFIG_Z=y\n", "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `FORBIDDEN 命中应阻断；stdout=${r.stdout}\nstderr=${r.stderr}`);
    assert.equal(JSON.parse(r.stdout).ok, false);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// ── doc-meta：文档元数据块 ────────────────────────────────────────────────

test("doc-meta：缺少元数据块时门禁必须失败", () => {
  const root = createCleanFixture();
  try {
    const p = path.join(root, "README.md");
    fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace(/<!-- DOCSYNC_DOC[\s\S]*?-->\n\n/, ""), "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `缺少 DOCSYNC_DOC 块应阻断；stdout=${r.stdout}`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("doc-meta：status 取值非法时门禁必须失败", () => {
  const root = createCleanFixture();
  try {
    const p = path.join(root, "README.md");
    fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace("status=living", "status=whatever"), "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `status 不在闭集内应阻断；stdout=${r.stdout}`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("doc-meta：as_of 不是合法日期时门禁必须失败", () => {
  const root = createCleanFixture();
  try {
    const p = path.join(root, "README.md");
    fs.writeFileSync(p, fs.readFileSync(p, "utf8").replace("as_of=2026-01-01", "as_of=2026/01/01"), "utf8");
    const r = runAudit(root);
    assert.notEqual(r.status, 0, `as_of 格式非法应阻断；stdout=${r.stdout}`);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// ── doc-index 的双向差集（用真实索引驱动）─────────────────────────────────
// 注：doc-index 在夹具中不生效——夹具没有 git，门禁枚举不到文件会回退到 3 个根文档，
// docs/index.md 根本不在扫描范围内。故此处直接测 auditDocIndex 的出参。
// 真实仓库的索引完整性由 docs-sync-audit-selfcheck.test.js 覆盖。

test("auditDocIndex 报告漏登的文档（high）与索引死条目（medium）", () => {
  const repoRoot = path.join(__dirname, "..", "..");
  const indexMd = fs.readFileSync(path.join(repoRoot, "docs", "index.md"), "utf8");

  // 真实索引 + 一份未登记的文档 → 应报 high
  const docs = ["README.md", "docs/architecture/ARCHITECTURE.md", "docs/not-registered.md"];
  const d = auditDocIndex(indexMd, repoRoot, docs);
  assert.ok(
    d.some((x) => x.severity === "high" && x.file === "docs/not-registered.md"),
    `未登记的文档应报 high；实际：${JSON.stringify(d)}`
  );
  // 已登记的文档不应被误报
  assert.ok(!d.some((x) => x.file === "docs/architecture/ARCHITECTURE.md"), "已登记文档不得误报");

  // 索引里塞一条指向不存在文件的带路径条目 → 应报 medium
  const d2 = auditDocIndex(`${indexMd}\n| \`missing/nope.md\` | 不存在 |\n`, repoRoot, docs);
  assert.ok(
    d2.some((x) => x.severity === "medium" && /missing\/nope\.md/.test(x.message)),
    `索引死条目应报 medium；实际：${JSON.stringify(d2.filter((x) => x.severity === "medium"))}`
  );
});
