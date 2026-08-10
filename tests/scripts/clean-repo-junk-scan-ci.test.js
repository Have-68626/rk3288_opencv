// Q-CQ-B08-010 判别测试：clean-repo-junk.js 的 `scan --ci` 必须是无副作用的纯检查。
//
// 判别逻辑（修复前 RED，修复后 GREEN）：
//   - 修复前：scan --ci 即使候选为 0，也会在仓库内创建 ErrorLog/ 并写入
//     run-meta / tree / disk-usage / scan-report / snapshot-summary 等约 11 个
//     报告文件（见 scripts/clean-repo-junk.js main()）。连续两次 scan --ci
//     都会在仓库内新增一批文件，因此仓库文件集发生变化。
//   - 修复后：scan 默认把全部报告定向系统临时区，stdout 输出摘要与报告路径，
//     不创建 ErrorLog/，连续扫描不再改变仓库文件集。
//
// 运行：node --test tests/scripts/clean-repo-junk-scan-ci.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const crypto = require("node:crypto");
const { execFileSync } = require("node:child_process");

const SCRIPT = path.resolve(__dirname, "..", "..", "scripts", "clean-repo-junk.js");

function write(root, relativePath, text) {
  const target = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, text, "utf8");
}

function sha256(buf) {
  return crypto.createHash("sha256").update(buf).digest("hex");
}

// 快照仓库文件集：{ relPath: sha256 }，仅统计文件（符号链接跳过），目录本身不计。
function snapshotFiles(root) {
  const out = {};
  const stack = [root];
  while (stack.length > 0) {
    const cur = stack.pop();
    for (const d of fs.readdirSync(cur, { withFileTypes: true })) {
      const p = path.join(cur, d.name);
      if (d.isSymbolicLink()) continue;
      if (d.isDirectory()) {
        stack.push(p);
        continue;
      }
      const rel = path.relative(root, p).split(path.sep).join("/");
      out[rel] = sha256(fs.readFileSync(p));
    }
  }
  return out;
}

// 在 fixture 仓库内执行一次 `scan --ci`，返回 stdout。
function runScanCI(root) {
  return execFileSync(process.execPath, [SCRIPT, "scan", "--ci", "--root", root], {
    cwd: root,
    encoding: "utf8",
  });
}

// 干净 fixture：不含任何垃圾文件，scan 候选为 0——即便如此，修复前仍会落库。
function createFixture() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "clean-repo-junk-scan-"));
  write(root, "README.md", "# fixture\n");
  write(root, "src/main.js", "module.exports = 1;\n");
  // 让 detectRepoRoot 确定性地停在 fixture 根目录。
  write(root, ".trae/specs/clean-repo-junk/spec.md", "# spec marker\n");
  return root;
}

test("scan --ci 连续两次不改仓库文件集且报告不落仓库内（Q-CQ-B08-010）", () => {
  const root = createFixture();
  try {
    const before = snapshotFiles(root);

    // 第一次 scan --ci：报告必须定向临时区/stdout，不得在仓库内落任何文件。
    const out1 = runScanCI(root);
    assert.match(out1, /scan 完成: /, "scan 应在 stdout 输出报告路径（报告仍被生成）");
    const afterFirst = snapshotFiles(root);
    assert.deepEqual(afterFirst, before, "scan --ci 不得在仓库内生成任何报告文件（纯检查）");

    // 第二次 scan --ci：连续扫描文件集必须稳定。
    runScanCI(root);
    const afterSecond = snapshotFiles(root);
    assert.deepEqual(afterSecond, afterFirst, "连续 scan --ci 文件集必须保持稳定");
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});
