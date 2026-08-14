/**
 * @file check-workflow-pins.js
 * @brief GitHub Actions 可移动引用静态门禁（SEC-14 / S-SEC-B10-005）
 *
 * 扫描 .github/workflows/ 下的所有 YAML，提取每个 step 的 uses: 引用，断言
 * 所有远程 GitHub Action 均以完整 40 位十六进制 commit SHA 固定：
 *
 *   1. `owner/repo@<40hex>` —— 完整 SHA，不可移动（通过，配合注释保留 tag 可读性）；
 *   2. `owner/repo@v4` / `owner/repo@main` / 短 SHA —— 可移动引用（失败），
 *      上游 tag/分支被改写或仓库被攻破时，CI 每次运行拉取到不同代码，无法复现；
 *   3. `docker://镜像` 与 `./本地 Action` 不属于 git 远程 Action，跳过。
 *
 * 校验只做正则/长度，不发起任何网络请求（离线可跑，供 CI 门禁直接调用）。
 * 退出码：0=全部固定，2=存在可移动引用，1=执行出错。
 */
"use strict";

const fs = require("fs");
const path = require("path");

const FULL_SHA_RE = /^[0-9a-f]{40}$/;

/** 匹配一行真实 uses: 键（含引号键与列表项前缀 `- uses:`），值取首个空白分隔 token，容忍行尾 `# tag` 注释 */
const USES_LINE_RE = /^\s*(?:-\s*)?(?:["'])?uses(?:["'])?\s*:\s*(\S+)(?:\s+.*)?$/;

/**
 * 判断某行的首 token 是否是块标量开始（key: | / key: >，含 chomp 与显式缩进指示符）。
 * 块标量内的后续行不是 YAML 键，需跳过 uses 匹配（防 run 块里的字符串被误判）。
 * @param {string} trimmed 去首尾空白的行
 * @returns {boolean}
 */
function isBlockScalarStart(trimmed) {
  return /^[\w.\- ]+:\s*[|>][-+]?(?:\d+)?\s*(?:#.*)?$/.test(trimmed);
}

/**
 * 从一行提取 uses 引用值；非 uses 键或值不完整时返回 null。
 * @param {string} line
 * @returns {string|null}
 */
function parseUsesValue(line) {
  const m = line.match(USES_LINE_RE);
  return m ? m[1] : null;
}

/**
 * 把 uses 值拆分为 owner/repo 与 ref；返回引用形态。
 * @param {string} value 如 "actions/checkout@v4" / "docker://alpine:3.18" / "./local/action"
 * @returns {{kind:string, ownerRepo:string|null, ref:string|null}}
 *   kind: "action"（远程 GitHub Action）| "docker" | "local" | "no-ref"
 */
function splitUsesValue(value) {
  if (/^docker:\/\//i.test(value)) return { kind: "docker", ownerRepo: null, ref: null };
  if (/^\.{1,2}[\\/]/.test(value)) return { kind: "local", ownerRepo: null, ref: null };
  const at = value.lastIndexOf("@");
  if (at < 0) return { kind: "no-ref", ownerRepo: value, ref: null };
  return { kind: "action", ownerRepo: value.slice(0, at), ref: value.slice(at + 1) };
}

/**
 * 判定 ref 是否完整固定。
 * @param {string} ref @ 之后的引用串
 * @returns {string} "full-sha" | "short-sha" | "mutable"
 */
function classifyRef(ref) {
  if (FULL_SHA_RE.test(ref)) return "full-sha";
  // 1~39 位 hex：GitHub 仅认完整 40 位 SHA 为不可移动引用，短 SHA 视为未固定
  if (/^[0-9a-f]{1,39}$/i.test(ref)) return "short-sha";
  return "mutable";
}

/**
 * 扫描单个 workflow 源文本，返回所有 uses 引用条目（含行号与形态判定）。
 * 简单的行级扫描：跟踪块标量上下文避免误判 run 块内的文本。
 * @param {string} src YAML 源文本
 * @returns {Array<{line:number, value:string, kind:string, ownerRepo:string|null, ref:string|null, verdict:string}>}
 */
function scanWorkflow(src) {
  const entries = [];
  const lines = String(src).split(/\r?\n/);
  let blockScalarIndent = -1; // 块标量内容缩进下限；>=0 表示当前行处于块标量内
  for (let i = 0; i < lines.length; i += 1) {
    const raw = lines[i];
    const indent = (raw.match(/^ */) || [""])[0].length;
    const trimmed = raw.trim();
    if (trimmed === "") continue;

    if (blockScalarIndent >= 0) {
      // 空行已在上面跳过；只有缩进超过块标量起始行才仍算标量内容
      if (indent > blockScalarIndent) continue;
      blockScalarIndent = -1; // 缩进回落，退出块标量上下文
    }

    if (isBlockScalarStart(trimmed)) {
      blockScalarIndent = indent;
      continue;
    }

    const value = parseUsesValue(raw);
    if (value === null) continue;

    const parts = splitUsesValue(value);
    // refKind 保留引用形态（full-sha / short-sha / mutable / docker / local / no-ref）
    const refKind =
      parts.kind === "action"
        ? classifyRef(parts.ref)
        : parts.kind;
    // verdict 归一化为门禁判定：仅 "mutable"（含短 SHA 与无 @ 引用）被标记为失败，
    // full-sha / docker / local 均视为通过，避免判定集合漂移。
    let verdict = "ok";
    if (parts.kind === "action" && refKind !== "full-sha") verdict = "mutable";
    if (parts.kind === "no-ref") verdict = "mutable";
    entries.push({ line: i + 1, value, kind: parts.kind, ownerRepo: parts.ownerRepo, ref: parts.ref, refKind, verdict });
  }
  return entries;
}

/** 列出 .github/workflows 下的 *.yml / *.yaml 文件 */
function collectWorkflowFiles(repoRoot) {
  const wfDir = path.join(repoRoot, ".github", "workflows");
  if (!fs.existsSync(wfDir)) return [];
  return fs
    .readdirSync(wfDir)
    .filter((f) => /\.ya?ml$/i.test(f))
    .map((f) => path.join(wfDir, f))
    .sort();
}

/**
 * 审计整个 workflow 目录。
 * @returns {Promise<{files:string[], entries:Array, mutable:Array}>}
 */
async function auditWorkflowDir(repoRoot) {
  const files = collectWorkflowFiles(repoRoot)
    .map((f) => path.relative(repoRoot, f).split("\\").join("/"))
    .sort();
  const entries = [];
  for (const rel of files) {
    const abs = path.join(repoRoot, rel);
    const src = await fs.promises.readFile(abs, "utf8");
    for (const e of scanWorkflow(src)) {
      entries.push({ file: rel, ...e });
    }
  }
  const mutable = entries.filter((e) => e.verdict === "mutable");
  return { files, entries, mutable };
}

function printHelp() {
  process.stdout.write(
    "用法: node scripts/check-workflow-pins.js [选项]\n" +
      "扫描 .github/workflows/ 下所有 workflow 的 uses: 引用，断言远程 Action 均以完整 40 位 SHA 固定。\n" +
      "  --repo-root <dir>   仓库根目录（默认脚本上级目录）\n" +
      "  --help / -h         显示帮助\n" +
      "退出码：0=全部固定，2=存在可移动引用，1=执行出错\n"
  );
}

async function main() {
  let repoRoot = "";
  for (let i = 2; i < process.argv.length; i += 1) {
    const a = process.argv[i];
    if (a === "--repo-root" && process.argv[i + 1]) {
      repoRoot = process.argv[i + 1];
      i += 1;
    } else if (a === "--help" || a === "-h") {
      printHelp();
      process.exit(0);
    }
  }
  const root = path.resolve(repoRoot || path.join(__dirname, ".."));
  const { files, entries, mutable } = await auditWorkflowDir(root);

  const actionCount = entries.filter((e) => e.kind === "action").length;
  process.stdout.write(
    `WORKFLOW_PINS: ${mutable.length === 0 ? "ALL_PINNED" : "MUTABLE_FOUND"} ` +
      `files=${files.length} actions=${actionCount} mutable=${mutable.length}\n`
  );
  for (const m of mutable) {
    const desc = m.kind === "action" ? `可移动引用 @${m.ref}` : `未固定引用`;
    process.stdout.write(`  [MUTABLE] ${m.file}:${m.line} ${m.value} — ${desc}\n`);
  }

  process.exit(mutable.length === 0 ? 0 : 2);
}

if (require.main === module) {
  main().catch((e) => {
    process.stderr.write(String(e && e.stack ? e.stack : e) + "\n");
    process.exit(1);
  });
}

module.exports = { parseUsesValue, splitUsesValue, classifyRef, scanWorkflow, collectWorkflowFiles, auditWorkflowDir };
