/**
 * @file check-plan-freshness.js
 * @brief 计划清单（`pending-only-*.md`）新鲜度自检（**本机手动工具，未接入 CI**）
 *
 * 背景：跨会话交接用的待办清单在 `~/.claude/plans/`（**不在 git 内**），靠人手工维护。
 * 它有两种独立的腐坏轴，本脚本只治**第一种**：
 *   1. **锚点漂移**（引用失准：文件被删、符号被改名、用例被删、行数变化）—— 本脚本能查；
 *   2. **状态过期**（断言变假：「从未执行」其实已做、「已闭合」其实只到部分闭合）—— **本脚本查不了**，
 *      因为那需要判断语义，属人工。⇒ 故本脚本**只报引用是否还在**。
 *
 * 它做三件事：
 *   1. 读清单头部的 `plan-freshness` 块，报 `verified_at_head..HEAD` 落后多少提交；
 *   2. 逐条解析各 `plan-anchors` 块（行格式 `ID | kind | payload | expect`）→ `OK` / `STALE` / `UNKNOWN`；
 *   3. 打印逐条结果 + 复核提示。
 *
 * 锚点类型（与清单 §0.2 的约定一致）：
 *   file:<path>                 文件存在（绝对路径用 fs，仓库相对路径用 git ls-files）
 *   sym:<file>#<Symbol>         该符号在该文件内出现（⚠️ 图谱对本仓某文件可能只部分抽取 ⇒ 回源为准）
 *   test:<用例名>               在 tests/** 内出现
 *   grep:<字面串>@<dir>[#expect=N]  命中数 == N；**无 expect ⇒ 只要求 > 0**
 *   noref:<符号>@<范围>          命中数 == 0（范围必填）
 *   sha:<短sha>                 存在，且是当前 HEAD 的祖先
 *   cmd:<命令>                   **不执行**（会跑任意命令）⇒ 一律报 UNKNOWN 并提示人工跑
 *
 * ⚠️ 为什么【不】放进 `tests/scripts/`：那里被 `repo-hygiene` job 以目录级 glob 整体接入 CI，
 *   而本脚本要读的清单在 git 外 ⇒ CI 全新检出后清单**不存在**，任何断言只会恒红或恒绿，纯噪声。
 *   故本脚本做成与 `scripts/check-graphify-freshness.js` / `scripts/check-raii-violations.sh`
 *   同类的「存在但未接入 CI」手动工具。**但它的纯函数被 tests/scripts/ 下的单测覆盖**（逻辑可测、清单不可测）。
 *
 * ⚠️ 退出码（刻意保守）：只有 0 / 1 两个码。
 *   - `1` **只来自执行性错误**（清单不存在 / 不是 git 仓库 / 顶层异常）；
 *   - **一切 STALE / UNKNOWN 结论都返回 0** —— 理由同 `check-graphify-freshness.js`：
 *     一个恒非零的退出码只会训练人忽略本脚本。「报得响一些」由**逐条结果表**承担，不靠退出码。
 *
 * 用法：
 *   node scripts/check-plan-freshness.js [--plan <abs path>] [--repo-root <dir>] [--quiet]
 */

"use strict";

const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const FRESHNESS_FENCE = "plan-freshness";
const ANCHORS_FENCE = "plan-anchors";

// ---------------------------------------------------------------- 纯函数（可单测）

/**
 * 抽出所有 ```<name> ... ``` 围栏块的内容（不解析散文）。
 * @param {string} text 清单全文
 * @param {string} fence 围栏标识（如 `plan-anchors`）
 * @returns {string[]} 每个块的内容（不含围栏行）
 */
function extractFencedBlocks(text, fence) {
  const out = [];
  const lines = String(text).split(/\r?\n/);
  let open = false;
  let buf = [];
  for (const line of lines) {
    const t = line.trim();
    if (!open) {
      if (t === "```" + fence) {
        open = true;
        buf = [];
      }
      continue;
    }
    if (t === "```") {
      out.push(buf.join("\n"));
      open = false;
      continue;
    }
    buf.push(line);
  }
  return out;
}

/**
 * 解析 `plan-freshness` 块 → `{ verified_at_head, verified_at_date }`（缺失字段为 null）。
 * @param {string} text 清单全文
 * @returns {{verified_at_head: string|null, verified_at_date: string|null}}
 */
function parseFreshnessBlock(text) {
  const blocks = extractFencedBlocks(text, FRESHNESS_FENCE);
  const meta = { verified_at_head: null, verified_at_date: null };
  if (blocks.length === 0) return meta;
  for (const line of blocks[0].split(/\r?\n/)) {
    const m = line.match(/^\s*([a-z_]+)\s*=\s*(\S+)\s*$/i);
    if (m && Object.prototype.hasOwnProperty.call(meta, m[1])) meta[m[1]] = m[2];
  }
  return meta;
}

/**
 * 按**最后一个** `@` 切分 `<literal>@<scope>[#expect=N]`。
 * 用最后一个 `@` 是因为字面串本身可能含 `@`（如邮箱、`as_of=` 形式的值）。
 * @param {string} payload
 * @returns {{literal: string, scope: string, expect: number|null}}
 */
function splitLiteralAtScope(payload) {
  let body = String(payload).trim();
  let expect = null;
  const em = body.match(/#expect=(\d+)\s*$/);
  if (em) {
    expect = Number(em[1]);
    body = body.slice(0, em.index).trim();
  }
  const at = body.lastIndexOf("@");
  if (at < 0) return { literal: body, scope: "", expect };
  return { literal: body.slice(0, at).trim(), scope: body.slice(at + 1).trim(), expect };
}

/**
 * 解析全部 `plan-anchors` 块 → `{id, kind, payload, expect, line}` 数组。
 * 忽略空行与 `#` 注释行；字段数不足 3 的行**不静默跳过**，而是作为 `kind:"__malformed__"` 返回（会被判 UNKNOWN）。
 * @param {string} text
 * @returns {{id: string, kind: string, payload: string, expect: number|null}[]}
 */
function parseAnchors(text) {
  const out = [];
  for (const block of extractFencedBlocks(text, ANCHORS_FENCE)) {
    for (const raw of block.split(/\r?\n/)) {
      const line = raw.trim();
      if (line === "" || line.startsWith("#")) continue;
      const parts = line.split("|").map((s) => s.trim());
      if (parts.length < 3 || parts[0] === "") {
        out.push({ id: line.slice(0, 40), kind: "__malformed__", payload: "", expect: null });
        continue;
      }
      const expect = parts[3] === undefined || parts[3] === "" ? null : Number(parts[3]);
      out.push({ id: parts[0], kind: parts[1], payload: parts[2], expect });
    }
  }
  return out;
}

/**
 * 把一个锚点判定为 OK / STALE / UNKNOWN。
 * @param {{kind: string, payload: string, expect: number|null}} a
 * @param {{grepCount: (literal: string, scope: string) => number|null,
 *          symbolInFile: (sym: string, file: string) => boolean|null,
 *          fileExists: (p: string) => boolean|null,
 *          shaIsAncestor: (sha: string) => boolean|null}} fns 注入的解析器（便于单测）
 * @returns {{verdict: string, detail: string}}
 */
function classifyAnchor(a, fns) {
  const kind = String(a.kind);
  if (kind === "__malformed__") return { verdict: "UNKNOWN", detail: "行格式错（需 `ID | kind | payload`）" };
  if (kind === "cmd") return { verdict: "UNKNOWN", detail: "cmd 锚点不自动执行，请人工跑" };

  if (kind === "file") {
    const r = fns.fileExists(a.payload);
    if (r === null) return { verdict: "UNKNOWN", detail: "无法判定（无 git 且非绝对路径）" };
    return r ? { verdict: "OK", detail: a.payload } : { verdict: "STALE", detail: "文件不存在： " + a.payload };
  }

  if (kind === "sym") {
    const at = a.payload.lastIndexOf("#");
    if (at < 0) return { verdict: "UNKNOWN", detail: "写法应为 <file>#<Symbol>" };
    const r = fns.symbolInFile(a.payload.slice(at + 1), a.payload.slice(0, at));
    if (r === null) return { verdict: "UNKNOWN", detail: "无法判定" };
    return r ? { verdict: "OK", detail: a.payload } : { verdict: "STALE", detail: "该文件内找不到该符号：" + a.payload };
  }

  if (kind === "test") {
    const n = fns.grepCount(a.payload, "tests");
    if (n === null) return { verdict: "UNKNOWN", detail: "无法判定" };
    return n > 0 ? { verdict: "OK", detail: "tests/ 命中 " + n } : { verdict: "STALE", detail: "tests/ 零命中：" + a.payload };
  }

  if (kind === "grep" || kind === "noref") {
    const { literal, scope, expect } = splitLiteralAtScope(a.payload);
    if (scope === "") return { verdict: "UNKNOWN", detail: "scope 必填（本仓硬拒绝无范围的 grep）" };
    const n = fns.grepCount(literal, scope);
    if (n === null) return { verdict: "UNKNOWN", detail: "无法判定" };
    const want = a.expect !== null ? a.expect : expect;
    if (kind === "noref") {
      return n === 0
        ? { verdict: "OK", detail: "零命中（符合预期）" }
        : { verdict: "STALE", detail: "期望 0 命中，实得 " + n + "：" + literal + " @" + scope };
    }
    if (want === null || Number.isNaN(want)) {
      return n > 0 ? { verdict: "OK", detail: "命中 " + n + "（>0 即通过）" } : { verdict: "STALE", detail: "零命中：" + literal };
    }
    return n === want
      ? { verdict: "OK", detail: "命中 " + n + "（== " + want + "）" }
      : { verdict: "STALE", detail: "期望 " + want + " 命中，实得 " + n + "：" + literal + " @" + scope };
  }

  if (kind === "sha") {
    const r = fns.shaIsAncestor(a.payload);
    if (r === null) return { verdict: "UNKNOWN", detail: "无法判定" };
    return r ? { verdict: "OK", detail: a.payload } : { verdict: "STALE", detail: "不是 HEAD 的祖先或不存在：" + a.payload };
  }

  return { verdict: "UNKNOWN", detail: "未知锚点类型：" + kind };
}

/**
 * 汇总：只统计可判定的；UNKNOWN 单列（**不得静默并入 OK**）。
 * @param {{verdict: string}[]} results
 * @returns {{ok: number, stale: number, unknown: number, total: number}}
 */
function summarize(results) {
  const s = { ok: 0, stale: 0, unknown: 0, total: results.length };
  for (const r of results) {
    if (r.verdict === "OK") s.ok += 1;
    else if (r.verdict === "STALE") s.stale += 1;
    else s.unknown += 1;
  }
  return s;
}

// ---------------------------------------------------------------- 实际解析器（依赖 git / fs）

/**
 * 跑 git（失败返回 null，不抛）。
 * @param {string} repoRoot
 * @param {string[]} args
 * @returns {{code: number, out: string}|null}
 */
function runGit(repoRoot, args) {
  try {
    const out = execFileSync("git", args, { cwd: repoRoot, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
    return { code: 0, out: String(out) };
  } catch (e) {
    if (typeof e.status === "number") return { code: e.status, out: String(e.stdout || "") };
    return null;
  }
}

/** git 可用？ */
function gitUsable(repoRoot) {
  const r = runGit(repoRoot, ["rev-parse", "--is-inside-work-tree"]);
  return r !== null && r.code === 0;
}

/**
 * 字面串在某范围内出现多少行（`git grep` 的 `-c` 是**每文件**计数 ⇒ 求和）。
 * @returns {number|null} null = 执行性错误（非「零命中」）
 */
function makeGrepCount(repoRoot) {
  return (literal, scope) => {
    const r = runGit(repoRoot, ["grep", "-c", "--fixed-strings", "-e", literal, "--", scope]);
    if (r === null) return null;
    if (r.code === 1) return 0; // 无匹配
    if (r.code !== 0) return null; // 128 等 = 路径不存在等执行性错误
    let n = 0;
    for (const line of r.out.split(/\r?\n/)) {
      if (line.trim() === "") continue;
      const c = Number(line.slice(line.lastIndexOf(":") + 1));
      if (!Number.isNaN(c)) n += c;
    }
    return n;
  };
}

/** 该符号是否在该文件内出现（字面串，非正则） */
function makeSymbolInFile(repoRoot) {
  return (sym, file) => {
    const r = runGit(repoRoot, ["grep", "-c", "--fixed-strings", "-e", sym, "--", file]);
    if (r === null) return null;
    if (r.code === 1) return false;
    if (r.code !== 0) return null;
    return r.out.trim() !== "";
  };
}

/** 文件存在（绝对路径 → fs；仓库相对路径 → git ls-files） */
function makeFileExists(repoRoot) {
  return (p) => {
    const s = String(p);
    if (/^[A-Za-z]:[\\/]/.test(s) || s.startsWith("/")) return fs.existsSync(path.normalize(s));
    if (gitUsable(repoRoot)) {
      const r = runGit(repoRoot, ["ls-files", "--error-unmatch", "--", s]);
      if (r === null) return null;
      if (r.code === 0) return true;
      if (r.code === 1) {
        // ls-files --error-unmatch 对未跟踪但存在的文件也返回 1 ⇒ 回退到 fs
        return fs.existsSync(path.join(repoRoot, s));
      }
      return null;
    }
    return fs.existsSync(path.join(repoRoot, s));
  };
}

/** sha 存在且是 HEAD 的祖先 */
function makeShaIsAncestor(repoRoot) {
  return (sha) => {
    const r = runGit(repoRoot, ["merge-base", "--is-ancestor", sha, "HEAD"]);
    if (r === null) return null;
    return r.code === 0;
  };
}

/** HEAD 与冻结 sha 的距离与关系 */
function diffHead(repoRoot, frozenSha) {
  const head = runGit(repoRoot, ["rev-parse", "HEAD"]);
  if (head === null || head.code !== 0) return { head: null, behind: null, relation: "UNKNOWN" };
  const headSha = head.out.trim();
  if (!frozenSha || frozenSha.length < 7) return { head: headSha, behind: null, relation: "NO_FROZEN_SHA" };
  const anc = runGit(repoRoot, ["merge-base", "--is-ancestor", frozenSha, "HEAD"]);
  if (anc === null) return { head: headSha, behind: null, relation: "UNKNOWN" };
  if (anc.code !== 0) return { head: headSha, behind: null, relation: "DIVERGED" };
  const cnt = runGit(repoRoot, ["rev-list", "--count", frozenSha + "..HEAD"]);
  if (cnt === null || cnt.code !== 0) return { head: headSha, behind: null, relation: "UNKNOWN" };
  return { head: headSha, behind: Number(cnt.out.trim()), relation: Number(cnt.out.trim()) === 0 ? "FRESH" : "STALE" };
}

// ---------------------------------------------------------------- CLI

function printHelp() {
  process.stdout.write(
    [
      "用法：node scripts/check-plan-freshness.js [--plan <abs path>] [--repo-root <dir>] [--quiet]",
      "",
      "  --plan       计划清单路径（默认 C:\\Users\\19842\\.claude\\plans\\pending-only-2026-09-25.md）",
      "  --repo-root  仓库根（默认脚本所在目录的父目录）",
      "  --quiet      只打印汇总，不打印逐条结果",
      "",
      "退出码：0 = 跑完（含存在 STALE/UNKNOWN）；1 = 执行性错误。",
      "",
    ].join("\n")
  );
}

function resolveDefaultPlan() {
  return "C:\\Users\\19842\\.claude\\plans\\pending-only-2026-09-25.md";
}

function main() {
  let planPath = resolveDefaultPlan();
  let repoRoot = path.resolve(__dirname, "..");
  let quiet = false;

  for (let i = 2; i < process.argv.length; i += 1) {
    const a = process.argv[i];
    if (a === "--plan" && process.argv[i + 1]) {
      planPath = process.argv[i + 1];
      i += 1;
    } else if (a === "--repo-root" && process.argv[i + 1]) {
      repoRoot = process.argv[i + 1];
      i += 1;
    } else if (a === "--quiet") {
      quiet = true;
    } else if (a === "--help" || a === "-h") {
      printHelp();
      process.exit(0);
    } else {
      process.stderr.write("未知参数：" + a + "\n");
      printHelp();
      process.exit(1);
    }
  }

  if (!fs.existsSync(planPath)) {
    process.stderr.write("PLAN_NOT_FOUND：" + planPath + "\n");
    process.exit(1);
  }

  const text = fs.readFileSync(planPath, "utf8");
  const meta = parseFreshnessBlock(text);
  const anchors = parseAnchors(text);
  const usable = gitUsable(repoRoot);
  const delta = usable ? diffHead(repoRoot, meta.verified_at_head) : { head: null, behind: null, relation: "NO_GIT" };

  const fns = {
    grepCount: usable ? makeGrepCount(repoRoot) : () => null,
    symbolInFile: usable ? makeSymbolInFile(repoRoot) : () => null,
    fileExists: makeFileExists(repoRoot),
    shaIsAncestor: usable ? makeShaIsAncestor(repoRoot) : () => null,
  };

  const results = anchors.map((a) => Object.assign({}, a, classifyAnchor(a, fns)));
  const s = summarize(results);

  const lines = [];
  lines.push("=== 计划清单新鲜度自检 ===");
  lines.push("清单：" + planPath);
  lines.push("冻结：" + (meta.verified_at_head || "(未声明)") + "  @ " + (meta.verified_at_date || "(未声明)"));
  lines.push("当前 HEAD：" + (delta.head || "(不可用)"));
  if (delta.relation === "FRESH") lines.push("⇒ 与冻结点**一致**（落后 0 个提交）");
  else if (delta.relation === "STALE") lines.push("⇒ **落后 " + delta.behind + " 个提交** —— 请先看下面的 STALE 条目");
  else lines.push("⇒ 关系：" + delta.relation + "（无法判定落后数）");

  if (!usable) lines.push("⚠️ 本目录不是 git 仓库（或 git 不可用）⇒ 除绝对路径/fs 判据外一律 UNKNOWN");

  if (!quiet) {
    lines.push("");
    lines.push("--- 逐条锚点 ---");
    for (const r of results) {
      const mark = r.verdict === "OK" ? "OK   " : r.verdict === "STALE" ? "STALE" : "UNKWN";
      lines.push("[" + mark + "] " + r.id + "  " + r.kind + "  " + r.detail);
    }
  }

  lines.push("");
  lines.push(
    "--- 汇总 --- 总计 " + s.total + " 条：OK " + s.ok + " / STALE " + s.stale + " / UNKNOWN " + s.unknown
  );
  lines.push("⚠️ 块内全 OK **不等于**清单正确：本脚本只判「引用还在」，");
  lines.push("   不判「这条还该不该做」「某行为是否真有覆盖」——后者永远是人工。");
  if (s.stale > 0) lines.push("⇒ 有 " + s.stale + " 条引用已失准：这些条目的**状态在人工复核前视为不可信**。");
  if (s.unknown > 0) lines.push("⇒ 有 " + s.unknown + " 条无法判定（cmd 锚点 / 无 git / 写法错）：请人工核对。");

  process.stdout.write(lines.join("\n") + "\n");
  process.exit(0);
}

if (require.main === module) {
  try {
    main();
  } catch (e) {
    process.stderr.write(String(e && e.stack ? e.stack : e) + "\n");
    process.exit(1);
  }
}

module.exports = {
  FRESHNESS_FENCE,
  ANCHORS_FENCE,
  extractFencedBlocks,
  parseFreshnessBlock,
  splitLiteralAtScope,
  parseAnchors,
  classifyAnchor,
  summarize,
};
