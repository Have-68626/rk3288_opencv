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
  }
  return args;
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
    /最后更新\s*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /更新时间\s*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /更新日期\s*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
    /last\s+updated\s*[:：]\s*([0-9]{4}-[0-9]{2}-[0-9]{2})/i,
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
    return { ok: r.status === 200 || r.status === 301, status: r.status, finalUrl: r.url };
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
    return { ok: r.status === 200 || r.status === 206 || r.status === 301, status: r.status, finalUrl: r.url };
  } catch (e) {
    return { ok: false, status: 0, finalUrl: url, error: String(e && e.message ? e.message : e), unverifiable: true };
  } finally {
    clearTimeout(timer);
  }
}

async function checkLinks(urls) {
  const uniq = Array.from(new Set(urls.filter((u) => isHttpUrl(u))));
  const results = [];
  for (const u of uniq) {
    const r = await headFetch(u);
    if (!r.ok && (r.status === 403 || r.status === 405 || r.status === 0)) {
      const r2 = await getFetchFallback(u);
      results.push({ url: u, ...r2, method: "GET_RANGE" });
    } else {
      results.push({ url: u, ...r, method: "HEAD" });
    }
  }
  return results;
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

function parseConfigItems(text) {
  const out = new Set();
  const lines = text.split(/\r?\n/);
  for (const line of lines) {
    const m = line.match(/\b(CONFIG_[A-Z0-9_]+)\b/);
    if (m) out.add(m[1]);
  }
  return out;
}

async function main() {
  const args = parseArgs(process.argv);
  const repoRoot = path.resolve(__dirname, "..");
  const files = [
    path.join(repoRoot, "README.md"),
    path.join(repoRoot, "DEVELOP.md"),
    path.join(repoRoot, "docs", "RK3288_CONSTRAINTS.md"),
  ];

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

  for (const abs of files) {
    const rel = toPosix(path.relative(repoRoot, abs));
    const md = await fsp.readFile(abs, "utf8");
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

    if (lagDays !== null && lagDays > 7) {
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
      report.defects.push({
        type: "format",
        severity: "low",
        file: rel,
        message:
          `排版一致性：中英文混排缺空格行数=${conv.zhEnNoSpace}，全角ASCII/全角空格行数=${conv.fullwidthAscii}` +
          sampleText,
      });
    }
  }

  const readme = await fsp.readFile(files[0], "utf8");
  const develop = await fsp.readFile(files[1], "utf8");
  const constraints = await fsp.readFile(files[2], "utf8");

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

  const crossRefs = [
    ...parseCrossRefs(readme, "README.md"),
    ...parseCrossRefs(develop, "DEVELOP.md"),
    ...parseCrossRefs(constraints, "docs/RK3288_CONSTRAINTS.md"),
  ];
  const anchorMap = {
    "README.md": collectAnchors(readme),
    "DEVELOP.md": collectAnchors(develop),
    "docs/RK3288_CONSTRAINTS.md": collectAnchors(constraints),
  };
  for (const r of crossRefs) {
    const to = r.toFile.includes("/") ? r.toFile : r.toFile;
    const normalizedTo =
      to === "RK3288_CONSTRAINTS.md" ? "docs/RK3288_CONSTRAINTS.md" : to === "DEVELOP.md" ? "DEVELOP.md" : to === "README.md" ? "README.md" : to;
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
      }
    }
  }

  if (args.runMarkdownlintCli2) {
    const lintOut = runMarkdownlintCli2(files, repoRoot);
    report.markdownlintCli2 = { enabled: true, output: lintOut };
  } else {
    report.markdownlintCli2 = { enabled: false, output: "" };
  }

  const urlsAll = Array.from(new Set([...collectMdLinks(readme), ...collectMdLinks(develop), ...collectMdLinks(constraints)]));
  const linkResults = await checkLinks(urlsAll);
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

  const printed = {
    ok: report.summary.severity.high === 0,
    outDir: toPosix(path.relative(repoRoot, outDirAbs)),
    json: args.json ? toPosix(path.relative(repoRoot, jsonPath)) : null,
    md: args.md ? toPosix(path.relative(repoRoot, mdPath)) : null,
    defects: report.summary,
  };
  process.stdout.write(JSON.stringify(printed, null, 2) + "\n");
  process.exit(report.summary.severity.high === 0 ? 0 : 2);
}

main().catch((e) => {
  process.stderr.write(String(e && e.stack ? e.stack : e) + "\n");
  process.exit(1);
});
