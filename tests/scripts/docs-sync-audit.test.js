const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const { auditAgentGuides, auditCrossRefs, isGatePassed, resolveCrossRefPath } = require("../../scripts/docs-sync-audit.js");

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
