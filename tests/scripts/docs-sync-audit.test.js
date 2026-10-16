const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const {
  auditAgentGuides,
  auditCrossRefs,
  isGatePassed,
  resolveCrossRefPath,
  extractHeadUpdateTime,
  computeLagDays,
  isVersionStale,
  getCodeBaselineTs,
  shouldSkipVersionPath,
  scanTextConventions,
  classifyFullwidth,
  expandRepoPathRef,
  collectRepoPathRefs,
  auditRefPaths,
  parseKernelConfigMap,
  parseKernelConfigPolicyFromConstraints,
  parseConfigItems,
  isPlaceholderText,
  findDefconfigPath,
  findKernelConfigSnapshotPath,
  findRequiredSections,
  sectionKeyHit,
  fixMarkdown,
  classifyLinkResult,
  linkDefectMessage,
  parseDocMeta,
  auditDocMeta,
  analyzeDocMetaMarkers,
  isCalendarDate,
  auditDocMetaExemptList,
  DOC_META_HEAD_LINES,
  requiresDocMeta,
  isLivingDoc,
  DOC_META_EXEMPT_PATTERNS,
  DOC_META_EXEMPT_UNLESS,
  VERSION_LAG_THRESHOLD_DAYS,
} = require("../../scripts/docs-sync-audit.js");

const FACTS = [
  "gradle_wrapper=gradle-9.0",
  "android_ci_java=21",
  "windows_ci_events=pull_request,push",
  "windows_ci_workflow_dispatch=false",
  "test_frameworks=custom_bool,googletest",
  "merge_strategy=squash_and_merge",
].join("\n");

function write(root, relativePath, text) {
  const target = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, text, "utf8");
}

function guide(shared, facts = FACTS) {
  return `<!-- DOCSYNC_AGENT_GUIDE_FACTS\n${facts}\n-->\n<!-- DOCSYNC_SHARED_GUIDE_START -->\n${shared}\n<!-- DOCSYNC_SHARED_GUIDE_END -->\n`;
}

function createFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "docs-sync-audit-"));
  write(root, "gradle/wrapper/gradle-wrapper.properties", "distributionUrl=https\\://services.gradle.org/distributions/gradle-9.0-bin.zip\n");
  write(root, ".github/workflows/ci.yml", "on:\n  pull_request:\n  push:\njobs:\n  android:\n    steps:\n      - with:\n          java-version: '21'\n  windows:\n    if: >\n      github.event_name == 'pull_request' || github.event_name == 'push'\n");
  write(root, "tests/cpp/face_infer_unit_tests_main.cpp", "using TestFn = bool (*)();\nstruct TestCase {};\n");
  write(root, "tests/win/win_unit_tests_main.cpp", "using TestFn = bool (*)();\nstruct TestCase {};\n");
  write(root, "tests/cpp/ncnn_precision_test.cpp", "struct TestCase {};\n");
  write(root, "tests/cpp/core_unit_tests_main.cpp", "#include <gtest/gtest.h>\nint main() { return RUN_ALL_TESTS(); }\n");
  write(root, "CMakeLists.txt", "add_executable(core_gtest_tests test.cpp)\n");
  return root;
}

test("auditAgentGuides 接受与配置一致的镜像", () => {
  const root = createFixture();
  try {
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("共享规则"));
    assert.equal(auditAgentGuides(root).defects.length, 0);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("精度运行器不再使用 TestCase 时仍保留其他真实自定义框架事实", () => {
  const root = createFixture();
  try {
    write(root, "tests/cpp/ncnn_precision_test.cpp", "int main() { return 77; }\n");
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("共享规则"));
    assert.equal(auditAgentGuides(root).defects.length, 0);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("全部自定义运行器缺失时不得仍宣称存在该测试框架", () => {
  const root = createFixture();
  try {
    for (const file of ["tests/cpp/face_infer_unit_tests_main.cpp", "tests/win/win_unit_tests_main.cpp", "tests/cpp/ncnn_precision_test.cpp"]) {
      write(root, file, "int main() { return 0; }\n");
    }
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("共享规则"));
    assert.equal(auditAgentGuides(root).expectedFacts.test_frameworks, "unknown");
    assert.ok(auditAgentGuides(root).defects.some(d => d.rule === "agent_guide_fact_drift"));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("auditAgentGuides 报告事实与共享正文漂移", () => {
  const root = createFixture();
  try {
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("不同规则", FACTS.replace("android_ci_java=21", "android_ci_java=17")));
    const rules = auditAgentGuides(root).defects.map((d) => d.rule);
    assert.ok(rules.includes("agent_guide_fact_drift"));
    assert.ok(rules.includes("agent_guide_mirror_drift"));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("auditAgentGuides 接受事实块等号两侧的空格", () => {
  const root = createFixture();
  try {
    const spacedFacts = FACTS.replace(/=/g, " = ");
    write(root, "AGENTS.md", guide("共享规则", spacedFacts));
    write(root, "CLAUDE.md", guide("共享规则", spacedFacts));
    assert.equal(auditAgentGuides(root).defects.length, 0);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("auditAgentGuides 在事实源缺失时报告漂移而非崩溃", () => {
  const root = createFixture();
  try {
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("共享规则"));
    fs.rmSync(path.join(root, "gradle"), { recursive: true, force: true });
    assert.doesNotThrow(() => auditAgentGuides(root));
    assert.ok(auditAgentGuides(root).defects.some((d) => d.rule === "agent_guide_fact_drift"));
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("auditAgentGuides 容忍 Windows CI 条件的引号与空格格式", () => {
  const root = createFixture();
  try {
    write(root, ".github/workflows/ci.yml", "jobs:\n  android:\n    steps:\n      - with:\n          java-version: '21'\n  windows:\n    if: github.event_name==\"pull_request\" || github.event_name == \"push\"\n");
    write(root, "AGENTS.md", guide("共享规则"));
    write(root, "CLAUDE.md", guide("共享规则"));
    assert.equal(auditAgentGuides(root).expectedFacts.windows_ci_events, "pull_request,push");
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("isGatePassed 仅在高、中优先级均为零时通过", () => {
  assert.equal(isGatePassed({ high: 0, medium: 0, low: 0 }), true);
  assert.equal(isGatePassed({ high: 1, medium: 0, low: 0 }), false);
  assert.equal(isGatePassed({ high: 0, medium: 1, low: 0 }), false);
  assert.equal(isGatePassed({ high: 0, medium: 0, low: 1 }), true);
});

test("resolveCrossRefPath 按引用源目录解析 Markdown 路径", () => {
  assert.equal(resolveCrossRefPath("docs/research/appendix-b.md", "../../DEVELOP.md"), "DEVELOP.md");
  assert.equal(resolveCrossRefPath("docs/research/appendix-e.md", "appendix-c.md"), "docs/research/appendix-c.md");
  // docs/ 前缀链接按 GitHub 语义相对解析：docs/research/appendix-e.md 内的
  // docs/RK3288_CONSTRAINTS.md 实际指向 docs/research/docs/RK3288_CONSTRAINTS.md
  // （若不存在即为失效链接，不再一律回落到仓库根 docs/RK3288_CONSTRAINTS.md）
  assert.equal(resolveCrossRefPath("docs/research/appendix-e.md", "docs/RK3288_CONSTRAINTS.md"), "docs/research/docs/RK3288_CONSTRAINTS.md");
});

test("resolveCrossRefPath 对 docs/ 前缀链接按引用源目录相对解析且根级文件兼容", () => {
  // 根级引用文件（dirname 为 "."）链接 docs/ 前缀等价于仓库根解析，兼容旧行为
  assert.equal(resolveCrossRefPath("README.md", "docs/RK3288_CONSTRAINTS.md"), "docs/RK3288_CONSTRAINTS.md");
  // 非根级引用文件链接 docs/ 前缀按 GitHub 语义相对解析（此前错误地回落到仓库根）
  assert.equal(resolveCrossRefPath("docs/foo/bar.md", "docs/other.md"), "docs/foo/docs/other.md");
});

test("xref 门禁对 docs/ 前缀相对链接缺失目标报 high", () => {
  // 场景：根级 docs/other.md 存在，但引用文件 docs/foo/bar.md 所在目录相对解析的
  // docs/foo/docs/other.md 不存在（GitHub 渲染为 404）。修复前门禁查根级存在即放行（漏报）。
  const crossRefs = [
    { fromFile: "docs/foo/bar.md", toFile: "docs/other.md", anchor: null, raw: "docs/other.md" },
  ];
  const anchorMap = {
    "docs/other.md": new Set(), // 根级存在
    "docs/foo/bar.md": new Set(),
  };
  const defects = auditCrossRefs(crossRefs, anchorMap);
  assert.ok(
    defects.some((d) => d.type === "xref" && d.severity === "high" && d.message.includes("docs/foo/docs/other.md")),
    "应报 high（xref 目标缺失：docs/foo/docs/other.md）"
  );
});

// ── extractHeadUpdateTime：头部更新时间解析 ────────────────────────────────
// 修复前 `[\s]*[:：]` 会被 `**` 卡住，而仓库里 15 个文件写的正是 `**更新日期**: YYYY-MM-DD`，
// 导致它们完全不携带头部时间戳、只能回退到 git 提交时间。

test("extractHeadUpdateTime 识别加粗与 ASCII 冒号写法", () => {
  assert.equal(extractHeadUpdateTime("**更新日期**: 2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
  assert.equal(extractHeadUpdateTime("**更新日期**：2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
});

test("extractHeadUpdateTime 识别最后复核标签与引用/列表前缀", () => {
  assert.equal(extractHeadUpdateTime("**最后复核**: 2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
  assert.equal(extractHeadUpdateTime("> **最后复核**: 2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
  assert.equal(extractHeadUpdateTime("- 最后复核：2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
});

test("extractHeadUpdateTime 保留原有标签写法", () => {
  for (const label of ["最后更新", "更新时间", "更新日期"]) {
    assert.equal(extractHeadUpdateTime(`${label}：2026-07-21\n`), Date.UTC(2026, 6, 21) / 1000, label);
  }
  assert.equal(extractHeadUpdateTime("Last updated: 2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
});

test("extractHeadUpdateTime 按 UTC 解释日期", () => {
  // 修复前用 `new Date(\`${d}T00:00:00\`)` 按本地时区解释，与 git 的 UTC 基准恒偏 8 小时，
  // 使同一文档在 Windows 本地与 ubuntu CI 上判定不一致。
  assert.equal(extractHeadUpdateTime("更新时间：2026-07-21\n"), Date.UTC(2026, 6, 21) / 1000);
});

test("extractHeadUpdateTime 只扫前 60 行，无日期时返回 null", () => {
  const body = Array.from({ length: 60 }, (_, i) => `第 ${i + 1} 行`).join("\n");
  assert.equal(extractHeadUpdateTime(`${body}\n**更新日期**: 2026-07-21\n`), null);
  assert.equal(extractHeadUpdateTime("# 标题\n\n正文\n"), null);
});

// ── 滞后判定：基准、阈值边界 ──────────────────────────────────────────────

test("computeLagDays 对缺失基准或时间戳返回 null 而非 NaN", () => {
  assert.equal(computeLagDays(null, 1000), null);
  assert.equal(computeLagDays(1000, null), null);
  assert.equal(computeLagDays(0, 1000), null);
});

test("isVersionStale 阈值当天不判滞后、第 8 天判滞后", () => {
  const base = 1800000000;
  assert.equal(computeLagDays(base, base - VERSION_LAG_THRESHOLD_DAYS * 86400), VERSION_LAG_THRESHOLD_DAYS);
  assert.equal(isVersionStale(VERSION_LAG_THRESHOLD_DAYS), false);
  assert.equal(isVersionStale(VERSION_LAG_THRESHOLD_DAYS + 1), true);
});

test("isVersionStale 对文档比基准新的负值不判滞后", () => {
  const base = 1800000000;
  assert.equal(computeLagDays(base, base + 86400), -1);
  assert.equal(isVersionStale(-1), false);
  assert.equal(isVersionStale(null), false);
});

// 回归点：本项目用 squash merge，`git log --merges` 的基准会长期冻结在偶发的 git pull 上，
// 2026-09 曾因此产生 45 个假阳性 high 缺陷。基准必须取「最近一次代码变更」。
test("getCodeBaselineTs 不使用 --merges 且限定在代码路径上", () => {
  const calls = [];
  const runGit = (cmd) => {
    calls.push(cmd);
    return "1700000000";
  };
  assert.equal(getCodeBaselineTs("master", runGit), 1700000000);
  assert.equal(calls.length, 1);
  assert.ok(!/--merges/.test(calls[0]), `基准命令不得使用 --merges：${calls[0]}`);
  assert.match(calls[0], /git log master -n 1 --format=%ct -- /);
  assert.match(calls[0], /"src\/"/);
  assert.match(calls[0], /"\.github\/"/);
});

test("getCodeBaselineTs 在代码路径取不到时回退到全仓最后一次提交", () => {
  const calls = [];
  const runGit = () => {
    calls.push(1);
    return calls.length === 1 ? "" : "1699999999";
  };
  assert.equal(getCodeBaselineTs("master", runGit), 1699999999);
  assert.equal(calls.length, 2);
});

test("getCodeBaselineTs 在两路都取不到时返回 null", () => {
  assert.equal(getCodeBaselineTs("master", () => ""), null);
});

// ── shouldSkipVersionPath：豁免路径匹配 ──────────────────────────────────

test("shouldSkipVersionPath 归一化末尾斜杠与反斜杠", () => {
  assert.equal(shouldSkipVersionPath("docs/bsp/kernel-config/x.md", ["docs/bsp/kernel-config/"]), true);
  assert.equal(shouldSkipVersionPath("docs/bsp/kernel-config/x.md", ["docs\\bsp\\kernel-config\\"]), true);
});

test("shouldSkipVersionPath 空模式不得豁免一切", () => {
  // 裸 `rel.includes("")` 恒真，会让豁免清单静默失效、门禁失去意义
  assert.equal(shouldSkipVersionPath("README.md", [""]), false);
  assert.equal(shouldSkipVersionPath("README.md", []), false);
});

test("shouldSkipVersionPath 按段匹配，不跨越目录边界", () => {
  // 子串匹配会把 docs/bsp/kernel-config-notes.md 一并豁免
  assert.equal(shouldSkipVersionPath("docs/bsp/kernel-config-notes.md", ["docs/bsp/kernel-config/"]), false);
  assert.equal(shouldSkipVersionPath("docs/bsp/kernel-config", ["docs/bsp/kernel-config/"]), true);
});

// ── 全角字符：只把字母数字与全角空格算作问题 ──────────────────────────────
// 中文文档本就应当使用全角标点，把它们算作问题会产生海量假阳性（实测全仓 5086 行）。
const FW_ALNUM = String.fromCodePoint(0xff21, 0xff22, 0xff10); // ＡＢ０
const FW_SPACE = String.fromCodePoint(0x3000);
const FW_PUNCT = "，。；：！";

test("classifyFullwidth 不把中文全角标点算作问题", () => {
  const c = classifyFullwidth(`中文${FW_PUNCT}测试`);
  assert.equal(c.alnum, 0);
  assert.equal(c.space, 0);
  assert.ok(c.punct > 0);
});

test("classifyFullwidth 识别全角字母数字与全角空格", () => {
  assert.equal(classifyFullwidth(FW_ALNUM).alnum, 3);
  assert.equal(classifyFullwidth(`中间${FW_SPACE}空格`).space, 1);
});

test("scanTextConventions 跳过围栏与行内代码内的全角字符", () => {
  const fenced = ["```", `中文${FW_SPACE}${FW_ALNUM}`, "```", ""].join("\n");
  assert.equal(scanTextConventions(fenced, 12).fullwidthProblem, 0);
  assert.equal(scanTextConventions(`中文 \`${FW_ALNUM}\` 结束\n`, 12).fullwidthProblem, 0);
});

test("scanTextConventions 保留全角标点为统计信息而非问题", () => {
  const r = scanTextConventions(`中文${FW_PUNCT}结束\n`, 12);
  assert.equal(r.fullwidthProblem, 0);
  assert.ok(r.fullwidthAscii > 0, "既有 fullwidthAscii 统计应保持向后兼容");
});

test("scanTextConventions 全角问题样例带行号", () => {
  const md = ["第一行", `第二行${FW_SPACE}有全角空格`, "第三行"].join("\n");
  const r = scanTextConventions(md, 12);
  assert.equal(r.fullwidthProblem, 1);
  assert.equal(r.problemSamples[0].line, 2);
});

// ── refpath：反引号路径引用 ──────────────────────────────────────────────

test("expandRepoPathRef 只接受已知顶层目录下的仓库相对路径", () => {
  assert.deepEqual(expandRepoPathRef("src/cpp/src/Engine.cpp"), ["src/cpp/src/Engine.cpp"]);
  assert.deepEqual(expandRepoPathRef("FramePipeline.cpp"), [], "裸文件名歧义过大，不计");
  assert.deepEqual(expandRepoPathRef("try/catch"), [], "非仓库路径不得误判");
  assert.deepEqual(expandRepoPathRef("actions/setup-java"), [], "非仓库路径不得误判");
  assert.deepEqual(expandRepoPathRef("storage/win_face_db.yml"), [], "运行时路径不入库");
  assert.deepEqual(expandRepoPathRef("src/a.cpp:123"), ["src/a.cpp"], "行号后缀应剥离");
});

test("expandRepoPathRef 排除通配、花括号与占位写法", () => {
  assert.deepEqual(expandRepoPathRef("src/**/*.cpp"), []);
  assert.deepEqual(expandRepoPathRef("src/cpp/jni/{engine,camera}.cpp"), []);
  assert.deepEqual(expandRepoPathRef("src/a.b.cpp"), ["src/a.b.cpp"]);
});

test("expandRepoPathRef 展开 Foo.h/.cpp 合并写法", () => {
  assert.deepEqual(expandRepoPathRef("src/win/src/FramePipeline.h/.cpp"), [
    "src/win/src/FramePipeline.h",
    "src/win/src/FramePipeline.cpp",
  ]);
});

test("collectRepoPathRefs 跳过围栏代码块并记录行号", () => {
  const md = ["```", "`src/nope.cpp`", "```", "`src/also-nope.cpp`", ""].join("\n");
  const refs = collectRepoPathRefs(md);
  assert.deepEqual(refs.map((r) => r.path), ["src/also-nope.cpp"]);
  assert.equal(refs[0].line, 4);
});

test("auditRefPaths 仅对不存在的路径报 low", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "docs-sync-refpath-"));
  try {
    write(root, "src/real.cpp", "// x\n");
    const defects = auditRefPaths(
      [{ path: "src/real.cpp", line: 1 }, { path: "src/missing.cpp", line: 2 }],
      root,
      "README.md"
    );
    assert.equal(defects.length, 1);
    assert.equal(defects[0].severity, "low");
    assert.match(defects[0].message, /src\/missing\.cpp/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

// ── bsp：内核配置解析（此前完全无测试覆盖） ────────────────────────────────

test("parseKernelConfigMap 解析 CONFIG_X=y 与 is not set", () => {
  const m = parseKernelConfigMap(
    ["CONFIG_A=y", "# CONFIG_B is not set", "CONFIG_C=0x1000", "无关行"].join("\n")
  );
  assert.equal(m.get("CONFIG_A"), "y");
  assert.equal(m.get("CONFIG_B"), "n", "`# CONFIG_B is not set` 必须记为未启用");
  assert.equal(m.get("CONFIG_C"), "0x1000");
  assert.equal(m.size, 3, "无关行不得进入映射");
});

test("parseConfigItems 提取全部 CONFIG_ 标识符并去重", () => {
  const s = parseConfigItems("CONFIG_A 与 CONFIG_B=y；重复 CONFIG_A");
  assert.deepEqual([...s].sort(), ["CONFIG_A", "CONFIG_B"]);
});

test("isPlaceholderText 识别占位标记", () => {
  assert.equal(isPlaceholderText("# PLACEHOLDER\n"), true);
  assert.equal(isPlaceholderText("正文含 PLACEHOLDER 字样"), true);
  assert.equal(isPlaceholderText("# Linux/arm 4.4.143 Kernel Configuration"), false);
});

test("findDefconfigPath 与 findKernelConfigSnapshotPath 取首个反引号内匹配项", () => {
  const md = ["说明：`configs/rockchip_defconfig` 是基准。", "快照：`docs/bsp/kernel-config/kernel_cfg`。"].join("\n");
  assert.equal(findDefconfigPath(md), "configs/rockchip_defconfig");
  assert.equal(findKernelConfigSnapshotPath(md), "docs/bsp/kernel-config/kernel_cfg");
  assert.equal(findDefconfigPath("无匹配"), null);
});

const POLICY_MD = [
  "```",
  "DOCSYNC_KERNEL_CONFIG_POLICY",
  "[MUST]",
  "CONFIG_A",
  "[OPTIONAL]",
  "CONFIG_B",
  "[FORBIDDEN]",
  "CONFIG_C",
  "```",
].join("\n");

test("parseKernelConfigPolicyFromConstraints 解析三档分节", () => {
  const p = parseKernelConfigPolicyFromConstraints(POLICY_MD);
  assert.deepEqual(p.must, ["CONFIG_A"]);
  assert.deepEqual(p.optional, ["CONFIG_B"]);
  assert.deepEqual(p.forbidden, ["CONFIG_C"]);
  assert.equal(p.enabled, true);
});

test("parseKernelConfigPolicyFromConstraints 无任何分节时 enabled 为 false", () => {
  const p = parseKernelConfigPolicyFromConstraints("```\nDOCSYNC_KERNEL_CONFIG_POLICY\n```\n");
  assert.equal(p.enabled, false);
  assert.deepEqual(p.must, []);
});

test("parseKernelConfigPolicyFromConstraints 只有 FORBIDDEN 时 enabled 为 true 且 MUST 为空", () => {
  // 该情形下 MUST 为空，diff 计算依赖 Math.max(1, must.length) 兜底，不得除零
  const p = parseKernelConfigPolicyFromConstraints("```\nDOCSYNC_KERNEL_CONFIG_POLICY\n[FORBIDDEN]\nCONFIG_C\n```\n");
  assert.equal(p.enabled, true);
  assert.deepEqual(p.must, []);
  assert.equal(Math.max(1, p.must.length), 1);
});

// ── content：必需章节检查（此前无测试覆盖） ────────────────────────────────

test("findRequiredSections 命中 anyOf 中任一关键字即视为存在", () => {
  const md = ["# T", "## 项目简介", "## Quick Start", "## Dependencies", ""].join("\n");
  const required = [
    { name: "简介", anyOf: ["项目简介", "简介", "Overview"] },
    { name: "快速开始", anyOf: ["快速开始", "Quick Start"] },
    { name: "许可证", anyOf: ["许可证", "License"] },
  ];
  const r = findRequiredSections(md, required);
  assert.deepEqual(r.missing, ["许可证"]);
  assert.deepEqual(r.headings, ["T", "项目简介", "Quick Start", "Dependencies"]);
});

test("findRequiredSections 英文键按双侧词边界匹配：`## Contest` 不再顶替「测试命令」", () => {
  // 修复前 anyOf 里的 "test" 是无锚定子串正则，`## Contest` 被当成「测试命令」，
  // 于是 README 根本没有测试章节也能过门禁。
  const readmeReq = [{ name: "测试命令", anyOf: ["测试", "ctest", "test"] }];
  assert.deepEqual(findRequiredSections("# T\n## Contest\n", readmeReq).missing, ["测试命令"]);
  assert.deepEqual(findRequiredSections("# T\n## Rebuild\n", [{ name: "构建命令", anyOf: ["构建", "build"] }]).missing, ["构建命令"]);
  // 真正的 Test 章节（双侧词边界成立）仍命中
  assert.deepEqual(findRequiredSections("# T\n## Test\n", readmeReq).missing, []);
});

test("findRequiredSections 前缀误匹配：`PR` 不被 `Process` / `Project` 满足", () => {
  // 与 `## Contest` 同类的「半边」bug：`PR` 是 "Process" / "Project" 的前缀，
  // 无锚定子串或仅左词边界都会让它蒙混过关。双侧边界下必须判「提交流程」缺失。
  const req = [{ name: "提交流程", anyOf: ["提交", "commit", "pull request", "PR"] }];
  const md = "# T\n## 发布流程 (Release Process)\n## 项目地图 (Project Map)\n";
  assert.deepEqual(findRequiredSections(md, req).missing, ["提交流程"]);
  // 真正的提交小节（中文键子串 / 英文单词）仍命中
  assert.deepEqual(findRequiredSections("# T\n## 6.8 提交流程 (Commit & Pull Request)\n", req).missing, []);
});

test("findRequiredSections 中文键保持子串包含", () => {
  // 中文没有词边界概念：`## 项目简介与运行模式` 仍应满足「项目简介」
  assert.deepEqual(findRequiredSections("# T\n## 项目简介与运行模式\n", [{ name: "简介", anyOf: ["项目简介"] }]).missing, []);
});

test("sectionKeyHit 双侧边界：真实标题里的 CONFIG_ / Quick Start / Release 仍命中", () => {
  // 改坏即回归：这些键在仓库真实标题上必须仍命中
  assert.equal(sectionKeyHit("3.1 内核配置（Kernel config）最小关注清单（CONFIG_）", "CONFIG_"), true);
  // 下划线结尾的前缀键只要求左边界：必须命中真实标题 ## CONFIG_FOO（同一键，不再靠
  // 「恰好成边界的构造（CONFIG_）」冒充证据——那种构造双侧边界也能过，证不出判别力）
  assert.equal(sectionKeyHit("3.1 内核配置（Kernel config）最小关注清单（CONFIG_FOO）", "CONFIG_"), true, "前缀键 CONFIG_ 应命中 CONFIG_FOO");
  assert.equal(sectionKeyHit("MYCONFIG_X", "CONFIG_"), false, "前缀键仍须左边界，MYCONFIG_X 不得命中");
  assert.equal(sectionKeyHit("4.1 快速开始 (Quick Start)", "Quick Start"), true);
  assert.equal(sectionKeyHit("6.3 发布流程 (Release Process)", "Release"), true);
  assert.equal(sectionKeyHit("T\nTest", "test"), true);
  // 后缀/内嵌/前缀三类误匹配一律不命中
  assert.equal(sectionKeyHit("Contest", "test"), false);
  assert.equal(sectionKeyHit("Rebuild", "build"), false);
  assert.equal(sectionKeyHit("6.3 发布流程 (Release Process)", "PR"), false, "前缀 `Pr`ocess 不得命中 `PR`");
  assert.equal(sectionKeyHit("3. 项目地图 (Project Map)", "PR"), false);
  // 双侧边界不接受词形变化：这些组靠中文键兜底（`调试`/`代码规范`）
  assert.equal(sectionKeyHit("6.6 调试与排障 (Debugging)", "debug"), false, "debug 不匹配 Debugging");
  assert.equal(sectionKeyHit("6.2 代码规范 (Code Style)", "format"), false);
  // `style` 命中的是「完整单词」，不是词形变化反例：Code Style 里 Style 双侧成界 → true
  assert.equal(sectionKeyHit("6.2 代码规范 (Code Style)", "style"), true, "Style 是完整单词，双侧成界应命中");
  assert.equal(sectionKeyHit("6.2 Code Styling", "style"), false, "词形变化 Styling 才不命中 style");
  assert.equal(sectionKeyHit("6.6 调试与排障 (Debugging)", "调试"), true, "中文键子串兜底");
  assert.equal(sectionKeyHit("6.2 代码规范 (Code Style)", "代码规范"), true);
});

// ── --fix：Markdown 自动修复的 4 条规则（此前无测试覆盖） ──────────────────

test("fixMarkdown 为未声明语言的围栏补语言", () => {
  const defects = [];
  const out = fixMarkdown(["# T", "```", '{"a": 1}', "```", ""].join("\n"), defects);
  assert.match(out, /```\s*json/, "JSON 内容应被推断为 json");
  assert.ok(defects.some((d) => d.rule === "code_fence_lang"));
});

test("fixMarkdown 对无法识别的内容回退为 bash（锁定启发式现状）", () => {
  const defects = [];
  const out = fixMarkdown(["# T", "```", "cmake -S . -B build", "```", ""].join("\n"), defects);
  assert.match(out, /```\s*bash/);
});

test("fixMarkdown 为重复标题追加后缀并插入锚点", () => {
  const defects = [];
  const out = fixMarkdown(["# T", "## 说明", "正文", "## 说明", ""].join("\n"), defects);
  assert.match(out, /## 说明（2）/);
  assert.match(out, /<a id="[^"]+-2"><\/a>/);
  assert.ok(defects.some((d) => d.rule === "MD024"));
});

test("fixMarkdown 把裸露 URL 包裹为尖括号，已包裹的不重复处理", () => {
  const defects = [];
  const out = fixMarkdown("见 https://example.com/a 与 <https://example.com/b>\n", defects);
  assert.match(out, /<https:\/\/example\.com\/a>/, "裸露 URL 应被包裹");
  assert.equal(defects.filter((d) => d.rule === "MD034").length, 1);
});

test("fixMarkdown 不折行表格行、列表行与标题行", () => {
  const long = "x".repeat(200);
  const md = ["# T", `| ${long} |`, `- ${long}`, `## ${long}`, ""].join("\n");
  const defects = [];
  assert.equal(fixMarkdown(md, defects).split("\n").length, md.split("\n").length);
  assert.equal(defects.filter((d) => d.rule === "MD013").length, 0);
});

// ── link：外部链接的三档严重级别（此前无测试覆盖） ────────────────────────

test("classifyLinkResult 只对确凿失效报 medium", () => {
  assert.equal(classifyLinkResult({ ok: true }), null, "成功链接不产生缺陷");
  assert.equal(classifyLinkResult({ ok: false, unverifiable: true, method: "WORKER" }), "low");
  assert.equal(classifyLinkResult({ ok: false, method: "CACHE", status: 404 }), "low");
  assert.equal(classifyLinkResult({ ok: false, method: "HEAD", status: 404 }), "medium");
  assert.equal(classifyLinkResult({ ok: false, method: "HEAD", status: 500 }), "medium");
});

test("link 的 low 不阻断门禁，medium 阻断", () => {
  // 网络抖动导致的请求失败若阻断，CI 会随机变红
  assert.equal(isGatePassed({ high: 0, medium: 0, low: 5 }), true);
  assert.equal(isGatePassed({ high: 0, medium: 1, low: 0 }), false);
});

test("linkDefectMessage 按档位给出可区分的文案", () => {
  const u = "https://example.com/x";
  assert.match(linkDefectMessage({ url: u, method: "WORKER", finalUrl: u, unverifiable: true }, "low"), /无法验证/);
  assert.match(linkDefectMessage({ url: u, method: "CACHE", status: 404 }, "low"), /缓存/);
  assert.match(linkDefectMessage({ url: u, method: "HEAD", status: 404, finalUrl: u }, "medium"), /失效链接/);
});

// ── 文档元数据与豁免语义（docs/DOC_STANDARD.md） ──────────────────────────

test("parseDocMeta 解析元数据块，无块时返回 null", () => {
  const meta = parseDocMeta("<!-- DOCSYNC_DOC\ntype=design\nstatus=living\nas_of=2026-09-15\n-->\n\n# T\n");
  assert.deepEqual(meta, { type: "design", status: "living", as_of: "2026-09-15" });
  assert.equal(parseDocMeta("# 无块\n"), null);
  assert.equal(parseDocMeta("<!-- DOCSYNC_DOC\ntype=design\n"), null, "块未闭合时返回 null");
});

test("isLivingDoc 只在 status=living 或缺失元数据时为真", () => {
  const mk = (s) => `<!-- DOCSYNC_DOC\ntype=plan\nstatus=${s}\nas_of=2026-01-01\n-->`;
  // 时点快照不是活文档——这是「豁免靠自述而非目录」的核心语义
  assert.equal(isLivingDoc(mk("snapshot")), false);
  assert.equal(isLivingDoc(mk("frozen")), false);
  assert.equal(isLivingDoc(mk("placeholder")), false);
  assert.equal(isLivingDoc(mk("living")), true);
  // 缺块时保守按 living 处理（缺失本身由 doc-meta 报错），
  // 避免一个坏掉的块顺带关掉 version 与 refpath 两条检查
  assert.equal(isLivingDoc("# 无块"), true);
});

test("requiresDocMeta 只豁免第三方内容与工具配置", () => {
  assert.equal(requiresDocMeta("docs/designs/a.md"), true);
  assert.equal(requiresDocMeta("docs/research/appendix-b.md"), true, "研究附录仍是项目文档，只是 status=snapshot");
  assert.equal(requiresDocMeta("deps/rk_mpp/README.md"), false);
  assert.equal(requiresDocMeta(".claude/agents/win.md"), false);
  assert.equal(requiresDocMeta("src/win/third_party/civetweb/LICENSE.md"), false);
  assert.equal(requiresDocMeta("src/win/third_party/civetweb/LEDGER.md"), true, "本仓自撰台账，需元数据");
});

test("auditDocMeta 报告缺失与非法取值", () => {
  const missing = auditDocMeta("a.md", "# 无块");
  assert.equal(missing.length, 1);
  assert.match(missing[0].message, /缺少 DOCSYNC_DOC/);
  assert.equal(missing[0].severity, "high");
  const bad = auditDocMeta("a.md", "<!-- DOCSYNC_DOC\ntype=nope\nstatus=zzz\nas_of=2026/01/01\n-->");
  assert.equal(bad.length, 3, "type / status / as_of 三项都应报错");
  assert.equal(auditDocMeta("a.md", "<!-- DOCSYNC_DOC\ntype=design\nstatus=living\nas_of=2026-01-01\n-->").length, 0);
});

// ── 元数据块位置 / 唯一性 / 日历日期（docs/DOC_STANDARD.md §1.3） ─────────────

const META_BLOCK = (asOf = "2026-01-01") => `<!-- DOCSYNC_DOC\ntype=design\nstatus=living\nas_of=${asOf}\n-->`;

test("parseDocMeta 只在前 20 行内查找元数据块", () => {
  // 块在第 1 行内 → 命中
  assert.notEqual(parseDocMeta(META_BLOCK()), null);
  // 块被埋到第 22 行（前 20 行之后）→ 视为不存在
  const buried = Array.from({ length: 21 }, (_, i) => `第 ${i + 1} 行`).join("\n") + "\n" + META_BLOCK() + "\n";
  assert.equal(parseDocMeta(buried), null);
});

test("auditDocMeta 对「元数据块不在前 20 行内」报 high", () => {
  const buried = Array.from({ length: DOC_META_HEAD_LINES + 1 }, (_, i) => `第 ${i + 1} 行`).join("\n") + "\n" + META_BLOCK() + "\n";
  const defects = auditDocMeta("a.md", buried);
  assert.equal(defects.length, 1);
  assert.equal(defects[0].severity, "high");
  assert.match(defects[0].message, /前 20 行内/);
});

test("auditDocMeta 对「出现多于一个元数据块」报 high", () => {
  const md = `${META_BLOCK()}\n\n正文\n\n${META_BLOCK()}\n`;
  const defects = auditDocMeta("a.md", md);
  assert.equal(defects.length, 1);
  assert.equal(defects[0].severity, "high");
  assert.match(defects[0].message, /只能有一个/);
});

test("analyzeDocMetaMarkers 忽略代码围栏里的示例块", () => {
  // docs/DOC_STANDARD.md §1 自身会在围栏里展示一个示例块；若把它算作第二个块，规格文档会误报
  const md = `${META_BLOCK()}\n\n\`\`\`\n${META_BLOCK()}\n\`\`\`\n`;
  assert.deepEqual(analyzeDocMetaMarkers(md), { count: 1, firstLine: 1 });
  assert.equal(auditDocMeta("a.md", md).length, 0, "围栏示例不得触发「多个块」");
});

test("isCalendarDate 拒绝日历上不存在的日期（rollover 陷阱）", () => {
  // new Date("2026-02-31") 在本机 node 会 rollover 成 3-03 而非 Invalid Date，
  // 单看 Date.parse 是否 NaN 不够 —— 必须回写比对。
  assert.equal(isCalendarDate("2026-02-31"), false);
  assert.equal(isCalendarDate("2026-02-29"), false, "2026 非闰年");
  assert.equal(isCalendarDate("2024-02-29"), true, "2024 是闰年");
  assert.equal(isCalendarDate("2026-02-28"), true);
  assert.equal(isCalendarDate("2026/01/01"), false, "形状不对");
  assert.equal(isCalendarDate(""), false);
});

test("auditDocMeta 把 as_of=2026-02-31 判为 high，2026-02-28 放行", () => {
  assert.equal(auditDocMeta("a.md", META_BLOCK("2026-02-31")).length, 1);
  assert.equal(auditDocMeta("a.md", META_BLOCK("2026-02-28")).length, 0);
  assert.equal(auditDocMeta("a.md", META_BLOCK("2024-02-29")).length, 0);
});

// ── 元数据豁免清单与 docs/DOC_STANDARD.md §1.4 对齐 ─────────────────────────

test("DOC_META_EXEMPT_PATTERNS 与 §1.4 对齐且不含死条目", () => {
  assert.ok(DOC_META_EXEMPT_PATTERNS.includes("deps/"), "deps/** 整树");
  assert.ok(DOC_META_EXEMPT_PATTERNS.includes("src/win/third_party/"), "third_party/** 整树（旧实现只豁免 LICENSE.md 一个文件）");
  assert.ok(DOC_META_EXEMPT_PATTERNS.includes(".claude/agents/"));
  // 两条根级工作台账当前不存在，是死条目，必须删除
  assert.ok(!DOC_META_EXEMPT_PATTERNS.includes("FULL_PROJECT_AUDIT_2026-07-22.md"));
  assert.ok(!DOC_META_EXEMPT_PATTERNS.includes("CLAUDECODE_DEEPSEEKV4FLASH_GOVERNANCE_TASKS_2026-07-22.md"));
  // 例外优先清单保留 LEDGER.md
  assert.ok(DOC_META_EXEMPT_UNLESS.includes("src/win/third_party/civetweb/LEDGER.md"));
});

test("requiresDocMeta 对 third_party 整树豁免但保留 LEDGER.md 例外", () => {
  assert.equal(requiresDocMeta("src/win/third_party/civetweb/LICENSE.md"), false);
  assert.equal(requiresDocMeta("src/win/third_party/other/LIB.md"), false, "整树豁免");
  assert.equal(requiresDocMeta("src/win/third_party/civetweb/LEDGER.md"), true, "例外优先，仍需元数据");
});

test("auditDocMetaExemptList 对文件级死条目报 low、目录前缀不检查、默认清单纳入 UNLESS 文件级项", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "docs-sync-exempt-"));
  try {
    write(root, "docs/exists.md", "x");
    // 目录前缀条目不检查存在性 → 无缺陷
    assert.equal(auditDocMetaExemptList(root, ["deps/", "src/win/third_party/"]).length, 0);
    // 文件级条目存在 → 无缺陷
    assert.equal(auditDocMetaExemptList(root, ["docs/exists.md"]).length, 0);
    // 文件级条目不存在 → 一条 low
    const dead = auditDocMetaExemptList(root, ["docs/gone.md"]);
    assert.equal(dead.length, 1);
    assert.equal(dead[0].severity, "low");
    assert.match(dead[0].message, /docs\/gone\.md/);
    // 默认清单把 DOC_META_EXEMPT_UNLESS 的文件级条目并入自检：空夹具目录下唯一会报的
    // 就是 LEDGER.md（DOC_META_EXEMPT_PATTERNS 三项全为目录前缀 → 全跳过；若 UNLESS
    // 未被纳入，则默认调用会返回 []，此处即红）
    const defaultHits = auditDocMetaExemptList(root);
    assert.equal(defaultHits.length, 1, "默认清单应只报 UNLESS 的文件级死条目");
    assert.equal(defaultHits[0].severity, "low");
    assert.match(defaultHits[0].message, /civetweb\/LEDGER\.md/);
    // 真实仓库下 UNLESS 的文件级条目（LEDGER.md）存在 → 如实断言无缺陷
    const real = path.join(__dirname, "..", "..");
    assert.equal(auditDocMetaExemptList(real).length, 0);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("豁免不再由目录决定：目录不产生豁免，声明才产生", () => {
  // 回归点：旧实现按目录豁免（TIMEPOINT_DOC_PATTERNS），把文档挪进 docs/research/
  // 即可同时免检 version 与 refpath，是实际生效的后门。现改由 status 自述决定。
  assert.equal(requiresDocMeta("docs/research/appendix-b.md"), true);
  assert.equal(isLivingDoc("<!-- DOCSYNC_DOC\ntype=research\nstatus=living\nas_of=2026-01-01\n-->"), true);
  assert.equal(isLivingDoc("<!-- DOCSYNC_DOC\ntype=design\nstatus=snapshot\nas_of=2026-01-01\n-->"), false);
});

test("CI 命令行不得传入整体关闭检查的参数", () => {
  // --skip-check-paths 是整体替换而非追加，会同时关闭 version 与 refpath。
  // 此前没有任何测试守护 CI 命令行，一行参数即可让全仓检查静默失效。
  const repoRoot = path.join(__dirname, "..", "..");
  for (const wf of ["ci.yml", "docs-audit.yml"]) {
    const p = path.join(repoRoot, ".github", "workflows", wf);
    if (!fs.existsSync(p)) continue;
    const yml = fs.readFileSync(p, "utf8");
    assert.ok(!yml.includes("--skip-check-paths"), `${wf} 不得传入 --skip-check-paths`);
    assert.ok(!yml.includes("--skip-version-paths"), `${wf} 不得传入已废弃的 --skip-version-paths`);
  }
});
