// 治理任务 SEC-15（S-SEC-B10-006）：外部二进制来源账本 —— 字段齐全 + 摘要可复算。
// 背景：CREDITS.md 模型台账多数条目标注「⚠️ 需在部署时验证」/「—」，无来源 URL、无
// 可复算摘要，外部二进制来源不可追溯。SEC-01（ncnn，CREDITS.md）、SEC-13（Gradle
// wrapper）、SEC-17（CivetWeb，src/win/third_party/civetweb/LEDGER.md）已建成账本；
// 本任务将字段级台账收敛到 docs/security/supply-chain-ledger.md（引用而非重复既有账本）。
//
// RED（失败）：账本缺失；或任一必录外部二进制缺行；或任一字段为空/占位符（—）；或
//   摘要列既无 64 位 hex 也无「[状态标记] 可复算方式」；或 LBP 模型 git blob hash 与账本不一致。
// GREEN（通过）：账本齐备且字段可复算。
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const LEDGER = path.join(REPO_ROOT, "docs", "security", "supply-chain-ledger.md");

// 外部二进制清单：账本必须逐项覆盖（匹配第一列，大小写/空白/反引号无关）。
const MANIFEST_KEYS = [
  "opencv-contrib",
  "opencv",
  "ncnn",
  "civetweb",
  "lbp",
  "scrfd",
  "arcface",
  "mobilefacenet",
  "yolo",
  "ffmpegkit",
  "mpp",
  "snpe",
  "libyuv",
];

// 摘要列允许的状态标记：带方括号的显式状态 + 可复算方式（后续文本说明复算命令）。
// 仅出现「⚠️ 需在部署时验证」这类旧占位符（无方括号状态）视为缺摘要，判 RED。
const HASH_STATUS_RE =
  /\[(参考 SEC-0[1-9]|参考 SEC-1[0-9]|部署时生成|部署时获取|部署时验证|未随附|未集成|未启用|未定)\]/;

// 许可证列已知关键词（大小写无关）；自训练/供应商分发项须显式声明 [未定]/[供应商专有]。
const LICENSE_RE =
  /mit|apache|bsd|lgpl|gpl|zlib|android sdk|microsoft|专有|\[(未定|未随附|未集成|未启用|供应商专有)\]/i;

// 来源列：必须是可追踪 URL，或显式 [未随附]/[未集成]/[未启用]/[未定]。
const SOURCE_RE = /https?:\/\//;

const PLACEHOLDER_RE = /^[—-]+$/;

function git(args) {
  const r = spawnSync("git", args, { cwd: REPO_ROOT, encoding: "buffer" });
  assert.equal(r.status, 0, `git ${args.join(" ")} 失败: ${r.stderr && r.stderr.toString()}`);
  return r.stdout;
}

// 规范摘要：git blob（LF 归一化入库字节），与 checkout 行尾状态无关（同 SEC-17 约定）。
function blobSha256(rel) {
  return crypto.createHash("sha256").update(git(["show", `HEAD:${rel}`])).digest("hex");
}

function norm(s) {
  return String(s || "").replace(/[`* ]/g, "").toLowerCase();
}

function isBlank(c) {
  return c === "" || PLACEHOLDER_RE.test(c);
}

// 解析账本清单表：行以 `|` 开头且恰好 7 个数据列；跳过表头/分隔行。
function manifestRows() {
  const md = fs.readFileSync(LEDGER, "utf8");
  const rows = [];
  for (const line of md.split(/\r?\n/)) {
    const cells = line.split("|").map((c) => c.trim());
    if (cells.length < 3 || cells[0] !== "" || cells[cells.length - 1] !== "") continue;
    const data = cells.slice(1, -1);
    if (data[0] === "依赖") continue;
    if (data.every((c) => /^:?[- ]+:?$/.test(c))) continue;
    if (data.length === 7) rows.push(data);
  }
  return rows;
}

test("SEC-15：来源账本文件必须存在", () => {
  assert.ok(fs.existsSync(LEDGER), `缺少外部二进制来源账本：${LEDGER}`);
});

test("SEC-15：账本覆盖全部外部二进制，无缺漏", () => {
  const rows = manifestRows();
  const firstCells = rows.map((r) => norm(r[0]));
  const missing = MANIFEST_KEYS.filter((k) => !firstCells.some((c) => c.includes(k)));
  assert.deepEqual(missing, [], `账本缺少外部二进制条目: ${missing.join(", ")}`);
});

test("SEC-15：每个条目字段齐全，禁止空值/占位符（来源/版本/许可证/摘要/发布渠道/部署状态）", () => {
  const rows = manifestRows();
  assert.ok(rows.length >= MANIFEST_KEYS.length, `清单行数不足（${rows.length} 行）`);
  for (const r of rows) {
    for (const cell of r.slice(1)) {
      assert.ok(!isBlank(cell), `条目 ${r[0]} 存在空字段或占位符字段: ${JSON.stringify(cell)}`);
    }
  }
});

test("SEC-15：来源列须为 URL 或显式状态标记", () => {
  for (const r of manifestRows()) {
    assert.ok(
      SOURCE_RE.test(r[1]) || /\[(未随附|未集成|未启用|未定)\]/.test(r[1]),
      `条目 ${r[0]} 来源列不可追溯: ${r[1]}`,
    );
  }
});

test("SEC-15：许可证列须为已知许可证或显式声明", () => {
  for (const r of manifestRows()) {
    assert.ok(LICENSE_RE.test(r[3]), `条目 ${r[0]} 许可证列缺失/非法: ${r[3]}`);
  }
});

test("SEC-15：摘要列须含 64 位 hex 或显式可复算状态标记（拒绝「⚠️ 需在部署时验证」旧占位）", () => {
  for (const r of manifestRows()) {
    const ok = /[0-9a-f]{64}/.test(r[4]) || HASH_STATUS_RE.test(r[4]);
    assert.ok(ok, `条目 ${r[0]} 摘要列缺可复算方式: ${r[4]}`);
  }
});

test("SEC-15：仓库内可复算摘要（LBP 模型 git blob）与账本一致", () => {
  const md = fs.readFileSync(LEDGER, "utf8");
  const re = /`([^`]+)`\s*\|\s*`?([0-9a-f]{64})`?\s*\|/g;
  let m = null;
  let found = null;
  while ((m = re.exec(md)) !== null) {
    if (m[1] === "app/src/main/assets/lbpcascade_frontalface.xml") found = m[2];
  }
  assert.ok(found, "可复算摘要表应包含 app/src/main/assets/lbpcascade_frontalface.xml");
  assert.equal(found, blobSha256("app/src/main/assets/lbpcascade_frontalface.xml"),
    "LBP 模型账本 hash 与当前 git blob 不一致，来源账本失步");
});

test("SEC-15：账本引用既有账本（SEC-01 ncnn / SEC-17 CivetWeb），避免重复台账", () => {
  const md = fs.readFileSync(LEDGER, "utf8");
  assert.match(md, /CREDITS\.md/, "账本应引用 CREDITS.md（SEC-01 ncnn 台账）");
  assert.match(md, /civetweb\/LEDGER\.md/, "账本应引用 src/win/third_party/civetweb/LEDGER.md（SEC-17）");
  assert.match(md, /SEC-15/, "账本应声明治理任务 SEC-15");
  assert.match(md, /S-SEC-B10-006/, "账本应声明治理缺口编号 S-SEC-B10-006");
});
