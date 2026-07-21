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
    bspReleaseNotes: "docs/bsp/BSP_RELEASE_NOTES.md",
    runMarkdownlintCli2: false,
    skipLinks: false,
    linkConcurrency: 6,
    linkCache: "",
    linkCacheTtl: 86400,
    skipVersionPaths: ["deps/", "docs/bsp/kernel-config/", "docs/bsp/BSP_RELEASE_NOTES.md", "src/win/third_party/"],
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
    } else if (a === "--skip-version-paths" && argv[i + 1]) {
      args.skipVersionPaths = argv[i + 1].split(",").map(s => s.trim()).filter(Boolean);
      i += 1;
    }
  }
  return args;
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
    const match = line.trim().match(/^([a-z0-9_]+)=(.+)$/i);
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
  const read = (relativePath) => fs.readFileSync(path.join(repoRoot, relativePath), "utf8");
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
  const hasCustomBool = customRunners.every((file) => /TestCase/.test(read(file)));
  const coreUnitMain = read("tests/cpp/core_unit_tests_main.cpp");
  const hasGoogleTest = /#include\s*<gtest\/gtest\.h>/.test(coreUnitMain) &&
    /RUN_ALL_TESTS\s*\(/.test(coreUnitMain) &&
    /add_executable\(core_gtest_tests/.test(read("CMakeLists.txt"));

  return {
    gradle_wrapper: gradleMatch ? gradleMatch[1] : "unknown",
    android_ci_java: javaMatch ? javaMatch[1] : "unknown",
    windows_ci_events: windowsJob.includes("github.event_name == 'pull_request'") && windowsJob.includes("github.event_name == 'push'")
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
  const head = md.split(/\r?\n/).slice(0, 60).join("\n");
  const patterns = [
    /最后更新[\s]*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /更新时间[\s]*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /更新日期[\s]*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /last\s+updated[\s]*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
  ];
  for (const re of patterns) {
    const m = head.match(re);
    if (m) {
      const d = new Date(`${m[1]}T00:00:00`);
      if (!Number.isNaN(d.getTime())) return Math.floor(d.getTime() / 1000);
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

function getLastMergeTs(branch) {
  const out = safeExec(`git log ${branch} --merges -n 1 --format=%ct`);
  const n = Number(out);
  if (Number.isFinite(n) && n > 0) return n;
  const out2 = safeExec(`git log ${branch} -n 1 --format=%ct`);
  const n2 = Number(out2);
  return Number.isFinite(n2) && n2 > 0 ? n2 : null;
}

function dayDiff(aTs, bTs) {
  return Math.floor((aTs - bTs) / 86400);
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

function scanTextConventions(md, sampleLimit) {
  const lines = md.split(/\r?\n/);
  let inFence = false;
  let fence = "```";
  let zhEnNoSpace = 0;
  let fullwidthAscii = 0;
  const samples = [];
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
    if (zhEn) zhEnNoSpace += 1;
    if (fw) fullwidthAscii += 1;
    if ((zhEn || fw) && samples.length < sampleLimit) {
      const msg = zhEn && fw ? "中英文混排缺空格 + 全角ASCII" : zhEn ? "中英文混排缺空格" : "全角ASCII/全角空格";
      samples.push({ line: i + 1, message: msg, excerpt: t.trim().slice(0, 120) });
    }
  }
  return { zhEnNoSpace, fullwidthAscii, samples };
}

function runMarkdownlintCli2(files, repoRoot) {
  const bin = "npx --yes markdownlint-cli2";
  const quoted = files.map((f) => `"${toPosix(path.relative(repoRoot, f))}"`).join(" ");
  const out = safeExec(`${bin} ${quoted}`);
  return out;
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
    const ok = group.anyOf.some((k) => new RegExp(k, "i").test(hay));
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
  if (rootFiles.has(raw) || raw.startsWith("docs/")) return path.posix.normalize(raw);
  if (raw === "RK3288_CONSTRAINTS.md") return "docs/RK3288_CONSTRAINTS.md";
  return path.posix.normalize(path.posix.join(path.posix.dirname(fromFile), raw));
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
  const lastMergeTs = getLastMergeTs(branch);

  const report = {
    runId: `${nowIsoLocal()}-${sha256Hex(String(Date.now())).slice(0, 8)}`,
    branch,
    lastMergeTs,
    lastMergeIso: lastMergeTs ? new Date(lastMergeTs * 1000).toISOString() : null,
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
    const lagDays = lastMergeTs && usedTs ? dayDiff(lastMergeTs, usedTs) : null;

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
      lagDaysFromLastMerge: lagDays,
      externalLinkCount: externalLinks.length,
      zhEnNoSpaceLineCount: conv.zhEnNoSpace,
      fullwidthAsciiLineCount: conv.fullwidthAscii,
    };

    const skipVersion = args.skipVersionPaths.some(p => rel.includes(p.replace(/\\/g, "/").replace(/\/$/, "")));
    if (skipVersion) { /* skip version check for third-party/placeholder files */ }
    else if (lagDays !== null && lagDays > 7) {
      report.defects.push({
        type: "version",
        severity: "high",
        file: rel,
        message: `文档更新滞后：距主分支最近一次合并已 ${lagDays} 天（阈值 7 天）`,
      });
    }

    if (conv.zhEnNoSpace > 0 || conv.fullwidthAscii > 0) {
      const sampleText =
        conv.samples.length === 0
          ? ""
          : `；样例：` +
            conv.samples
              .map((s) => `L${s.line}(${s.message}): ${s.excerpt}`)
              .join(" | ");
      if (!report.files[rel].formatInfo) report.files[rel].formatInfo = {};
      report.files[rel].formatInfo.zhEnNoSpace = conv.zhEnNoSpace;
      report.files[rel].formatInfo.fullwidthAscii = conv.fullwidthAscii;
    }
  }

  const agentGuideAudit = auditAgentGuides(repoRoot);
  report.agentGuideAudit = agentGuideAudit;
  report.defects.push(...agentGuideAudit.defects);

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
  for (const r of crossRefs) {
    const normalizedTo = resolveCrossRefPath(r.fromFile, r.toFile);
    if (!anchorMap[normalizedTo]) continue;
    if (r.anchor && !anchorMap[normalizedTo].has(r.anchor)) {
      report.defects.push({
        type: "xref",
        severity: "medium",
        file: r.fromFile,
        message: `交叉引用锚点不存在：${r.raw}（目标 ${normalizedTo} 缺少 #${r.anchor}）`,
      });
    }
  }

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
    const lintOut = runMarkdownlintCli2(files, repoRoot);
    report.markdownlintCli2 = { enabled: true, output: lintOut };
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
    if (!r.ok) {
      if (r.unverifiable) {
        report.defects.push({
          type: "link",
          severity: "low",
          file: "N/A",
          message: `链接无法验证（网络/证书/代理等导致请求失败）：${r.url}（${r.method} final=${r.finalUrl}${r.error ? ` err=${r.error}` : ""}）`,
        });
        continue;
      }
      if (r.method === "CACHE") {
        report.defects.push({
          type: "link",
          severity: "low",
          file: "N/A",
          message: `链接上次检查已失效（缓存）：${r.url}（status=${r.status || 0}）`,
        });
        continue;
      }
      report.defects.push({
        type: "link",
        severity: "medium",
        file: "N/A",
        message: `失效链接：${r.url}（${r.method} status=${r.status || 0} final=${r.finalUrl}${r.error ? ` err=${r.error}` : ""}）`,
      });
    }
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
    lines.push(`- lastMerge: \`${report.lastMergeIso || "N/A"}\``);
    lines.push(`- defects: \`${report.summary.defectCount}\` (high=${report.summary.severity.high}, medium=${report.summary.severity.medium}, low=${report.summary.severity.low})`);
    lines.push("");
    lines.push(`## Files`);
    lines.push("");
    for (const k of Object.keys(report.files)) {
      const f = report.files[k];
      lines.push(
        `- \`${f.path}\` update=\`${f.usedUpdateIso || "N/A"}\` lagDays=\`${String(f.lagDaysFromLastMerge)}\` links=\`${f.externalLinkCount}\` zhEnNoSpaceLines=\`${f.zhEnNoSpaceLineCount}\` fullwidthLines=\`${f.fullwidthAsciiLineCount}\``
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

module.exports = { auditAgentGuides, isGatePassed, resolveCrossRefPath };
