const fs = require("fs");
const fsp = require("fs/promises");
const path = require("path");
const crypto = require("crypto");
const { execSync } = require("child_process");

function nowIsoLocal() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}` +
    `T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  );
}

function safeExec(cmd) {
  try {
    return String(execSync(cmd, { stdio: ["ignore", "pipe", "pipe"] })).trim();
  } catch (e) {
    const out = e && e.stdout ? String(e.stdout) : "";
    const err = e && e.stderr ? String(e.stderr) : "";
    return (out + "\n" + err).trim();
  }
}

function parseArgs(argv) {
  const args = {
    fix: false,
    outDir: "tests/reports/docs-sync-audit",
    json: true,
    md: true,
    spacingSampleLimit: 12,
    bspReleaseNotes: "docs/bsp/BSP_EVIDENCE_STATUS.md",
    runMarkdownlintCli2: false,
    skipLinks: false,
    linkConcurrency: 6,
    linkCache: "",
    linkCacheTtl: 86400,
    skipCheckPaths: [],
  };
  for (let i = 2; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === "--fix") args.fix = true;
    else if (a === "--out-dir" && argv[i + 1]) {
      args.outDir = argv[i + 1];
      i += 1;
    } else if (a === "--no-json") args.json = false;
    else if (a === "--no-md") args.md = false;
    else if (a === "--spacing-samples" && argv[i + 1]) {
      args.spacingSampleLimit = Math.max(0, Number(argv[i + 1]) || 0);
      i += 1;
    } else if (a === "--bsp-release-notes" && argv[i + 1]) {
      args.bspReleaseNotes = argv[i + 1];
      i += 1;
    } else if (a === "--markdownlint-cli2") args.runMarkdownlintCli2 = true;
    else if (a === "--skip-links") args.skipLinks = true;
    else if (a === "--link-concurrency" && argv[i + 1]) {
      args.linkConcurrency = Math.max(1, Number(argv[i + 1]) || 1);
      i += 1;
    } else if (a === "--link-cache" && argv[i + 1]) {
      args.linkCache = argv[i + 1];
      i += 1;
    } else if (a === "--link-cache-ttl" && argv[i + 1]) {
      args.linkCacheTtl = Math.max(0, Number(argv[i + 1]) || 86400);
      i += 1;
    } else if (a === "--skip-check-paths" && argv[i + 1]) {
      // ⚠️ 这是整体替换而非追加，且会同时关闭 version 与 refpath 两条检查。
      // 它不是常规开关：CI 命令行中被断言不得出现该参数（见
      // tests/scripts/docs-sync-audit-gate.test.js），仅本地排查时使用。
      args.skipCheckPaths = argv[i + 1].split(",").map(s => s.trim()).filter(Boolean);
      i += 1;
    }
  }
  return args;
}

// 文档滞后基准的「代码路径」集合：文档描述的就是这些内容，只有它们变化才可能让文档陈旧。
// 不能用 `git log --merges` 作基准——本项目用 squash merge，master 上的 merge commit
// 只来自偶发的 `git pull`，基准会长期冻结（2026-09 曾因此产生 45 个假阳性 high 缺陷）。
const CODE_BASELINE_PATHS = [
  "src/",
  "app/",
  "web/src/",
  "tests/",
  "scripts/",
  "cmake/",
  ".github/",
  "gradle/",
  "config/",
  "CMakeLists.txt",
];

// 时间戳只能提示「该复核了」，无法证明内容陈旧（改一个字符即可让文档「通过」），
// 因此 version 缺陷定为 low：进报告作为复核清单，但不阻断门禁。
const VERSION_LAG_THRESHOLD_DAYS = 7;

// refpath 检查只认这些顶层前缀，避免把 `try/catch`、`actions/setup-java` 之类
// 误判成仓库路径而制造噪声。
const REPO_REF_PREFIXES = [
  "src/", "app/", "web/", "docs/", "tests/", "scripts/",
  "cmake/", ".github/", "config/", "models/", "deps/", "patterns/",
];

// 不要求文档元数据的「非项目文档」：第三方内容与工具配置（规范见 docs/DOC_STANDARD.md §1.4）。
// 这是唯一保留的按路径豁免概念 —— 一份文档是否豁免「跟随代码更新」的检查，不再由它
// 放在哪个目录决定，而由它头部的 status 自述决定（见 isLivingDoc）。
// 目录前缀条目以 `/` 结尾；文件级条目（不以 `/` 结尾）由 auditDocMetaExemptList 自检存在性，
// 防止清单里再长出指向已删除文件的死条目（历史上有两条根级工作台账已不存在）。
const DOC_META_EXEMPT_PATTERNS = [
  "deps/",
  "src/win/third_party/",
  ".claude/agents/",
];

// 「例外优先」于整树豁免：`src/win/third_party/**` 整树豁免，但本仓自撰的台账
// `civetweb/LEDGER.md` 仍需元数据块（docs/DOC_STANDARD.md §1.4 明确点名，type=security）。
const DOC_META_EXEMPT_UNLESS = [
  "src/win/third_party/civetweb/LEDGER.md",
];

// 文档元数据块（规范见 docs/DOC_STANDARD.md §1）。
// 它是「这份文档是什么 / 是否还需跟随代码」的唯一权威声明。此前的做法是按目录豁免
// （TIMEPOINT_DOC_PATTERNS）——把文档挪进 docs/research/ 即可免检，惩罚为 0，
// 是个实际生效的后门；改由自述决定后，豁免变成一处 review 时可见的声明。
const DOC_META_START = "<!-- DOCSYNC_DOC";
const DOC_META_TYPES = new Set([
  "guide",
  "architecture",
  "design",
  "plan",
  "audit",
  "research",
  "security",
  "runbook",
  "reference",
  "process",
]);
const DOC_META_STATUSES = new Set(["living", "snapshot", "frozen", "placeholder"]);

// 元数据块必须位于文件前 DOC_META_HEAD_LINES 行内（docs/DOC_STANDARD.md §1.3）。
const DOC_META_HEAD_LINES = 20;

// 取全文前 n 行的原始前缀（保留原换行），使 indexOf 偏移与全文对齐
// （若按换行符分割再拼接会归一化 CRLF，使偏移错位）。
function headLinesPrefix(md, n) {
  let end = 0;
  for (let i = 0; i < n; i += 1) {
    const nl = md.indexOf("\n", end);
    if (nl < 0) return md;
    end = nl + 1;
  }
  return md.slice(0, end);
}

// 只在前 DOC_META_HEAD_LINES 行内查找起始标记（此前是全文 indexOf，把块藏在正文
// 任意位置也算通过，与 §1.3「必须在文件最顶部、前 20 行内」不符）。闭合符仍在全文
// 查找，容忍块尾越过第 20 行。
function parseDocMeta(md) {
  const head = headLinesPrefix(md, DOC_META_HEAD_LINES);
  const start = head.indexOf(DOC_META_START);
  if (start < 0) return null;
  const end = md.indexOf("-->", start);
  if (end < 0) return null;
  const body = md.slice(start + DOC_META_START.length, end);
  const meta = {};
  for (const line of body.split(/\r?\n/)) {
    const m = line.trim().match(/^([a-z0-9_]+)\s*=\s*(.+)$/i);
    if (m) meta[m[1]] = m[2].trim();
  }
  return meta;
}

// 非围栏文本内 DOCSYNC_DOC 起始标记的数量与首个所在行号（1-based，未出现为 -1）。
// 围栏内的示例块（如 docs/DOC_STANDARD.md §1 自身的示范）不计入，否则规格文档会误报「多个块」。
function analyzeDocMetaMarkers(md) {
  let count = 0;
  let firstLine = -1;
  let lineNo = 1;
  for (const block of splitMarkdownBlocks(md)) {
    if (block.type !== "text") {
      lineNo += block.lines.length;
      continue;
    }
    for (const line of block.lines) {
      if (line.includes(DOC_META_START)) {
        count += 1;
        if (firstLine < 0) firstLine = lineNo;
      }
      lineNo += 1;
    }
  }
  return { count, firstLine };
}

// 回写比对校验日历日期：本机 node 下 `new Date(Date.UTC(2026,1,31))` 会 rollover 成
// 3-03 而非 Invalid Date，单看 Date.parse 是否 NaN 会把 2026-02-31 当成合法 —— 必须
// 逐项回写年月日比对。
function isCalendarDate(value) {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(value || ""));
  if (!m) return false;
  const y = Number(m[1]);
  const mo = Number(m[2]);
  const d = Number(m[3]);
  const dt = new Date(Date.UTC(y, mo - 1, d));
  return dt.getUTCFullYear() === y && dt.getUTCMonth() === mo - 1 && dt.getUTCDate() === d;
}

// 该文件是否属于「需要文档元数据守卫」的项目文档。
// 例外优先：整树豁免里被 §1.4 点名的自撰台账仍要求元数据。
function requiresDocMeta(rel) {
  if (shouldSkipVersionPath(rel, DOC_META_EXEMPT_UNLESS)) return true;
  return !shouldSkipVersionPath(rel, DOC_META_EXEMPT_PATTERNS);
}

// 活文档 = 声明 status=living。缺少元数据时保守地按 living 处理（缺失本身由
// doc-meta 规则单独报错），避免一个坏掉的块顺带关掉两条检查。
function isLivingDoc(md) {
  const meta = parseDocMeta(md);
  if (!meta) return true;
  return (meta.status || "living") === "living";
}

// 元数据块校验：存在、唯一、位于前 20 行、type/status 在闭集内、as_of 为合法日历日期。
function auditDocMeta(rel, md) {
  const defects = [];
  const push = (message) => defects.push({ type: "doc-meta", severity: "high", file: rel, message });
  const { count: markerCount, firstLine } = analyzeDocMetaMarkers(md);
  if (markerCount === 0) {
    push("缺少 DOCSYNC_DOC 元数据块（规范见 docs/DOC_STANDARD.md）");
    return defects;
  }
  if (markerCount > 1) {
    push(`DOCSYNC_DOC 元数据块出现 ${markerCount} 次，只能有一个（规范见 docs/DOC_STANDARD.md §1.3）`);
  }
  if (firstLine > DOC_META_HEAD_LINES) {
    push(`DOCSYNC_DOC 元数据块位于第 ${firstLine} 行，须在前 ${DOC_META_HEAD_LINES} 行内（规范见 docs/DOC_STANDARD.md §1.3）`);
  }
  const meta = parseDocMeta(md);
  if (!meta) {
    // 标记存在却解析不出块：只可能是块未闭合（起始行在前 20 行内时）。
    if (firstLine > 0 && firstLine <= DOC_META_HEAD_LINES) {
      push("DOCSYNC_DOC 元数据块未闭合（规范见 docs/DOC_STANDARD.md §1.3）");
    }
    return defects;
  }
  if (!DOC_META_TYPES.has(meta.type)) {
    push(`元数据 type 非法：${meta.type || "缺失"}（合法值：${[...DOC_META_TYPES].join(" / ")}）`);
  }
  if (!DOC_META_STATUSES.has(meta.status)) {
    push(`元数据 status 非法：${meta.status || "缺失"}（合法值：${[...DOC_META_STATUSES].join(" / ")}）`);
  }
  if (!isCalendarDate(meta.as_of)) {
    push(`元数据 as_of 非法：${meta.as_of || "缺失"}（需 YYYY-MM-DD 且为日历上存在的日期）`);
  }
  return defects;
}

// 豁免清单自检：文件级条目（不以 `/` 结尾者）必须真实存在，否则是死条目。
// 目录前缀条目（以 `/` 结尾）不检查存在性（目录可能按需创建）。
//
// 默认入参含 DOC_META_EXEMPT_UNLESS 的文件级条目（如 civetweb/LEDGER.md）——它们才是
// 文件级的、真正需要防「指向已删文件」的那批。按当前清单，DOC_META_EXEMPT_PATTERNS
// 的项全部以 `/` 结尾（前瞻性守卫，不参与存在性自检），故实际被检的是 UNLESS 的文件级项。
function auditDocMetaExemptList(repoRoot, patterns = [...DOC_META_EXEMPT_PATTERNS, ...DOC_META_EXEMPT_UNLESS]) {
  const defects = [];
  for (const raw of patterns) {
    const pattern = String(raw || "");
    if (!pattern || pattern.endsWith("/")) continue;
    const rel = toPosix(pattern);
    if (!fs.existsSync(path.resolve(repoRoot, rel))) {
      defects.push({
        type: "doc-meta-exempt",
        severity: "low",
        file: "scripts/docs-sync-audit.js",
        message: `元数据豁免清单的文件级条目不存在（死条目）：${pattern}`,
      });
    }
  }
  return defects;
}

// 索引里被引用的 .md 路径（反引号与 markdown 链接两种写法），按 docs/ 相对解析。
// 裸文件名只在它确实指向 docs/ 下的文件时才计入 —— 否则索引里顺带提到的
// `.claude/agents/android.md`、`FULL_PROJECT_AUDIT_….md` 之类会被误判成 docs/android.md。
function collectIndexEntries(indexMd, repoRoot) {
  const raw = new Set();
  let m;
  const tickRe = /`([^`]+)`/g;
  while ((m = tickRe.exec(indexMd))) raw.add(m[1].trim());
  const linkRe = /\[[^\]]*]\(([^)]+)\)/g;
  while ((m = linkRe.exec(indexMd))) {
    const href = m[1].trim();
    if (!/^https?:/i.test(href)) raw.add(href.replace(/#.*$/, ""));
  }
  const resolved = new Set();
  for (const r of raw) {
    if (!/\.md$/i.test(r)) continue;
    const rel = toPosix(path.posix.normalize(path.posix.join("docs", r)));
    const bare = !r.includes("/");
    if (bare && !fs.existsSync(path.resolve(repoRoot, rel))) continue;
    resolved.add(rel);
  }
  return resolved;
}

// 索引完整性（双向差集）：漏登 → high；索引条目指向不存在的文件 → medium。
// docs/index.md 此前自称受门禁保护，实际上完全不受检查——这条规则让它名副其实。
function auditDocIndex(indexMd, repoRoot, docFiles) {
  const defects = [];
  const indexed = collectIndexEntries(indexMd, repoRoot);
  for (const rel of docFiles) {
    if (!requiresDocMeta(rel)) continue;
    if (!indexed.has(rel)) {
      defects.push({
        type: "doc-index",
        severity: "high",
        file: rel,
        message: "未登记进 docs/index.md（规范见 docs/DOC_STANDARD.md §5）",
      });
    }
  }
  for (const rel of indexed) {
    if (fs.existsSync(path.resolve(repoRoot, rel))) continue;
    defects.push({
      type: "doc-index",
      severity: "medium",
      file: "docs/index.md",
      message: `索引条目指向不存在的文件：${rel}`,
    });
  }
  return defects;
}

const AGENT_GUIDE_FACTS_START = "<!-- DOCSYNC_AGENT_GUIDE_FACTS";
const AGENT_GUIDE_SHARED_START = "<!-- DOCSYNC_SHARED_GUIDE_START -->";
const AGENT_GUIDE_SHARED_END = "<!-- DOCSYNC_SHARED_GUIDE_END -->";

function parseAgentGuideFacts(md) {
  const start = md.indexOf(AGENT_GUIDE_FACTS_START);
  if (start < 0) return null;
  const end = md.indexOf("-->", start);
  if (end < 0) return null;
  const facts = {};
  const body = md.slice(start + AGENT_GUIDE_FACTS_START.length, end);
  for (const line of body.split(/\r?\n/)) {
    const match = line.trim().match(/^([a-z0-9_]+)\s*=\s*(.+)$/i);
    if (match) facts[match[1]] = match[2].trim();
  }
  return facts;
}

function extractSharedGuide(md) {
  const start = md.indexOf(AGENT_GUIDE_SHARED_START);
  const end = md.indexOf(AGENT_GUIDE_SHARED_END);
  if (start < 0 || end < 0 || end < start) return null;
  return md.slice(start + AGENT_GUIDE_SHARED_START.length, end).trim();
}

function extractWorkflowJobBlock(yaml, jobName) {
  const lines = yaml.split(/\r?\n/);
  const start = lines.findIndex((line) => new RegExp(`^  ${jobName}:\\s*$`).test(line));
  if (start < 0) return "";
  const block = [];
  for (let i = start + 1; i < lines.length; i += 1) {
    if (/^  [A-Za-z0-9_-]+:\s*$/.test(lines[i])) break;
    block.push(lines[i]);
  }
  return block.join("\n");
}

function detectAgentGuideFacts(repoRoot) {
  const read = (relativePath) => {
    const fullPath = path.join(repoRoot, relativePath);
    return fs.existsSync(fullPath) ? fs.readFileSync(fullPath, "utf8") : "";
  };
  const wrapper = read("gradle/wrapper/gradle-wrapper.properties");
  const workflow = read(".github/workflows/ci.yml");
  const androidJob = extractWorkflowJobBlock(workflow, "android");
  const windowsJob = extractWorkflowJobBlock(workflow, "windows");
  const gradleMatch = wrapper.match(/distributions\/(gradle-[0-9][A-Za-z0-9.-]*)-bin\.zip/);
  const javaMatch = androidJob.match(/java-version:\s*['\"]?([0-9]+)['\"]?/);
  const customRunners = [
    "tests/cpp/face_infer_unit_tests_main.cpp",
    "tests/win/win_unit_tests_main.cpp",
    "tests/cpp/ncnn_precision_test.cpp",
  ];
  // 这是仓库内框架的集合事实，单个工具改为独立 main 不会消除其他 bool 运行器。
  const hasCustomBool = customRunners.some((file) => /TestCase/.test(read(file)));
  const coreUnitMain = read("tests/cpp/core_unit_tests_main.cpp");
  const hasGoogleTest = /#include\s*<gtest\/gtest\.h>/.test(coreUnitMain) &&
    /RUN_ALL_TESTS\s*\(/.test(coreUnitMain) &&
    /add_executable\(core_gtest_tests/.test(read("CMakeLists.txt"));

  return {
    gradle_wrapper: gradleMatch ? gradleMatch[1] : "unknown",
    android_ci_java: javaMatch ? javaMatch[1] : "unknown",
    windows_ci_events: /github\.event_name\s*==\s*['"]pull_request['"]/.test(windowsJob) &&
      /github\.event_name\s*==\s*['"]push['"]/.test(windowsJob)
      ? "pull_request,push"
      : "unknown",
    windows_ci_workflow_dispatch: windowsJob.includes("workflow_dispatch") ? "true" : "false",
    test_frameworks: hasCustomBool && hasGoogleTest ? "custom_bool,googletest" : "unknown",
    merge_strategy: "squash_and_merge",
  };
}

function guideDefect(file, rule, message) {
  return { type: "agent-guide", severity: "high", file, rule, message };
}

function auditAgentGuides(repoRoot) {
  const expectedFacts = detectAgentGuideFacts(repoRoot);
  const guides = ["AGENTS.md", "CLAUDE.md"];
  const defects = [];
  const parsed = {};
  const shared = {};

  for (const file of guides) {
    const fullPath = path.join(repoRoot, file);
    if (!fs.existsSync(fullPath)) {
      defects.push(guideDefect(file, "agent_guide_missing", "缺少助手指南文件"));
      continue;
    }
    const md = fs.readFileSync(fullPath, "utf8");
    parsed[file] = parseAgentGuideFacts(md);
    shared[file] = extractSharedGuide(md);
    if (!parsed[file]) {
      defects.push(guideDefect(file, "agent_guide_facts_missing", "缺少 DOCSYNC_AGENT_GUIDE_FACTS 事实块"));
      continue;
    }
    for (const [key, expected] of Object.entries(expectedFacts)) {
      if (parsed[file][key] !== expected) {
        defects.push(guideDefect(file, "agent_guide_fact_drift", `事实 ${key} 应为 ${expected}，当前为 ${parsed[file][key] || "缺失"}`));
      }
    }
    if (shared[file] === null) {
      defects.push(guideDefect(file, "agent_guide_shared_missing", "缺少 DOCSYNC_SHARED_GUIDE_START/END 共享区间"));
    }
  }

  if (shared["AGENTS.md"] !== undefined && shared["CLAUDE.md"] !== undefined &&
      shared["AGENTS.md"] !== null && shared["CLAUDE.md"] !== null &&
      sha256Hex(shared["AGENTS.md"]) !== sha256Hex(shared["CLAUDE.md"])) {
    defects.push(guideDefect("CLAUDE.md", "agent_guide_mirror_drift", "共享指南正文与 AGENTS.md 不一致"));
  }

  return { expectedFacts, defects };
}

function toPosix(p) {
  return p.replace(/\\/g, "/");
}

function isHttpUrl(s) {
  return /^https?:\/\/\S+$/i.test(s);
}

function sha256Hex(s) {
  return crypto.createHash("sha256").update(String(s)).digest("hex");
}

function stripInlineCode(text) {
  const parts = [];
  let inCode = false;
  let cur = "";
  for (let i = 0; i < text.length; i += 1) {
    const ch = text[i];
    if (ch === "`") {
      parts.push({ t: cur, code: inCode });
      cur = "";
      inCode = !inCode;
      continue;
    }
    cur += ch;
  }
  parts.push({ t: cur, code: inCode });
  return parts.filter((p) => !p.code).map((p) => p.t).join("");
}

function splitMarkdownBlocks(md) {
  const lines = md.split(/\r?\n/);
  const blocks = [];
  let i = 0;
  let inFence = false;
  let fence = "```";
  let fenceLang = "";
  let cur = [];
  while (i < lines.length) {
    const line = lines[i];
    if (!inFence) {
      const m = line.match(/^(```+)(\s*)(.*)$/);
      if (m) {
        if (cur.length > 0) {
          blocks.push({ type: "text", lines: cur });
          cur = [];
        }
        inFence = true;
        fence = m[1];
        fenceLang = String(m[3] || "").trim();
        cur.push(line);
      } else {
        cur.push(line);
      }
      i += 1;
      continue;
    }
    cur.push(line);
    if (line.startsWith(fence)) {
      blocks.push({ type: "code", lines: cur, fence, fenceLang });
      cur = [];
      inFence = false;
      fence = "```";
      fenceLang = "";
    }
    i += 1;
  }
  if (cur.length > 0) blocks.push({ type: inFence ? "code" : "text", lines: cur, fence, fenceLang });
  return blocks;
}

function extractHeadUpdateTime(md) {
  // 只认文档头部（前 60 行）的元数据，正文里出现的日期不算。
  // 先归一化：剥掉粗体标记、行首引用符与列表符号，使 `**更新日期**:`、`> **最后复核**:`、
  // `- 最后复核：` 等写法与纯文本写法走同一条正则（此前 `[\s]*[:：]` 被 `**` 卡住，
  // 导致仓库里 15 个文件写的是 `**更新日期**: YYYY-MM-DD` 却完全不被识别）。
  const head = md
    .split(/\r?\n/)
    .slice(0, 60)
    .map((l) => l.replace(/\*\*/g, "").replace(/^\s*[>-]\s*/, ""))
    .join("\n");
  const labels = ["最后更新", "更新时间", "更新日期", "最后复核"];
  const patterns = [
    ...labels.map((l) => new RegExp(`${l}[ \\t]*[:：][ \\t]*([0-9]{4}-[0-9]{2}-[0-9]{2})`, "i")),
    /last\s+updated[ \t]*[:：][ \t]*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
  ];
  // 命中优先级 = 标签在 labels 中的顺序；同一头部含多个标签时取先列出的那个（确定性）。
  for (const re of patterns) {
    const m = head.match(re);
    if (m) {
      const [y, mo, d] = m[1].split("-").map(Number);
      // 用 UTC 解释日期：基准时间戳来自 git（UTC），此处若按本地时区解释会产生
      // 恒定的 8 小时偏差，使同一文档在 Windows 本地与 ubuntu CI 上判定不一致。
      return Math.floor(Date.UTC(y, mo - 1, d) / 1000);
    }
  }
  return null;
}

function getGitFileLastCommitTs(filePath) {
  const p = toPosix(filePath);
  const out = safeExec(`git log -n 1 --format=%ct -- "${p}"`);
  const n = Number(out);
  return Number.isFinite(n) && n > 0 ? n : null;
}

function getMainBranchName() {
  const hasMain = safeExec("git rev-parse --verify main").includes("fatal") ? false : true;
  if (hasMain) return "main";
  const hasMaster = safeExec("git rev-parse --verify master").includes("fatal") ? false : true;
  if (hasMaster) return "master";
  const out = safeExec("git branch --show-current");
  return out || "main";
}

// 保留旧基准实现，仅供对照/测试使用，主流程已改用 getCodeBaselineTs。
function getLastMergeTs(branch) {
  const out = safeExec(`git log ${branch} --merges -n 1 --format=%ct`);
  const n = Number(out);
  if (Number.isFinite(n) && n > 0) return n;
  const out2 = safeExec(`git log ${branch} -n 1 --format=%ct`);
  const n2 = Number(out2);
  return Number.isFinite(n2) && n2 > 0 ? n2 : null;
}

// 基准 = 最近一次触及「代码路径」的提交。语义是：代码变了，描述它的文档可能就旧了。
// 取不到时回退到全仓最后一次提交（而非返回 null），否则整个 version 检查会静默失效。
// runGit 可注入，使测试能断言命令内容（本次回归的本质就是「调错了 git 命令」）。
function getCodeBaselineTs(branch, runGit = safeExec) {
  const pathspecs = CODE_BASELINE_PATHS.map((p) => `"${p}"`).join(" ");
  const out = runGit(`git log ${branch} -n 1 --format=%ct -- ${pathspecs}`);
  const n = Number(out);
  if (Number.isFinite(n) && n > 0) return n;
  const out2 = runGit(`git log ${branch} -n 1 --format=%ct`);
  const n2 = Number(out2);
  return Number.isFinite(n2) && n2 > 0 ? n2 : null;
}

function dayDiff(aTs, bTs) {
  return Math.floor((aTs - bTs) / 86400);
}

// null 安全：基准或文档时间戳缺失时返回 null（而非 NaN），由调用方跳过该检查。
function computeLagDays(baselineTs, usedTs) {
  if (!baselineTs || !usedTs) return null;
  return dayDiff(baselineTs, usedTs);
}

function isVersionStale(lagDays, threshold = VERSION_LAG_THRESHOLD_DAYS) {
  return lagDays !== null && lagDays > threshold;
}

// 段匹配而非子串匹配：避免 `docs/bsp/kernel-config/` 连带豁免
// `docs/bsp/kernel-config-notes.md` 这类同前缀不同目录的文档。
// 空模式必须不匹配任何路径（裸 `rel.includes("")` 恒真，会静默豁免全部文档）。
function shouldSkipVersionPath(rel, patterns) {
  const p = toPosix(rel);
  return patterns.some((raw) => {
    const norm = toPosix(String(raw || "")).replace(/\/+$/, "");
    if (!norm) return false;
    return p === norm || p.startsWith(`${norm}/`);
  });
}

function normalizeHeadingText(t) {
  return String(t || "")
    .trim()
    .replace(/\s+/g, " ")
    .replace(/[`*_]/g, "");
}

function slugifyHeadingForGitHub(t) {
  const s = normalizeHeadingText(t).toLowerCase();
  const cleaned = s
    .replace(/[^\p{Letter}\p{Mark}\p{Number}\p{Connector_Punctuation}\s-]/gu, "")
    .replace(/\s+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "");
  return cleaned;
}

function collectAnchors(md) {
  const anchors = new Set();
  const lines = md.split(/\r?\n/);
  for (const line of lines) {
    const m = line.match(/<a\s+id="([^"]+)"\s*><\/a>/i);
    if (m) anchors.add(m[1]);
    const h = line.match(/^(#{1,6})\s+(.+?)\s*$/);
    if (h) anchors.add(slugifyHeadingForGitHub(h[2]));
  }
  return anchors;
}

function collectMdLinks(md) {
  const blocks = splitMarkdownBlocks(md);
  const links = [];
  for (const b of blocks) {
    if (b.type !== "text") continue;
    for (const line of b.lines) {
      const re = /\[[^\]]*]\(([^)]+)\)/g;
      let m;
      while ((m = re.exec(line)) !== null) {
        links.push(m[1]);
      }
      const bare = stripInlineCode(line).match(/https?:\/\/[^\s)>\]]+/g);
      if (bare) {
        for (const u of bare) links.push(u);
      }
    }
  }
  return links;
}

function isLikelyTableLine(line) {
  const t = line.trim();
  if (!t.includes("|")) return false;
  if (/^\|/.test(t) || /\|$/.test(t)) return true;
  return false;
}

function isListLine(line) {
  return /^\s*([-*+]|\d+\.)\s+/.test(line);
}

function wrapLongLine(line, maxLen) {
  if (line.length <= maxLen) return [line];
  const out = [];
  let rest = line;
  while (rest.length > maxLen) {
    let cut = -1;
    const probe = rest.slice(0, maxLen + 1);
    for (let i = probe.length - 1; i >= 0; i -= 1) {
      if (probe[i] === " ") {
        cut = i;
        break;
      }
    }
    if (cut <= 0) {
      const punct = ["，", "。", "；", "：", "、", ",", ".", ";", ":"];
      for (let i = probe.length - 1; i >= 0; i -= 1) {
        if (punct.includes(probe[i])) {
          cut = i + 1;
          break;
        }
      }
    }
    if (cut <= 0) cut = maxLen;
    out.push(rest.slice(0, cut).trimEnd());
    rest = rest.slice(cut).trimStart();
  }
  if (rest.length > 0) out.push(rest);
  return out;
}

function inferFenceLang(codeLines) {
  const body = codeLines.slice(1, -1).join("\n").trim();
  if (!body) return "text";
  if (/^\s*\{[\s\S]*\}\s*$/.test(body)) return "json";
  if (/^\s*<\?xml/.test(body)) return "xml";
  if (/\bcmake\b|\bctest\b/i.test(body)) return "bash";
  if (/\badb\b|\bshell\b/i.test(body)) return "bash";
  if (/^\s*&\s*".+\.exe"/m.test(body) || /\$env:/m.test(body)) return "powershell";
  if (/^\s*#include\s+<|^\s*int\s+main\s*\(/m.test(body)) return "cpp";
  if (/^\s*package\s+[\w.]+;|^\s*import\s+android\./m.test(body)) return "java";
  if (/^graph\s+(TD|LR)|^sequenceDiagram/m.test(body)) return "mermaid";
  return "bash";
}

function fixMarkdown(md, defects) {
  const blocks = splitMarkdownBlocks(md);
  const fixedBlocks = [];
  for (const b of blocks) {
    if (b.type === "code") {
      if (b.lines.length > 0) {
        const first = b.lines[0];
        const m = first.match(/^(```+)(\s*)(.*)$/);
        if (m) {
          const lang = String(m[3] || "").trim();
          if (!lang) {
            const inferred = inferFenceLang(b.lines);
            defects.push({ type: "format", rule: "code_fence_lang", message: `代码块未声明语言，已补为 ${inferred}` });
            b.lines[0] = `${m[1]} ${inferred}`.trimEnd();
          }
        }
      }
      fixedBlocks.push(b.lines.join("\n"));
      continue;
    }
    const outLines = [];
    const headingCounts = new Map();
    for (let idx = 0; idx < b.lines.length; idx += 1) {
      const raw = b.lines[idx];
      const h = raw.match(/^(#{1,6})\s+(.+?)\s*$/);
      if (h) {
        const key = normalizeHeadingText(h[2]);
        const n = (headingCounts.get(key) || 0) + 1;
        headingCounts.set(key, n);
        if (n >= 2) {
          const newText = `${h[2]}（${n}）`;
          const id = `${slugifyHeadingForGitHub(h[2])}-${n}`;
          defects.push({ type: "markdownlint", rule: "MD024", message: `重复标题：${key}，已追加后缀并插入锚点 #${id}` });
          outLines.push(`<a id="${id}"></a>`);
          outLines.push(`${h[1]} ${newText}`);
          continue;
        }
      }

      const noInline = stripInlineCode(raw);
      const bareUrls = noInline.match(/https?:\/\/[^\s)>\]]+/g);
      let line = raw;
      if (bareUrls) {
        for (const u of bareUrls) {
          const esc = u.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
          const inLink = new RegExp(`\\]\\(${esc}\\)`).test(line);
          const inAngle = new RegExp(`<${esc}>`).test(line);
          if (!inLink && !inAngle) {
            line = line.replace(u, `<${u}>`);
            defects.push({ type: "markdownlint", rule: "MD034", message: `裸露链接已包裹为 <>：${u}` });
          }
        }
      }

      const maxLen = 120;
      if (line.length > maxLen && !isLikelyTableLine(line) && !isListLine(line) && !/^#{1,6}\s+/.test(line)) {
        const wrapped = wrapLongLine(line, maxLen);
        if (wrapped.length >= 2) {
          defects.push({ type: "markdownlint", rule: "MD013", message: `长行已自动换行（>${maxLen}）` });
          outLines.push(...wrapped);
          continue;
        }
      }

      outLines.push(line);
    }
    fixedBlocks.push(outLines.join("\n"));
  }
  return fixedBlocks.join("\n");
}

// 全角字符分类。中文文档本就应当使用全角标点（，。；：），把它们算作问题会产生
// 海量假阳性（实测全仓 5086 行会被误报）。真正的问题是全角字母数字（ＡＢＣ１２３）
// 与全角空格 U+3000——它们会破坏搜索、复制与代码对齐。
// 用码点区间判断而非正则字面量，避免源文件里出现肉眼难辨的全角字符。
function classifyFullwidth(text) {
  let alnum = 0;
  let space = 0;
  let punct = 0;
  for (const ch of text) {
    const c = ch.codePointAt(0);
    if (c === 0x3000) space += 1;
    else if (
      (c >= 0xff10 && c <= 0xff19) ||
      (c >= 0xff21 && c <= 0xff3a) ||
      (c >= 0xff41 && c <= 0xff5a)
    ) {
      alnum += 1;
    } else if (c >= 0xff01 && c <= 0xff5e) {
      punct += 1;
    }
  }
  return { alnum, space, punct };
}

function scanTextConventions(md, sampleLimit) {
  const lines = md.split(/\r?\n/);
  let inFence = false;
  let fence = "```";
  let zhEnNoSpace = 0;
  let fullwidthAscii = 0;
  let fullwidthProblem = 0;
  const samples = [];
  const problemSamples = [];
  const fullwidthRe = /[\u3000\uFF01-\uFF5E]/u;
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    const fenceStart = line.match(/^(```+)/);
    if (fenceStart && !inFence) {
      inFence = true;
      fence = fenceStart[1];
      continue;
    }
    if (inFence) {
      if (line.startsWith(fence)) inFence = false;
      continue;
    }
    const t = stripInlineCode(line);
    const zhEn = /[\u4e00-\u9fff][A-Za-z0-9]/.test(t) || /[A-Za-z0-9][\u4e00-\u9fff]/.test(t);
    const fw = fullwidthRe.test(t);
    const cls = classifyFullwidth(t);
    if (zhEn) zhEnNoSpace += 1;
    if (fw) fullwidthAscii += 1;
    // 与中英混排分开维护样例池：否则中英混排行会挤占全角问题的样例，
    // 使 low 缺陷的 message 拼不出可定位的行号。
    if (cls.alnum > 0 || cls.space > 0) {
      fullwidthProblem += 1;
      if (problemSamples.length < sampleLimit) {
        const parts = [];
        if (cls.alnum > 0) parts.push(`全角字母数字 ×${cls.alnum}`);
        if (cls.space > 0) parts.push(`全角空格 ×${cls.space}`);
        problemSamples.push({ line: i + 1, message: parts.join(" + "), excerpt: t.trim().slice(0, 120) });
      }
    }
    if ((zhEn || fw) && samples.length < sampleLimit) {
      const msg = zhEn && fw ? "中英文混排缺空格 + 全角ASCII" : zhEn ? "中英文混排缺空格" : "全角ASCII/全角空格";
      samples.push({ line: i + 1, message: msg, excerpt: t.trim().slice(0, 120) });
    }
  }
  return { zhEnNoSpace, fullwidthAscii, fullwidthProblem, samples, problemSamples };
}

// 外部 markdownlint-cli2 默认规则集与本仓库的既有排版惯例冲突严重，故不纳入 CI。
// 实测（2026-09-15，83 个文件）：共约 5900 条违规，其中 MD060（表格管道紧凑样式）
// 2194 条、MD013（行长 80）2173 条 —— 中文长行与紧凑表格是本仓库的既有风格，不是缺陷。
// 启用前需先禁用 28 条规则中的 22 条，代价与收益不成比例。
// 等价的克制方案已内置：`--fix` 覆盖 MD040/MD024/MD034/MD013(120) 四条规则。
// 若要把这四条改为「报告模式」，实测会新增 362 条 low（MD013 197 / MD024 127），
// 同样以风格偏好为主，故亦不启用。本开关保留供按需手工排查。
function runMarkdownlintCli2(files, repoRoot) {
  const bin = "npx --yes markdownlint-cli2";
  const quoted = files.map((f) => `"${toPosix(path.relative(repoRoot, f))}"`).join(" ");
  // Q-CQ-B08-007 门禁语义：markdownlint-cli2 非零退出码即视为检查失败。
  // 此前 safeExec 吞掉非零退出码导致漏报，现需把退出码一并返回，由门禁阻断。
  try {
    return {
      ok: true,
      exitCode: 0,
      output: String(execSync(`${bin} ${quoted}`, { stdio: ["ignore", "pipe", "pipe"] })).trim(),
    };
  } catch (e) {
    const out = e && e.stdout ? String(e.stdout) : "";
    const err = e && e.stderr ? String(e.stderr) : "";
    const exitCode = e && typeof e.status === "number" ? e.status : 1;
    return { ok: false, exitCode, output: (out + "\n" + err).trim() };
  }
}

// 章节关键字命中判定。英文键按「双侧词边界」匹配（被匹配项两侧须分别是行首/非字母数字
// 与行尾/非字母数字）：此前用无锚定子串正则（`new RegExp(k,"i").test(hay)`），README 写一个
// `## Contest` 就能顶替「测试命令」、`## Rebuild` 顶替「构建命令」，`PR` 也能被 "Process" 顶替。
// 中文键（不含 ASCII 字母）不存在词边界概念，保持子串包含。
//
// 副作用需知：双侧边界下英文键只命中「完整单词」，词形变化不再命中——`debug` 不匹配
// "Debugging"、`style` 不匹配 "Styling"、`test` 不匹配 "Testing"。注意 `style` 对标题
// "Code Style" 是命中的（Style 是完整单词、双侧成界），并非词形变化反例。这些必填组
// 另有中文键兜底（`调试` / `代码规范` / `测试`），真实文档不受影响。
function sectionKeyHit(hay, key) {
  const k = String(key || "");
  if (!/[A-Za-z]/.test(k)) return hay.toLowerCase().includes(k.toLowerCase());
  const esc = k.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  // 以 `_` 结尾的键（如 `CONFIG_`）表示「部分标识符 / 前缀」——右侧 `_` 后常直接接字母
  // （`CONFIG_FOO`），严格的右边界会让它对真实标题失效（`CONFIG_` 命中不了 `## CONFIG_FOO`）。
  // 故这类键只要求左边界；其余英文键仍走双侧边界。左边界保留，防止 `MYCONFIG_X` 被顶替。
  if (/_$/.test(k)) return new RegExp(`(^|[^A-Za-z0-9])${esc}`, "i").test(hay);
  return new RegExp(`(^|[^A-Za-z0-9])${esc}([^A-Za-z0-9]|$)`, "i").test(hay);
}

function findRequiredSections(md, required) {
  const headings = [];
  const lines = md.split(/\r?\n/);
  for (const line of lines) {
    const m = line.match(/^#{1,6}\s+(.+?)\s*$/);
    if (m) headings.push(m[1].trim());
  }
  const hay = headings.join("\n");
  const missing = [];
  for (const group of required) {
    const ok = group.anyOf.some((k) => sectionKeyHit(hay, k));
    if (!ok) missing.push(group.name);
  }
  return { headings, missing };
}

async function headFetch(url) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 12000);
  try {
    const r = await fetch(url, { method: "HEAD", redirect: "follow", signal: controller.signal });
    return { ok: r.status >= 200 && r.status < 400, status: r.status, finalUrl: r.url };
  } catch (e) {
    return { ok: false, status: 0, finalUrl: url, error: String(e && e.message ? e.message : e), unverifiable: true };
  } finally {
    clearTimeout(timer);
  }
}

async function getFetchFallback(url) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), 12000);
  try {
    const r = await fetch(url, {
      method: "GET",
      redirect: "follow",
      headers: { Range: "bytes=0-0" },
      signal: controller.signal,
    });
    return { ok: r.status >= 200 && r.status < 400, status: r.status, finalUrl: r.url };
  } catch (e) {
    return { ok: false, status: 0, finalUrl: url, error: String(e && e.message ? e.message : e), unverifiable: true };
  } finally {
    clearTimeout(timer);
  }
}

async function checkLinks(urls, concurrency) {
  const uniq = Array.from(new Set(urls.filter((u) => isHttpUrl(u))));
  const results = new Array(uniq.length);
  let idx = 0;
  const withTimeout = async (p, ms) => {
    let timer;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error("Timeout")), ms);
    });
    try {
      return await Promise.race([p, timeout]);
    } finally {
      if (timer) clearTimeout(timer);
    }
  };
  const worker = async () => {
    while (true) {
      const cur = idx;
      idx += 1;
      if (cur >= uniq.length) return;
      const u = uniq[cur];
      try {
        const r = await withTimeout(headFetch(u), 15000);
        if (!r.ok && (r.status === 403 || r.status === 405 || r.status === 0)) {
          const r2 = await withTimeout(getFetchFallback(u), 15000);
          results[cur] = { url: u, ...r2, method: "GET_RANGE" };
        } else {
          results[cur] = { url: u, ...r, method: "HEAD" };
        }
      } catch (e) {
        results[cur] = {
          url: u,
          ok: false,
          status: 0,
          finalUrl: u,
          error: String(e && e.message ? e.message : e),
          unverifiable: true,
          method: "WORKER",
        };
      }
    }
  };
  const n = Math.min(Math.max(1, concurrency || 1), uniq.length || 1);
  await Promise.all(new Array(n).fill(0).map(() => worker()));
  return results.filter(Boolean);
}

async function loadLinkCache(cachePath) {
  try {
    const data = await fsp.readFile(cachePath, "utf8");
    const parsed = JSON.parse(data);
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) return parsed;
  } catch { /* ignore corrupt / missing */ }
  return {};
}

async function saveLinkCache(cachePath, cache) {
  await fsp.mkdir(path.dirname(cachePath), { recursive: true });
  await fsp.writeFile(cachePath, JSON.stringify(cache, null, 2) + "\n", "utf8");
}

function resolveCrossRefPath(fromFile, rawPath) {
  const rootFiles = new Set(["README.md", "DEVELOP.md", "AGENTS.md", "CREDITS.md", "CHANGELOG.md"]);
  const raw = toPosix(rawPath);
  // P3-5 R2：裸名短路仅对根级文件生效（dirname="."）。非根文件（如 docs/research/appendix-e.md）
  // 写 [CHANGELOG.md] 时 GitHub 按当前目录相对解析为 docs/research/CHANGELOG.md（可能 404），
  // 短路会绕过相对解析导致漏报——故仅 dirname 为根时短路。
  if (rootFiles.has(raw) && path.posix.dirname(fromFile) === ".") return raw;
  if (raw === "RK3288_CONSTRAINTS.md") return "docs/RK3288_CONSTRAINTS.md";
  // P3-5 修复：docs/ 前缀链接此前一律按仓库根归一化，导致 docs/foo/bar.md 写 docs/other.md 时
  // 门禁查根级 docs/other.md 放行，但 GitHub 按当前文件目录相对解析为 docs/foo/docs/other.md（404），
  // 造成 xref 缺失漏报。现统一按引用文件所在目录相对解析（GitHub 语义）：
  // 根级文件的 dirname 为 "."，join 后等价于仓库根解析，天然兼容旧行为。
  return path.posix.normalize(path.posix.join(path.posix.dirname(fromFile), raw));
}

function auditCrossRefs(crossRefs, anchorMap) {
  const defects = [];
  for (const r of crossRefs) {
    const normalizedTo = resolveCrossRefPath(r.fromFile, r.toFile);
    if (!anchorMap[normalizedTo]) {
      // Q-CQ-B08-007 门禁语义：交叉引用指向不存在（或未被 git 跟踪）的目标 .md 文档，
      // 此前被 continue 静默跳过造成漏报；现计为必须阻断门禁的高危缺陷。
      defects.push({
        type: "xref",
        severity: "high",
        file: r.fromFile,
        message: `交叉引用目标文档缺失：${r.raw}（目标 ${normalizedTo} 不存在或未被跟踪）`,
      });
      continue;
    }
    if (r.anchor && !anchorMap[normalizedTo].has(r.anchor)) {
      defects.push({
        type: "xref",
        severity: "medium",
        file: r.fromFile,
        message: `交叉引用锚点不存在：${r.raw}（目标 ${normalizedTo} 缺少 #${r.anchor}）`,
      });
    }
  }
  return defects;
}

function parseCrossRefs(md, fromFile) {
  const links = collectMdLinks(md);
  const refs = [];
  for (const href of links) {
    if (href.startsWith("#")) {
      refs.push({ fromFile, toFile: fromFile, anchor: href.slice(1), raw: href });
      continue;
    }
    const m = href.match(/^([A-Za-z0-9_./-]+\.md)(#(.+))?$/);
    if (m) {
      refs.push({ fromFile, toFile: m[1], anchor: m[3] || null, raw: href });
    }
  }
  return refs;
}

function findDefconfigPath(developMd) {
  const lines = developMd.split(/\r?\n/);
  for (const line of lines) {
    const m = line.match(/`([^`]*defconfig[^`]*)`/i);
    if (m) return m[1];
  }
  return null;
}

function findKernelConfigSnapshotPath(developMd) {
  const lines = developMd.split(/\r?\n/);
  for (const line of lines) {
    const m = line.match(/`([^`]*kernel-config[^`]*)`/i);
    if (m) return m[1];
  }
  return null;
}

function isPlaceholderText(text) {
  return /(^|\n)\s*#\s*PLACEHOLDER\b/i.test(text) || /\bPLACEHOLDER\b/i.test(text);
}

function parseKernelConfigMap(text) {
  const map = new Map();
  const lines = String(text || "").split(/\r?\n/);
  for (const line of lines) {
    const m1 = line.match(/^(CONFIG_[A-Z0-9_]+)=(.+?)\s*$/);
    if (m1) {
      map.set(m1[1], m1[2]);
      continue;
    }
    const m2 = line.match(/^#\s*(CONFIG_[A-Z0-9_]+)\s+is\s+not\s+set\s*$/);
    if (m2) {
      map.set(m2[1], "n");
    }
  }
  return map;
}

function parseKernelConfigPolicyFromConstraints(md) {
  const lines = String(md || "").split(/\r?\n/);
  let inBlock = false;
  let section = null;
  const must = new Set();
  const optional = new Set();
  const forbidden = new Set();

  for (const raw of lines) {
    const line = raw.trim();
    if (!inBlock) {
      if (line === "DOCSYNC_KERNEL_CONFIG_POLICY") inBlock = true;
      continue;
    }
    if (line.startsWith("```")) break;
    if (!line) continue;
    if (line === "[MUST]") {
      section = "must";
      continue;
    }
    if (line === "[OPTIONAL]") {
      section = "optional";
      continue;
    }
    if (line === "[FORBIDDEN]") {
      section = "forbidden";
      continue;
    }
    if (line.startsWith("#")) continue;
    if (!/^CONFIG_[A-Z0-9_]+$/.test(line)) continue;
    if (section === "must") must.add(line);
    else if (section === "optional") optional.add(line);
    else if (section === "forbidden") forbidden.add(line);
  }

  return {
    must: Array.from(must),
    optional: Array.from(optional),
    forbidden: Array.from(forbidden),
    enabled: must.size + optional.size + forbidden.size > 0,
  };
}

function parseConfigItems(text) {
  const out = new Set();
  const lines = text.split(/\r?\n/);
  for (const line of lines) {
    const re = /\b(CONFIG_[A-Z0-9_]+)\b/g;
    let m;
    while ((m = re.exec(line)) !== null) out.add(m[1]);
  }
  return out;
}

const REPO_REF_EXT = /\.(md|js|mjs|cjs|ts|tsx|jsx|py|cpp|cc|h|hpp|json|yaml|yml|sh|ps1|bat|cmd|ini|toml|txt|xml|properties|gradle|cmake|cfg|kt|java)$/i;

// 把一处反引号引用展开成候选仓库路径。返回空数组表示「这不是一条仓库路径引用」——
// 过滤规则刻意保守：裸文件名、通配符、占位符、绝对路径、URL 一律不计，以免制造噪声。
function expandRepoPathRef(raw) {
  let s = String(raw || "").trim();
  if (!s) return [];
  s = s.replace(/[#:].*$/, ""); // 去掉 :行号 与 #锚点
  if (!s || /^https?:/i.test(s)) return [];
  if (/[*?<>{}]/.test(s) || s.includes("...")) return []; // 通配、花括号展开、占位符
  if (!s.includes("/")) return []; // 裸文件名歧义过大（同名文件散落多处），不计
  if (s.startsWith("/") || /^[A-Za-z]:/.test(s)) return [];
  const candidates = [];
  // `Foo.h/.cpp` 这类合并写法展开成 Foo.h 与 Foo.cpp
  const multi = s.match(/^(.*)\.([A-Za-z0-9]+)((?:\/\.[A-Za-z0-9]+)+)$/);
  if (multi) {
    candidates.push(`${multi[1]}.${multi[2]}`);
    for (const extra of multi[3].split("/")) candidates.push(`${multi[1]}${extra}`);
  } else {
    candidates.push(s);
  }
  // 只接受仓库根相对路径且落在已知顶层目录下：据此排除 `try/catch`、
  // `actions/setup-java`、`storage/...`（运行时路径，不入库）之类的误命中。
  return candidates.filter(
    (c) => REPO_REF_EXT.test(c) && REPO_REF_PREFIXES.some((pre) => c.startsWith(pre))
  );
}

// 提取正文（跳过围栏代码块）中反引号引用的仓库路径。
function collectRepoPathRefs(md) {
  const lines = md.split(/\r?\n/);
  const refs = [];
  let inFence = false;
  let fence = "```";
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    const fenceStart = line.match(/^(```+)/);
    if (fenceStart && !inFence) {
      inFence = true;
      fence = fenceStart[1];
      continue;
    }
    if (inFence) {
      if (line.startsWith(fence)) inFence = false;
      continue;
    }
    const re = /`([^`]+)`/g;
    let m;
    while ((m = re.exec(line))) {
      for (const p of expandRepoPathRef(m[1])) refs.push({ path: p, line: i + 1 });
    }
  }
  return refs;
}

// 引用的仓库路径不存在 → low（提示，不阻断）。
// 实测本仓库上该检查的真阳性为零：命中的都是「上游第三方仓库同名文件」（如 CREDITS.md
// 引 ncnn 的 `src/paramdict.cpp`）、「运行时路径」（`storage/...`）与「计划中的路径」。
// 反引号里的路径本质上是歧义的，不像 .md 链接那样可判定，因此不能用作阻断条件——
// 否则会重蹈 version 时间戳检查的覆辙。
function auditRefPaths(refs, repoRoot, fromFile) {
  const defects = [];
  for (const r of refs) {
    if (fs.existsSync(path.resolve(repoRoot, r.path))) continue;
    defects.push({
      type: "refpath",
      severity: "low",
      file: fromFile,
      message: `引用的仓库路径不存在：\`${r.path}\`（第 ${r.line} 行）`,
    });
  }
  return defects;
}

// 链接检查结果的严重级别映射。独立成纯函数以便单测 —— 真实探测依赖网络，
// 无法在测试中稳定复现「不可校验 / 缓存失效 / 真实 4xx」三档分支。
// 只有确凿的失效（真实非 2xx-3xx 响应）才阻断门禁；网络抖动导致的请求失败
// 与缓存中的历史失效都不阻断，否则 CI 会因外网抖动而随机变红。
function classifyLinkResult(r) {
  if (r.ok) return null;
  if (r.unverifiable) return "low";
  if (r.method === "CACHE") return "low";
  return "medium";
}

function linkDefectMessage(r, severity) {
  if (severity === "low" && r.unverifiable) {
    return `链接无法验证（网络/证书/代理等导致请求失败）：${r.url}（${r.method} final=${r.finalUrl}${r.error ? ` err=${r.error}` : ""}）`;
  }
  if (severity === "low") {
    return `链接上次检查已失效（缓存）：${r.url}（status=${r.status || 0}）`;
  }
  return `失效链接：${r.url}（${r.method} status=${r.status || 0} final=${r.finalUrl}${r.error ? ` err=${r.error}` : ""}）`;
}

async function main() {
  const args = parseArgs(process.argv);
  const repoRoot = path.resolve(__dirname, "..");
  const allFiles = safeExec(
    `git ls-files --cached --others --exclude-standard "*.md"`
  ).split(/\r?\n/).filter(Boolean).map(f => path.resolve(repoRoot, f)).filter(f => fs.existsSync(f));
  const rootFiles = [
    path.join(repoRoot, "README.md"),
    path.join(repoRoot, "DEVELOP.md"),
    path.join(repoRoot, "docs", "RK3288_CONSTRAINTS.md"),
  ];
  const files = allFiles.length > 0 ? allFiles : rootFiles;

  const branch = getMainBranchName();
  const baselineTs = getCodeBaselineTs(branch);

  const report = {
    runId: `${nowIsoLocal()}-${sha256Hex(String(Date.now())).slice(0, 8)}`,
    branch,
    baselineTs,
    baselineIso: baselineTs ? new Date(baselineTs * 1000).toISOString() : null,
    files: {},
    defects: [],
    summary: { defectCount: 0, severity: { high: 0, medium: 0, low: 0 } },
  };

  const mdCache = {};
  for (const abs of files) {
    const rel = toPosix(path.relative(repoRoot, abs));
    const md = await fsp.readFile(abs, "utf8");
    mdCache[rel] = md;
    const headTs = extractHeadUpdateTime(md);
    const gitTs = getGitFileLastCommitTs(abs);
    const usedTs = headTs || gitTs;
    const lagDays = computeLagDays(baselineTs, usedTs);

    const links = collectMdLinks(md);
    const externalLinks = links.filter((u) => isHttpUrl(u));
    const conv = scanTextConventions(md, args.spacingSampleLimit);

    let fixed = md;
    if (args.fix) {
      fixed = fixMarkdown(fixed, report.defects);
      if (fixed !== md) await fsp.writeFile(abs, fixed, "utf8");
    }

    report.files[rel] = {
      path: rel,
      headUpdateTs: headTs,
      gitLastCommitTs: gitTs,
      usedUpdateTs: usedTs,
      usedUpdateIso: usedTs ? new Date(usedTs * 1000).toISOString() : null,
      lagDaysFromBaseline: lagDays,
      externalLinkCount: externalLinks.length,
      zhEnNoSpaceLineCount: conv.zhEnNoSpace,
      fullwidthAsciiLineCount: conv.fullwidthAscii,
      fullwidthProblemLineCount: conv.fullwidthProblem,
      // 保留 formatInfo 字段形状不变，避免破坏既有消费方
      formatInfo: { zhEnNoSpace: conv.zhEnNoSpace, fullwidthAscii: conv.fullwidthAscii },
    };

    // 元数据块是「这份文档是什么、是否还需跟随代码」的唯一权威声明
    if (requiresDocMeta(rel)) {
      report.defects.push(...auditDocMeta(rel, md));
    }

    // 只有活文档（status=living）做滞后与路径存在性检查——时点快照引用「当时存在、
    // 现已消失」的路径是合法的。豁免依据是文档自述，不是它放在哪个目录。
    const skipChecks =
      !requiresDocMeta(rel) ||
      !isLivingDoc(md) ||
      shouldSkipVersionPath(rel, args.skipCheckPaths);

    // version 定为 low（复核清单，不阻断门禁）：时间戳无法证明内容陈旧——同一份文档
    // 只改一个字符的提交就能让它「通过」，而活跃仓库里任何活文档按此指标都会永久报红。
    if (!skipChecks && isVersionStale(lagDays)) {
      report.defects.push({
        type: "version",
        severity: "low",
        file: rel,
        message: `文档更新滞后：距最近一次代码变更已 ${lagDays} 天（阈值 ${VERSION_LAG_THRESHOLD_DAYS} 天）`,
      });
    }

    // 全角字母数字 / 全角空格是真实排版问题（破坏搜索、复制与代码对齐），记 low 不阻断；
    // 全角标点是中文文档的正确用法，不计入。
    if (conv.fullwidthProblem > 0) {
      const sample = conv.problemSamples
        .map((s) => `L${s.line}(${s.message}): ${s.excerpt}`)
        .join(" | ");
      report.defects.push({
        type: "format",
        severity: "low",
        file: rel,
        message: `存在全角字母数字或全角空格共 ${conv.fullwidthProblem} 行${sample ? `；样例：${sample}` : ""}`,
      });
    }

    if (!skipChecks) {
      for (const d of auditRefPaths(collectRepoPathRefs(md), repoRoot, rel)) {
        report.defects.push(d);
      }
    }
  }

  const agentGuideAudit = auditAgentGuides(repoRoot);
  report.agentGuideAudit = agentGuideAudit;
  report.defects.push(...agentGuideAudit.defects);

  // 索引完整性：docs/index.md 与实际文档集做双向差集
  if (mdCache["docs/index.md"] !== undefined) {
    report.defects.push(...auditDocIndex(mdCache["docs/index.md"], repoRoot, Object.keys(mdCache)));
  }

  // 元数据豁免清单自检（文件级条目存在性）——防止清单里再长出指向已删除文件的死条目
  report.defects.push(...auditDocMetaExemptList(repoRoot));

  const readme = await fsp.readFile(rootFiles[0], "utf8");
  const develop = await fsp.readFile(rootFiles[1], "utf8");
  const constraints = await fsp.readFile(rootFiles[2], "utf8");

  const readmeReq = [
    { name: "项目简介", anyOf: ["项目简介", "简介", "Overview"] },
    { name: "快速开始", anyOf: ["快速开始", "Quick Start"] },
    { name: "依赖列表", anyOf: ["依赖", "Dependencies", "环境要求"] },
    { name: "构建命令", anyOf: ["编译", "构建", "build", "assemble"] },
    { name: "测试命令", anyOf: ["测试", "ctest", "test"] },
    { name: "许可证", anyOf: ["许可证", "License"] },
  ];
  const developReq = [
    { name: "环境搭建", anyOf: ["环境", "安装", "SDK", "依赖"] },
    { name: "调试步骤", anyOf: ["调试", "debug", "log"] },
    { name: "分支策略", anyOf: ["分支", "branch"] },
    { name: "提交流程", anyOf: ["提交", "commit", "pull request", "PR"] },
    { name: "代码规范", anyOf: ["代码规范", "lint", "format", "style"] },
    { name: "发布流程", anyOf: ["发布", "release", "版本"] },
  ];
  const constraintsReq = [
    { name: "硬件限制", anyOf: ["硬件", "内存", "CPU", "GPU", "摄像头"] },
    { name: "内核配置", anyOf: ["内核", "kernel", "defconfig", "CONFIG_"] },
    { name: "驱动补丁", anyOf: ["驱动", "patch", "补丁"] },
    { name: "已知缺陷", anyOf: ["已知缺陷", "known issues", "问题"] },
  ];

  const r1 = findRequiredSections(readme, readmeReq);
  const r2 = findRequiredSections(develop, developReq);
  const r3 = findRequiredSections(constraints, constraintsReq);

  for (const m of r1.missing) report.defects.push({ type: "content", severity: "high", file: "README.md", message: `README 缺少章节：${m}` });
  for (const m of r2.missing) report.defects.push({ type: "content", severity: "medium", file: "DEVELOP.md", message: `DEVELOP 缺少内容段：${m}` });
  for (const m of r3.missing) report.defects.push({ type: "content", severity: "high", file: "docs/RK3288_CONSTRAINTS.md", message: `约束文档缺少内容：${m}` });

  const readmeHasArm64 = /Platform-[^\n]*ARMv8|同时兼容[^\n]*ARMv8|兼容[^\n]*arm64-v8a/i.test(readme);
  const constraintsArmv7Only = /只支持\s*`armeabi-v7a`/i.test(constraints);
  if (readmeHasArm64 && constraintsArmv7Only) {
    report.defects.push({
      type: "consistency",
      severity: "high",
      file: "README.md",
      message: "README 声称支持 arm64/ARMv8，但 RK3288_CONSTRAINTS 约束为仅 armeabi-v7a（需要统一口径）",
    });
  }

  const crossRefs = [];
  const anchorMap = {};
  for (const [rel, md] of Object.entries(mdCache)) {
    crossRefs.push(...parseCrossRefs(md, rel));
    anchorMap[rel] = collectAnchors(md);
  }
  report.defects.push(...auditCrossRefs(crossRefs, anchorMap));

  const defconfigPath = findDefconfigPath(develop);
  if (!defconfigPath) {
    report.defects.push({
      type: "bsp",
      severity: "high",
      file: "DEVELOP.md",
      message: "未在 DEVELOP.md 中定位到 defconfig 文件路径，无法执行“内核配置逐条 diff”检查",
    });
  } else {
    const absDef = path.resolve(repoRoot, defconfigPath);
    const exists = fs.existsSync(absDef);
    if (!exists) {
      report.defects.push({
        type: "bsp",
        severity: "high",
        file: "DEVELOP.md",
        message: `DEVELOP.md 引用了 defconfig 路径但文件不存在：${defconfigPath}`,
      });
    } else {
      const def = await fsp.readFile(absDef, "utf8");
      if (isPlaceholderText(def)) {
        report.defects.push({
          type: "bsp",
          severity: "high",
          file: defconfigPath,
          message: "defconfig 当前为占位文件（PLACEHOLDER），无法执行“约束文档 vs defconfig”的逐条 diff",
        });
      } else {
        const policy = parseKernelConfigPolicyFromConstraints(constraints);
        const defMap = parseKernelConfigMap(def);
        const isEnabled = (v) => v !== undefined && v !== null && String(v).trim() !== "" && String(v).trim() !== "n";

        if (policy.enabled) {
          const missingMust = [];
          for (const k of policy.must) {
            const v = defMap.get(k);
            if (!isEnabled(v)) missingMust.push(k);
          }
          const forbidden = [];
          for (const k of policy.forbidden) {
            const v = defMap.get(k);
            if (isEnabled(v)) forbidden.push(`${k}=${String(v).trim()}`);
          }
          const optionalMissing = [];
          for (const k of policy.optional) {
            const v = defMap.get(k);
            if (!isEnabled(v)) optionalMissing.push(k);
          }

          const base = Math.max(1, policy.must.length);
          const diffPct = missingMust.length / base;
          if (diffPct > 0.03) {
            report.defects.push({
              type: "bsp",
              severity: "high",
              file: defconfigPath,
              message: `内核配置不同步：defconfig 缺失 MUST 项 ${(diffPct * 100).toFixed(2)}%（阈值 3%），缺失项：${missingMust.join(", ")}`,
            });
          }
          if (forbidden.length > 0) {
            report.defects.push({
              type: "bsp",
              severity: "high",
              file: defconfigPath,
              message: `defconfig 出现 FORBIDDEN 项：${forbidden.join(", ")}`,
            });
          }
          if (optionalMissing.length > 0) {
            report.defects.push({
              type: "bsp",
              severity: "low",
              file: defconfigPath,
              message: `defconfig 缺失 OPTIONAL 项（按业务裁剪）：${optionalMissing.join(", ")}`,
            });
          }
        } else {
          const a = parseConfigItems(constraints);
          const b = parseConfigItems(def);
          if (a.size === 0) {
            report.defects.push({
              type: "bsp",
              severity: "high",
              file: "docs/RK3288_CONSTRAINTS.md",
              message: "RK3288_CONSTRAINTS.md 未列出任何 CONFIG_ 项，无法按要求与 defconfig 执行逐条 diff",
            });
          } else if (b.size === 0) {
            report.defects.push({ type: "bsp", severity: "high", file: defconfigPath, message: "defconfig 未解析到任何 CONFIG_ 项" });
          } else {
            let diffCount = 0;
            for (const k of a) if (!b.has(k)) diffCount += 1;
            for (const k of b) if (!a.has(k)) diffCount += 1;
            const base = Math.max(a.size, b.size);
            const diffPct = base === 0 ? 0 : diffCount / base;
            if (diffPct > 0.03) {
              report.defects.push({
                type: "bsp",
                severity: "high",
                file: "docs/RK3288_CONSTRAINTS.md",
                message: `内核配置不同步：约束文档与 defconfig 差异 ${(diffPct * 100).toFixed(2)}%（阈值 3%）`,
              });
            }
          }
        }
      }
    }
  }

  const bspAbs = path.resolve(repoRoot, args.bspReleaseNotes);
  if (!fs.existsSync(bspAbs)) {
    report.defects.push({
      type: "bsp",
      severity: "high",
      file: "docs/RK3288_CONSTRAINTS.md",
      message: `缺少 BSP Release Note 文件，无法核对“内核配置/驱动补丁/已知缺陷”一致性：${toPosix(args.bspReleaseNotes)}`,
    });
  } else {
    const bspText = await fsp.readFile(bspAbs, "utf8");
    const bspHash = sha256Hex(bspText);
    if (isPlaceholderText(bspText)) {
      report.defects.push({
        type: "bsp",
        severity: "high",
        file: toPosix(args.bspReleaseNotes),
        message: "BSP Release Note 当前为占位文件（PLACEHOLDER），需要替换为真实 Release Note 内容以完成同步核对",
      });
    }
    const ref = constraints.match(/BSP\s*(Release\s*)?Note\s*[:：]\s*(.+)$/im);
    if (!ref) {
      report.defects.push({
        type: "bsp",
        severity: "high",
        file: "docs/RK3288_CONSTRAINTS.md",
        message: `RK3288_CONSTRAINTS.md 未声明当前同步的 BSP Release Note 版本/来源（建议增加 “BSP Release Note: ...” 且记录 sha256=${bspHash.slice(0, 12)}）`,
      });
    }
  }

  const kernelCfgPath = findKernelConfigSnapshotPath(develop);
  if (!kernelCfgPath) {
    report.defects.push({
      type: "bsp",
      severity: "high",
      file: "DEVELOP.md",
      message: "未在 DEVELOP.md 中定位到 kernel config 快照路径（kernel-config），无法核对运行态内核配置",
    });
  } else {
    const absK = path.resolve(repoRoot, kernelCfgPath);
    if (!fs.existsSync(absK)) {
      report.defects.push({
        type: "bsp",
        severity: "high",
        file: "DEVELOP.md",
        message: `DEVELOP.md 引用了 kernel config 快照路径但文件不存在：${kernelCfgPath}`,
      });
    } else {
      const ktxt = await fsp.readFile(absK, "utf8");
      if (isPlaceholderText(ktxt)) {
        report.defects.push({
          type: "bsp",
          severity: "high",
          file: kernelCfgPath,
          message: "kernel config 快照当前为占位文件（PLACEHOLDER），需要按约束文档 2.4 节从设备导出后替换",
        });
      } else {
        const policy = parseKernelConfigPolicyFromConstraints(constraints);
        if (!policy.enabled) {
          report.defects.push({
            type: "bsp",
            severity: "high",
            file: "docs/RK3288_CONSTRAINTS.md",
            message: "缺少 DOCSYNC_KERNEL_CONFIG_POLICY 机器可读清单，无法执行 MUST/OPTIONAL/FORBIDDEN 量化检查",
          });
        } else {
          const kernelMap = parseKernelConfigMap(ktxt);
          const isEnabled = (v) => v !== undefined && v !== null && String(v).trim() !== "" && String(v).trim() !== "n";

          const missingMustKernel = [];
          for (const k of policy.must) {
            const vK = kernelMap.get(k);
            if (!isEnabled(vK)) missingMustKernel.push(k);
          }

          const forbiddenKernel = [];
          for (const k of policy.forbidden) {
            const vK = kernelMap.get(k);
            if (isEnabled(vK)) forbiddenKernel.push(`${k}=${String(vK).trim()}`);
          }

          const missingOptionalKernel = [];
          for (const k of policy.optional) {
            const vK = kernelMap.get(k);
            if (!isEnabled(vK)) missingOptionalKernel.push(k);
          }

          if (missingMustKernel.length > 0) {
            report.defects.push({
              type: "bsp",
              severity: "high",
              file: kernelCfgPath,
              message: `运行态内核配置缺失 MUST 项：${missingMustKernel.join(", ")}`,
            });
          }
          if (forbiddenKernel.length > 0) {
            report.defects.push({
              type: "bsp",
              severity: "high",
              file: kernelCfgPath,
              message: `运行态内核配置出现 FORBIDDEN 项：${forbiddenKernel.join(", ")}`,
            });
          }
          if (missingOptionalKernel.length > 0) {
            report.defects.push({
              type: "bsp",
              severity: "low",
              file: kernelCfgPath,
              message: `运行态内核配置缺失 OPTIONAL 项（按业务裁剪）：${missingOptionalKernel.join(", ")}`,
            });
          }
        }
      }
    }
  }

  if (args.runMarkdownlintCli2) {
    const lintResult = runMarkdownlintCli2(files, repoRoot);
    report.markdownlintCli2 = { enabled: true, output: lintResult.output, exitCode: lintResult.exitCode };
    if (!lintResult.ok) {
      // Q-CQ-B08-007 门禁语义：markdownlint 检查以非零退出码结束即存在违规，必须阻断门禁。
      report.defects.push({
        type: "markdownlint",
        severity: "high",
        file: "N/A",
        message: `markdownlint-cli2 检查未通过（退出码 ${lintResult.exitCode}），存在违规需修复后通过门禁`,
      });
    }
  } else {
    report.markdownlintCli2 = { enabled: false, output: "" };
  }

  const urlsAll = Array.from(new Set(Object.values(mdCache).flatMap(md => collectMdLinks(md))));
  let linkResults = [];
  if (!args.skipLinks) {
    let linkCache = {};
    let urlsToCheck = urlsAll;
    if (args.linkCache) {
      linkCache = await loadLinkCache(path.resolve(repoRoot, args.linkCache));
      const now = Date.now() / 1000;
      const uncached = [];
      for (const u of urlsAll) {
        const cached = linkCache[u];
        if (cached && (now - cached.ts) < args.linkCacheTtl) {
          if (cached.ok === false) {
            linkResults.push({ url: u, ok: false, method: "CACHE", status: cached.status || 0, finalUrl: u });
          }
          continue;
        }
        uncached.push(u);
      }
      urlsToCheck = uncached;
    }
    const freshResults = urlsToCheck.length > 0 ? await checkLinks(urlsToCheck, args.linkConcurrency) : [];
    if (args.linkCache && freshResults.length > 0) {
      for (const r of freshResults) {
        linkCache[r.url] = { ok: r.ok, ts: Date.now() / 1000, status: r.status, finalUrl: r.finalUrl };
      }
      await saveLinkCache(path.resolve(repoRoot, args.linkCache), linkCache);
    }
    linkResults = [...linkResults, ...freshResults];
  }
  report.linkChecks = linkResults;
  for (const r of linkResults) {
    const severity = classifyLinkResult(r);
    if (!severity) continue;
    report.defects.push({
      type: "link",
      severity,
      file: "N/A",
      message: linkDefectMessage(r, severity),
    });
  }

  report.summary.defectCount = report.defects.length;
  for (const d of report.defects) {
    const sev = d.severity || "low";
    if (report.summary.severity[sev] !== undefined) report.summary.severity[sev] += 1;
  }

  const outDirAbs = path.resolve(repoRoot, args.outDir);
  await fsp.mkdir(outDirAbs, { recursive: true });
  const stem = `docs_sync_audit_${report.runId.replace(/[:]/g, "-")}`;
  const jsonPath = path.join(outDirAbs, `${stem}.json`);
  const mdPath = path.join(outDirAbs, `${stem}.md`);

  if (args.json) await fsp.writeFile(jsonPath, JSON.stringify(report, null, 2) + "\n", "utf8");
  if (args.md) {
    const lines = [];
    lines.push(`# Docs Sync Audit Report`);
    lines.push("");
    lines.push(`- runId: \`${report.runId}\``);
    lines.push(`- branch: \`${report.branch}\``);
    lines.push(`- baseline: \`${report.baselineIso || "N/A"}\``);
    lines.push(`- defects: \`${report.summary.defectCount}\` (high=${report.summary.severity.high}, medium=${report.summary.severity.medium}, low=${report.summary.severity.low})`);
    lines.push("");
    lines.push(`## Files`);
    lines.push("");
    for (const k of Object.keys(report.files)) {
      const f = report.files[k];
      lines.push(
        `- \`${f.path}\` update=\`${f.usedUpdateIso || "N/A"}\` lagDays=\`${String(f.lagDaysFromBaseline)}\` links=\`${f.externalLinkCount}\` zhEnNoSpaceLines=\`${f.zhEnNoSpaceLineCount}\` fullwidthLines=\`${f.fullwidthAsciiLineCount}\``
      );
    }
    lines.push("");
    lines.push(`## Defects`);
    lines.push("");
    for (const d of report.defects) {
      lines.push(`- [${d.severity || "low"}] ${d.type}: ${d.file || "N/A"} - ${d.message}`);
    }
    lines.push("");
    lines.push(`## Link Checks`);
    lines.push("");
    for (const r of linkResults) {
      lines.push(`- ${r.ok ? "[OK]" : "[FAIL]"} ${r.url} (${r.method} status=${r.status || 0} final=${r.finalUrl})`);
    }
    lines.push("");
    await fsp.writeFile(mdPath, lines.join("\n") + "\n", "utf8");
  }

  const gatePassed = isGatePassed(report.summary.severity);
  const printed = {
    ok: gatePassed,
    outDir: toPosix(path.relative(repoRoot, outDirAbs)),
    json: args.json ? toPosix(path.relative(repoRoot, jsonPath)) : null,
    md: args.md ? toPosix(path.relative(repoRoot, mdPath)) : null,
    defects: report.summary,
  };
  process.stdout.write(JSON.stringify(printed, null, 2) + "\n");
  process.exit(gatePassed ? 0 : 2);
}

function isGatePassed(severity) {
  return severity.high === 0 && severity.medium === 0;
}

if (require.main === module) {
  main().catch((e) => {
    process.stderr.write(String(e && e.stack ? e.stack : e) + "\n");
    process.exit(1);
  });
}

module.exports = {
  auditAgentGuides,
  auditCrossRefs,
  isGatePassed,
  resolveCrossRefPath,
  // 以下导出供单测直接调用（此前 40+ 个函数全为模块私有，导致 version/lagDays、
  // bsp、--fix 等规则完全无法被单测覆盖）
  parseArgs,
  extractHeadUpdateTime,
  computeLagDays,
  isVersionStale,
  getCodeBaselineTs,
  getLastMergeTs,
  getMainBranchName,
  shouldSkipVersionPath,
  scanTextConventions,
  classifyFullwidth,
  collectRepoPathRefs,
  expandRepoPathRef,
  auditRefPaths,
  fixMarkdown,
  findRequiredSections,
  sectionKeyHit,
  parseDocMeta,
  auditDocMeta,
  analyzeDocMetaMarkers,
  isCalendarDate,
  auditDocMetaExemptList,
  DOC_META_HEAD_LINES,
  auditDocIndex,
  collectIndexEntries,
  requiresDocMeta,
  isLivingDoc,
  DOC_META_TYPES,
  DOC_META_STATUSES,
  DOC_META_EXEMPT_PATTERNS,
  DOC_META_EXEMPT_UNLESS,
  classifyLinkResult,
  linkDefectMessage,
  parseKernelConfigMap,
  parseKernelConfigPolicyFromConstraints,
  parseConfigItems,
  isPlaceholderText,
  findDefconfigPath,
  findKernelConfigSnapshotPath,
  CODE_BASELINE_PATHS,
  REPO_REF_PREFIXES,
  VERSION_LAG_THRESHOLD_DAYS,
};
