// 治理任务 SEC-17（S-SEC-B10-008）：CivetWeb 来源账本 —— 摘要与差异可重建。
// 背景：src/win/third_party/civetweb/ 为 CivetWeb 1.16 源码与许可证归档快照（未编入目标，
// 见 CREDITS.md），此前无独立补丁/摘要账本，无法核对本地文件与上游一致、是否存在未记录改动。
// LEDGER.md 记录：上游仓库与 commit、各文件 git blob SHA-256、本地补丁集说明。
// 本测试从 LEDGER.md 解析期望 hash，与当前 git blob 复算值比对（行尾无关），保证账本可重建。
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const DIR = "src/win/third_party/civetweb";
const LEDGER = path.join(REPO_ROOT, DIR, "LEDGER.md");

// 账本必须覆盖的目录文件清单（源文件 + 许可证归档）。
const MANIFEST = ["LICENSE.md", "include/civetweb.h", "src/civetweb.c", "src/md5.inl"];

function git(args) {
  const r = spawnSync("git", args, { cwd: REPO_ROOT, encoding: "buffer" });
  assert.equal(r.status, 0, `git ${args.join(" ")} 失败: ${r.stderr && r.stderr.toString()}`);
  return r.stdout;
}

// 规范摘要：git blob（LF 归一化入库字节），与 checkout 行尾状态无关（同 manifest-anchor 约定）。
function blobSha256(rel) {
  return crypto.createHash("sha256").update(git(["show", `HEAD:${DIR}/${rel}`])).digest("hex");
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
  const missing = MANIFEST.filter((f) => !(f in map));
  assert.deepEqual(missing, [], `账本缺少条目: ${missing.join(", ")}`);
});

test("SEC-17：账本摘要可复算（记录 hash == 当前 git blob hash）", () => {
  const map = ledgerHashes();
  for (const rel of MANIFEST) {
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
