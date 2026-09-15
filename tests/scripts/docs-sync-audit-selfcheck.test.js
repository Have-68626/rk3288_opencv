// 全仓文档门禁自检：断言真实仓库上 `docs-sync-audit` 门禁为绿。
//
// 背景：本项目用 squash merge，master 上的 merge commit 只来自偶发的 `git pull`，
// 因此旧的 `git log --merges` 基准会长期冻结，使门禁持续假红（2026-09 实测 45 个 high
// 缺陷全部来自该基准，而非文档内容）。本测试把「真实仓库门禁必须通过」固化为可执行
// 断言，使基准再次失效时能在 CI 立刻暴露，而不是靠人肉发现。
//
// 注意：本文件断言的是"仓库当前状态"，因此它只有在文档与门禁都修好之后才会 GREEN。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const REPO_ROOT = path.join(__dirname, "..", "..");
const SCRIPT = path.join(REPO_ROOT, "scripts", "docs-sync-audit.js");

test("真实仓库门禁自检：退出码 0 且无 high/medium 缺陷", () => {
  const outDir = fs.mkdtempSync(path.join(os.tmpdir(), "docs-sync-selfcheck-"));
  try {
    const r = spawnSync(
      process.execPath,
      [SCRIPT, "--skip-links", "--no-md", "--out-dir", outDir],
      { cwd: REPO_ROOT, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 }
    );

    const reportFile = fs
      .readdirSync(outDir)
      .filter((f) => f.startsWith("docs_sync_audit_") && f.endsWith(".json"))
      .sort()
      .pop();
    const report = reportFile
      ? JSON.parse(fs.readFileSync(path.join(outDir, reportFile), "utf8"))
      : null;
    const detail = report
      ? report.defects.map((d) => `[${d.severity}] ${d.type}: ${d.file || "N/A"} - ${d.message}`).join("\n")
      : `stdout=${r.stdout}\nstderr=${r.stderr}`;

    assert.equal(
      r.status,
      0,
      `门禁未通过（退出码 ${r.status}），真实仓库文档存在阻断级缺陷：\n${detail}`
    );
    assert.equal(
      report.summary.severity.high,
      0,
      `存在 high 缺陷：\n${detail}`
    );
    assert.equal(
      report.summary.severity.medium,
      0,
      `存在 medium 缺陷：\n${detail}`
    );
  } finally {
    fs.rmSync(outDir, { recursive: true, force: true });
  }
});
