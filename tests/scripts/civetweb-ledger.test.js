// 治理任务 SEC-17（S-SEC-B10-008）：CivetWeb 来源账本 —— 摘要与差异可重建。
// 背景：src/win/third_party/civetweb/ 为 CivetWeb 1.16 源码与许可证归档快照（未编入目标，
// 见 CREDITS.md），此前无独立补丁/摘要账本，无法核对本地文件与上游一致、是否存在未记录改动。
// LEDGER.md 记录：上游仓库与 commit、各文件 git blob SHA-256、本地补丁集说明。
// 本测试从 LEDGER.md 解析期望 hash，与当前 git blob 复算值比对（行尾无关），保证账本可重建。
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const DIR = "src/win/third_party/civetweb";
const LEDGER = path.join(REPO_ROOT, DIR, "LEDGER.md");

// 运行时枚举目录内全部普通文件（排除账本自身）：新增文件无需手工改 MANIFEST 即被
// "账本覆盖全部目录文件"校验捕获。用 fs 目录遍历而非 git ls-files —— 未提交的新文件
// 同样会被识别，账本缺漏立即 RED（git ls-files 只列出已跟踪文件，会漏掉新增未提交文件）。
function dirManifest() {
  const root = path.join(REPO_ROOT, DIR);
  const out = [];
  const stack = [""];
  while (stack.length > 0) {
    const rel = stack.pop();
    const abs = path.join(root, rel);
    for (const entry of fs.readdirSync(abs, { withFileTypes: true })) {
      const childRel = rel ? `${rel}/${entry.name}` : entry.name;
      if (entry.isDirectory()) {
        stack.push(childRel);
      } else if (entry.isFile()) {
        if (childRel === "LEDGER.md") continue; // 账本不覆盖自身
        out.push(childRel);
      }
    }
  }
  return out.sort();
}

// 规范摘要：工作区读取（fs.readFile + CRLF→LF 归一化），与 git blob（LF 入库字节）对齐、
// 与 checkout 行尾状态无关（同 manifest-anchor 约定，.gitattributes `* text=auto eol=lf`）。
// 与旧实现 git show HEAD:... 的关键差异：后者只读"已提交 blob"，本地未提交的修改会假绿
// （账本仍与 HEAD 一致，测不出工作区漂移）；工作区读取使任何未提交改动立即被账本校验
// 捕获为 RED —— 账本校验的不再是"历史上提交了什么"，而是"磁盘当前实际是什么"。
function blobSha256(rel) {
  const raw = fs.readFileSync(path.join(REPO_ROOT, DIR, rel));
  return crypto
    .createHash("sha256")
    .update(raw.toString("utf8").replace(/\r\n/g, "\n"))
    .digest("hex");
}

// 从 LEDGER.md 解析路径→SHA-256 映射：匹配表格行 `| \`路径\` | \`64hex\` |`（hash 允许带/不带反引号）。
function ledgerHashes() {
  const md = fs.readFileSync(LEDGER, "utf8");
  const map = {};
  const re = /`([^`]+)`\s*\|\s*`?([0-9a-f]{64})`?\s*\|/g;
  let m;
  while ((m = re.exec(md)) !== null) {
    const rel = m[1].replace(/^\.\//, "");
    if (map[rel] !== undefined) {
      assert.fail(`LEDGER.md 中路径 ${rel} 重复出现`);
    }
    map[rel] = m[2];
  }
  return map;
}

// RED：账本缺失时本测试失败，证明"账本缺失无法重建/验证"；补账本后转 GREEN。
test("SEC-17：LEDGER.md 账本必须存在", () => {
  assert.ok(fs.existsSync(LEDGER), `缺少 CivetWeb 来源账本：${LEDGER}`);
});

test("SEC-17：账本覆盖全部目录文件，无缺漏", () => {
  const map = ledgerHashes();
  const manifest = dirManifest();
  assert.ok(manifest.length >= 4, `目录文件枚举异常: ${manifest.length}`);
  const missing = manifest.filter((f) => !(f in map));
  assert.deepEqual(missing, [], `账本缺少条目: ${missing.join(", ")}`);
});

test("SEC-17：账本摘要可复算（记录 hash == 当前 git blob hash）", () => {
  const map = ledgerHashes();
  for (const rel of dirManifest()) {
    assert.equal(map[rel], blobSha256(rel), `${rel} 账本 hash 与 git blob 不一致，来源账本失步`);
  }
});

// 上游 commit 锚定：账本须记录可核实的版本与提交标识，防止来源无从追查。
test("SEC-17：账本记录上游版本与 commit 锚点", () => {
  const md = fs.readFileSync(LEDGER, "utf8");
  assert.match(md, /v1\.16/i, "账本应声明上游版本 1.16");
  assert.match(md, /[0-9a-f]{40}/, "账本应记录上游 40 位 commit SHA");
});

// 本地补丁可追溯：civetweb.c 的 4 个安全补丁须在账本中逐一登记本地 commit。
test("SEC-17：账本登记 civetweb.c 本地补丁集（可追溯）", () => {
  const md = fs.readFileSync(LEDGER, "utf8");
  for (const sha of ["997197f", "85879e2", "93ccc06", "4b4e0e2"]) {
    assert.ok(md.includes(sha), `账本应登记本地补丁 commit ${sha}`);
  }
});
