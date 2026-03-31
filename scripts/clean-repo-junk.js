const fs = require("fs");
const fsp = require("fs/promises");
const path = require("path");
const os = require("os");
const crypto = require("crypto");
const readline = require("readline");

function toPosixPath(p) {
  return p.replace(/\\/g, "/");
}

function normalizePattern(p) {
  return toPosixPath(p).replace(/^\.\//, "");
}

function escapeRegExp(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function globToRegExp(glob) {
  const g = normalizePattern(glob);
  let out = "^";
  for (let i = 0; i < g.length; i += 1) {
    const ch = g[i];
    if (ch === "*") {
      if (g[i + 1] === "*") {
        out += ".*";
        i += 1;
      } else {
        out += "[^/]*";
      }
      continue;
    }
    if (ch === "?") {
      out += "[^/]";
      continue;
    }
    out += escapeRegExp(ch);
  }
  out += "$";
  return new RegExp(out, "i");
}

function isGlobMatch(relPosixPath, pattern) {
  const re = globToRegExp(pattern);
  return re.test(normalizePattern(relPosixPath));
}

function isSubPath(child, parent) {
  const rel = path.relative(parent, child);
  return rel && !rel.startsWith("..") && !path.isAbsolute(rel);
}

function safeRelPath(absPath, repoRoot) {
  const rel = path.relative(repoRoot, absPath);
  if (!rel || rel.startsWith("..") || path.isAbsolute(rel)) {
    throw new Error(`路径不在仓库根目录内: ${absPath}`);
  }
  return toPosixPath(rel);
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes)) return String(bytes);
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let v = bytes;
  let idx = -1;
  while (v >= 1024 && idx < units.length - 1) {
    v /= 1024;
    idx += 1;
  }
  return `${v.toFixed(2)} ${units[idx]}`;
}

function createDefaultSnapshotOptions() {
  return {
    tree: {
      maxDepth: 6,
      maxEntries: 8000,
      maxChildrenPerDir: 200,
    },
    disk: {
      topN: 20,
    },
  };
}

function createRunId() {
  const now = new Date();
  const stamp =
    `${now.getFullYear()}` +
    `${String(now.getMonth() + 1).padStart(2, "0")}` +
    `${String(now.getDate()).padStart(2, "0")}` +
    `T${String(now.getHours()).padStart(2, "0")}` +
    `${String(now.getMinutes()).padStart(2, "0")}` +
    `${String(now.getSeconds()).padStart(2, "0")}`;
  const rand = crypto.randomBytes(3).toString("hex");
  return `${stamp}-${rand}`;
}

async function pathExists(p) {
  try {
    await fsp.access(p);
    return true;
  } catch {
    return false;
  }
}

async function statSafe(p) {
  try {
    return await fsp.lstat(p);
  } catch {
    return null;
  }
}

async function ensureDir(p) {
  await fsp.mkdir(p, { recursive: true });
}

function isSameOrSubPath(child, parent) {
  const c = path.resolve(child);
  const p = path.resolve(parent);
  if (c === p) return true;
  return isSubPath(c, p);
}

function normalizeSnapshotOptions(cfg) {
  const base = createDefaultSnapshotOptions();
  const merged = {
    ...base,
    ...(cfg?.reports || {}),
    tree: { ...base.tree, ...(cfg?.reports?.tree || {}) },
    disk: { ...base.disk, ...(cfg?.reports?.disk || {}) },
  };
  const tree = merged.tree || base.tree;
  const disk = merged.disk || base.disk;
  return {
    tree: {
      maxDepth: Math.max(0, Number(tree.maxDepth) || base.tree.maxDepth),
      maxEntries: Math.max(100, Number(tree.maxEntries) || base.tree.maxEntries),
      maxChildrenPerDir: Math.max(10, Number(tree.maxChildrenPerDir) || base.tree.maxChildrenPerDir),
    },
    disk: {
      topN: Math.max(5, Number(disk.topN) || base.disk.topN),
    },
  };
}

function makeSnapshotLabel(cmd, stage) {
  return `${cmd}-${stage}`;
}

async function writeSnapshotError(outputDir, runId, cmd, stage, err) {
  const msg = err && err.stack ? err.stack : String(err);
  const p = path.join(outputDir, `snapshot-error-${makeSnapshotLabel(cmd, stage)}-${runId}.txt`);
  await writeText(p, msg);
  return p;
}

function shouldExcludeDirent(absPath, dirent, excludeDirNamesLower, excludeAbsPaths) {
  if (excludeAbsPaths && excludeAbsPaths.some((ex) => isSameOrSubPath(absPath, ex))) return true;
  if (dirent && dirent.isDirectory && dirent.isDirectory()) {
    const nameLower = String(dirent.name || "").toLowerCase();
    if (excludeDirNamesLower && excludeDirNamesLower.has(nameLower)) return true;
  }
  return false;
}

async function computeDirStatsWithExcludes(dirAbs, excludeDirNamesLower, excludeAbsPaths) {
  let bytes = 0;
  let filesCount = 0;
  let dirsCount = 0;
  const stack = [dirAbs];

  while (stack.length > 0) {
    const cur = stack.pop();
    if (excludeAbsPaths && excludeAbsPaths.some((ex) => isSameOrSubPath(cur, ex))) continue;
    let dirents;
    try {
      dirents = await fsp.readdir(cur, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const d of dirents) {
      const p = path.join(cur, d.name);
      if (shouldExcludeDirent(p, d, excludeDirNamesLower, excludeAbsPaths)) continue;
      try {
        if (d.isDirectory()) {
          dirsCount += 1;
          stack.push(p);
        } else {
          const st = await fsp.lstat(p);
          filesCount += 1;
          if (st.isFile()) bytes += st.size;
        }
      } catch {
      }
    }
  }

  return { bytes, filesCount, dirsCount };
}

async function getPathStatsWithExcludes(absPath, excludeDirNamesLower, excludeAbsPaths) {
  if (excludeAbsPaths && excludeAbsPaths.some((ex) => isSameOrSubPath(absPath, ex))) return null;
  const st = await statSafe(absPath);
  if (!st) return null;
  if (st.isDirectory()) {
    const ds = await computeDirStatsWithExcludes(absPath, excludeDirNamesLower, excludeAbsPaths);
    return { kind: "dir", bytes: ds.bytes, filesCount: ds.filesCount, dirsCount: ds.dirsCount };
  }
  if (st.isFile()) {
    return { kind: "file", bytes: st.size, filesCount: 1, dirsCount: 0 };
  }
  return { kind: "other", bytes: 0, filesCount: 0, dirsCount: 0 };
}

function relPathOrFallback(absPath, repoRoot) {
  try {
    return safeRelPath(absPath, repoRoot);
  } catch {
    return toPosixPath(path.relative(repoRoot, absPath));
  }
}

async function buildTreeSnapshotText(repoRoot, opts) {
  const createdAt = new Date().toISOString();
  const excludeDirNamesLower = new Set((opts.excludeDirNames || []).map((s) => String(s).toLowerCase()));
  const excludeAbsPaths = Array.isArray(opts.excludeAbsPaths) ? opts.excludeAbsPaths : [];
  const maxDepth = Number.isFinite(opts.maxDepth) ? opts.maxDepth : 6;
  const maxEntries = Number.isFinite(opts.maxEntries) ? opts.maxEntries : 8000;
  const maxChildrenPerDir = Number.isFinite(opts.maxChildrenPerDir) ? opts.maxChildrenPerDir : 200;

  const lines = [];
  lines.push(`createdAt: ${createdAt}`);
  lines.push(`repoRoot: ${repoRoot}`);
  lines.push(`maxDepth: ${maxDepth}`);
  lines.push(`maxEntries: ${maxEntries}`);
  lines.push(`maxChildrenPerDir: ${maxChildrenPerDir}`);
  lines.push(`excludeDirNames: ${(opts.excludeDirNames || []).join(", ")}`);
  lines.push(
    `excludePaths: ${excludeAbsPaths
      .map((p) => relPathOrFallback(p, repoRoot))
      .map((p) => p || ".")
      .join(", ")}`
  );
  lines.push("");

  let written = 0;
  const stack = [{ abs: repoRoot, depth: 0, indent: "" }];
  while (stack.length > 0) {
    const cur = stack.pop();
    if (cur.depth > maxDepth) continue;

    let dirents;
    try {
      dirents = await fsp.readdir(cur.abs, { withFileTypes: true });
    } catch {
      continue;
    }

    const filtered = dirents
      .filter((d) => {
        const p = path.join(cur.abs, d.name);
        return !shouldExcludeDirent(p, d, excludeDirNamesLower, excludeAbsPaths);
      })
      .sort((a, b) => {
        const ad = a.isDirectory();
        const bd = b.isDirectory();
        if (ad !== bd) return ad ? -1 : 1;
        return String(a.name).localeCompare(String(b.name), "zh-CN");
      })
      .slice(0, maxChildrenPerDir);

    const childItems = [];
    for (const d of filtered) {
      const abs = path.join(cur.abs, d.name);
      const rel = relPathOrFallback(abs, repoRoot);
      const name = cur.abs === repoRoot ? rel : d.name;
      const suffix = d.isDirectory() ? "/" : "";
      lines.push(`${cur.indent}- ${name}${suffix}`);
      written += 1;
      if (written >= maxEntries) {
        lines.push(`${cur.indent}- ...（已截断：达到 maxEntries=${maxEntries}）`);
        return lines.join("\n");
      }
      if (d.isDirectory() && cur.depth < maxDepth) {
        childItems.push({ abs, depth: cur.depth + 1, indent: `${cur.indent}  ` });
      }
    }
    for (let i = childItems.length - 1; i >= 0; i -= 1) stack.push(childItems[i]);
  }

  return lines.join("\n");
}

async function buildDiskUsageReport(repoRoot, outputDirAbs, cfg) {
  const createdAt = new Date().toISOString();
  const opts = normalizeSnapshotOptions(cfg);
  const topN = opts.disk.topN;

  const excludeDirNamesLower = new Set([".git", ".trae"].map((s) => s.toLowerCase()));
  const excludeAbsPaths = [];
  if (outputDirAbs && isSameOrSubPath(outputDirAbs, repoRoot)) excludeAbsPaths.push(outputDirAbs);

  let dirents;
  try {
    dirents = await fsp.readdir(repoRoot, { withFileTypes: true });
  } catch {
    dirents = [];
  }

  const topLevel = [];
  for (const d of dirents) {
    const abs = path.join(repoRoot, d.name);
    if (shouldExcludeDirent(abs, d, excludeDirNamesLower, excludeAbsPaths)) continue;
    const st = await getPathStatsWithExcludes(abs, excludeDirNamesLower, excludeAbsPaths);
    if (!st) continue;
    topLevel.push({
      name: d.name,
      relPath: relPathOrFallback(abs, repoRoot),
      kind: st.kind,
      bytes: st.bytes,
      filesCount: st.filesCount,
      dirsCount: st.dirsCount,
    });
  }

  topLevel.sort((a, b) => b.bytes - a.bytes);
  const limitedTop = topLevel.slice(0, topN);

  const totals = topLevel.reduce(
    (acc, it) => {
      acc.bytes += Number(it.bytes) || 0;
      acc.files += Number(it.filesCount) || 0;
      acc.dirs += Number(it.dirsCount) || 0;
      return acc;
    },
    { bytes: 0, files: 0, dirs: 0 }
  );

  return {
    schemaVersion: 1,
    createdAt,
    repoRoot,
    excludes: {
      dirNames: Array.from(excludeDirNamesLower),
      paths: excludeAbsPaths.map((p) => relPathOrFallback(p, repoRoot)),
    },
    totals,
    topLevel: limitedTop,
    topN,
  };
}

function renderDiskUsageText(report) {
  const lines = [];
  lines.push(`createdAt: ${report.createdAt}`);
  lines.push(`repoRoot: ${report.repoRoot}`);
  lines.push(`排除目录: ${(report.excludes?.dirNames || []).join(", ")}`);
  lines.push(`排除路径: ${(report.excludes?.paths || []).join(", ")}`);
  lines.push(`工作区总大小: ${formatBytes(report.totals?.bytes || 0)}`);
  lines.push(`文件数: ${report.totals?.files || 0}，目录数: ${report.totals?.dirs || 0}`);
  lines.push("");
  lines.push(`Top ${report.topN} 一级目录/文件（按大小降序）:`);
  for (const it of report.topLevel || []) {
    lines.push(
      `- ${it.relPath}${it.kind === "dir" ? "/" : ""}: ${formatBytes(it.bytes)} (${it.kind}, files=${it.filesCount}, dirs=${it.dirsCount})`
    );
  }
  return lines.join("\n");
}

function diffDiskReports(before, after) {
  const bBytes = Number(before?.totals?.bytes) || 0;
  const aBytes = Number(after?.totals?.bytes) || 0;
  const deltaBytes = aBytes - bBytes;

  const beforeMap = new Map((before?.topLevel || []).map((it, idx) => [it.relPath, { ...it, rank: idx + 1 }]));
  const afterMap = new Map((after?.topLevel || []).map((it, idx) => [it.relPath, { ...it, rank: idx + 1 }]));

  const changed = [];
  const keys = new Set([...beforeMap.keys(), ...afterMap.keys()]);
  for (const k of keys) {
    const b = beforeMap.get(k) || null;
    const a = afterMap.get(k) || null;
    if (b && a) {
      const db = (Number(a.bytes) || 0) - (Number(b.bytes) || 0);
      if (db !== 0 || b.rank !== a.rank) {
        changed.push({
          relPath: k,
          before: { rank: b.rank, bytes: b.bytes },
          after: { rank: a.rank, bytes: a.bytes },
          deltaBytes: db,
        });
      }
      continue;
    }
    if (b && !a) {
      changed.push({
        relPath: k,
        before: { rank: b.rank, bytes: b.bytes },
        after: null,
        deltaBytes: -(Number(b.bytes) || 0),
      });
      continue;
    }
    if (!b && a) {
      changed.push({
        relPath: k,
        before: null,
        after: { rank: a.rank, bytes: a.bytes },
        deltaBytes: Number(a.bytes) || 0,
      });
    }
  }

  changed.sort((x, y) => Math.abs(y.deltaBytes) - Math.abs(x.deltaBytes));

  return {
    schemaVersion: 1,
    before: {
      createdAt: before?.createdAt || null,
      totalsBytes: bBytes,
      topN: before?.topN || null,
    },
    after: {
      createdAt: after?.createdAt || null,
      totalsBytes: aBytes,
      topN: after?.topN || null,
    },
    delta: {
      bytes: deltaBytes,
      formatted: {
        before: formatBytes(bBytes),
        after: formatBytes(aBytes),
        delta: `${deltaBytes >= 0 ? "+" : ""}${formatBytes(deltaBytes)}`,
        freed: `${bBytes - aBytes >= 0 ? "" : "-"}${formatBytes(Math.abs(bBytes - aBytes))}`,
      },
    },
    topLevelChanged: changed.slice(0, Math.max(10, before?.topN || 20)),
  };
}

function renderSnapshotSummaryText(meta, beforeDisk, afterDisk, diff) {
  const lines = [];
  lines.push(`runId: ${meta.runId}`);
  lines.push(`cmd: ${meta.cmd}`);
  lines.push(`dryRun: ${meta.dryRun}`);
  lines.push(`createdAt: ${meta.createdAt}`);
  lines.push("");
  lines.push(`before 工作区总大小: ${formatBytes(Number(beforeDisk?.totals?.bytes) || 0)}`);
  if (!afterDisk) {
    lines.push("after 工作区总大小: （未生成：dry-run 或执行被取消）");
    return lines.join("\n");
  }
  lines.push(`after 工作区总大小:  ${formatBytes(Number(afterDisk?.totals?.bytes) || 0)}`);
  lines.push(`总大小变化: ${diff.delta.formatted.delta}`);
  lines.push(`释放空间:   ${diff.delta.formatted.freed}`);
  lines.push("");
  lines.push("Top 一级目录/文件变化（按影响大小降序，截取前若干项）:");
  for (const it of diff.topLevelChanged || []) {
    const b = it.before ? `#${it.before.rank} ${formatBytes(it.before.bytes)}` : "(无)";
    const a = it.after ? `#${it.after.rank} ${formatBytes(it.after.bytes)}` : "(无)";
    const delta = `${it.deltaBytes >= 0 ? "+" : ""}${formatBytes(it.deltaBytes)}`;
    lines.push(`- ${it.relPath}: ${b} -> ${a} (${delta})`);
  }
  return lines.join("\n");
}

async function writeWorkspaceSnapshots(repoRoot, outputDirAbs, cfg, runId, cmd, stage) {
  const opts = normalizeSnapshotOptions(cfg);
  const excludeAbsPaths = [];
  if (outputDirAbs && isSameOrSubPath(outputDirAbs, repoRoot)) excludeAbsPaths.push(outputDirAbs);

  const treeText = await buildTreeSnapshotText(repoRoot, {
    maxDepth: opts.tree.maxDepth,
    maxEntries: opts.tree.maxEntries,
    maxChildrenPerDir: opts.tree.maxChildrenPerDir,
    excludeDirNames: [".git", ".trae"],
    excludeAbsPaths,
  });

  const treePath = path.join(outputDirAbs, `tree-${makeSnapshotLabel(cmd, stage)}-${runId}.txt`);
  await writeText(treePath, treeText);

  const diskReport = await buildDiskUsageReport(repoRoot, outputDirAbs, cfg);
  const diskJsonPath = path.join(outputDirAbs, `disk-usage-${makeSnapshotLabel(cmd, stage)}-${runId}.json`);
  const diskTxtPath = path.join(outputDirAbs, `disk-usage-${makeSnapshotLabel(cmd, stage)}-${runId}.txt`);
  await writeJson(diskJsonPath, diskReport);
  await writeText(diskTxtPath, renderDiskUsageText(diskReport));

  return { treePath, diskJsonPath, diskTxtPath, diskReport };
}

function resolveErrorLogDir(repoRoot) {
  const errorLog1 = path.join(repoRoot, "ErrorLog");
  const errorLog2 = path.join(repoRoot, "errorlog");
  return (async () => {
    if (await pathExists(errorLog1)) return errorLog1;
    if (await pathExists(errorLog2)) return errorLog2;
    await ensureDir(errorLog1);
    return errorLog1;
  })();
}

function parseArgs(argv) {
  const out = {
    cmd: argv[2] || "",
    dryRun: true,
    yes: false,
    ci: false,
    configPath: null,
    outputDir: null,
    quarantineDir: null,
    manifestPath: null,
    rootDir: null,
  };

  const args = argv.slice(3);
  for (let i = 0; i < args.length; i += 1) {
    const a = args[i];
    if (a === "--dry-run") {
      out.dryRun = true;
      continue;
    }
    if (a === "--no-dry-run") {
      out.dryRun = false;
      continue;
    }
    if (a === "--yes" || a === "-y") {
      out.yes = true;
      continue;
    }
    if (a === "--ci") {
      out.ci = true;
      continue;
    }
    if (a === "--config") {
      out.configPath = args[i + 1] || null;
      i += 1;
      continue;
    }
    if (a === "--output-dir") {
      out.outputDir = args[i + 1] || null;
      i += 1;
      continue;
    }
    if (a === "--quarantine-dir") {
      out.quarantineDir = args[i + 1] || null;
      i += 1;
      continue;
    }
    if (a === "--manifest") {
      out.manifestPath = args[i + 1] || null;
      i += 1;
      continue;
    }
    if (a === "--root") {
      out.rootDir = args[i + 1] || null;
      i += 1;
      continue;
    }
    throw new Error(`未知参数: ${a}`);
  }

  if (out.ci) {
    out.yes = true;
    if (!args.includes("--no-dry-run")) out.dryRun = true;
  }

  return out;
}

function usage() {
  return [
    "用法:",
    "  node scripts/clean-repo-junk.js scan [--config <json>] [--dry-run|--no-dry-run] [--output-dir <dir>]",
    "  node scripts/clean-repo-junk.js clean [--config <json>] [--dry-run|--no-dry-run] [--yes] [--ci] [--output-dir <dir>] [--quarantine-dir <dir>]",
    "  node scripts/clean-repo-junk.js restore [--manifest <json>] [--dry-run|--no-dry-run] [--yes] [--ci] [--output-dir <dir>]",
    "",
    "默认行为:",
    "  - 默认 dry-run：不会移动/删除任何内容",
    "  - clean 默认“移动隔离”到系统临时目录，并生成 manifest",
    "  - 输出写入 ErrorLog/cleanup/（若仓库中存在 errorlog/ 则复用）",
  ].join("\n");
}

function defaultConfig() {
  return {
    schemaVersion: 1,
    outputDir: "ErrorLog/cleanup",
    quarantineDir: null,
    reports: createDefaultSnapshotOptions(),
    whitelist: [],
    protect: [
      ".git",
      ".git/**",
      ".trae",
      ".trae/**",
      "src",
      "src/**",
      "app",
      "app/**",
      "docs",
      "docs/**",
      "config",
      "config/**",
      "gradle",
      "gradle/**",
      "CMakeLists.txt",
      "README.md",
      "README_BUILD.md",
      "DEVELOP.md",
      "CREDITS.md",
      "CHANGELOG.md",
      ".gitignore",
      "**/*.keystore",
      "**/*.jks",
      "**/*.p12",
      "**/*.pfx",
      "**/*.pem",
      "**/*.key",
      "**/*.crt",
      "**/*.cer",
      "**/*.env",
    ],
    protectedFileExtensions: [
      ".c",
      ".cc",
      ".cpp",
      ".cxx",
      ".h",
      ".hpp",
      ".java",
      ".kt",
      ".kts",
      ".xml",
      ".gradle",
      ".cmake",
      ".mk",
      ".sln",
      ".vcxproj",
      ".props",
      ".targets",
      ".filters",
      ".bat",
      ".cmd",
      ".ps1",
      ".sh",
      ".yml",
      ".yaml",
      ".ini",
      ".json",
    ],
    scan: {
      skipDirs: [".git", ".trae"],
    },
    targets: [
      {
        id: "build_host",
        kind: "path",
        pattern: "build_host",
        action: "quarantine",
        reason: "构建产物目录（默认可重建）",
      },
      {
        id: "build_android_rk3288",
        kind: "path",
        pattern: "build_android_rk3288",
        action: "quarantine",
        reason: "构建产物/第三方源码镜像（默认可重建或可再拉取）",
      },
      {
        id: "build_win_wcfr",
        kind: "path",
        pattern: "build_win_wcfr",
        action: "quarantine",
        reason: "Windows 构建产物目录（默认可重建）",
      },
      {
        id: "system_garbage",
        kind: "glob",
        pattern: "**/.DS_Store",
        action: "delete",
        reason: "系统垃圾文件",
      },
      {
        id: "system_garbage_win",
        kind: "glob",
        pattern: "**/Thumbs.db",
        action: "delete",
        reason: "系统垃圾文件",
      },
      {
        id: "system_garbage_win2",
        kind: "glob",
        pattern: "**/desktop.ini",
        action: "delete",
        reason: "系统垃圾文件",
      },
    ],
  };
}

async function loadConfig(repoRoot, configPath) {
  const base = defaultConfig();
  const p =
    configPath ||
    (await pathExists(path.join(repoRoot, "scripts", "clean-repo-junk.rules.json"))
      ? path.join(repoRoot, "scripts", "clean-repo-junk.rules.json")
      : null);
  if (!p) return { config: base, configPathUsed: null };

  const abs = path.isAbsolute(p) ? p : path.join(repoRoot, p);
  const raw = await fsp.readFile(abs, "utf8");
  const parsed = JSON.parse(raw);
  const merged = {
    ...base,
    ...parsed,
    scan: { ...base.scan, ...(parsed.scan || {}) },
    reports: {
      ...base.reports,
      ...(parsed.reports || {}),
      tree: { ...base.reports.tree, ...(parsed.reports?.tree || {}) },
      disk: { ...base.reports.disk, ...(parsed.reports?.disk || {}) },
    },
  };
  if (!merged || typeof merged !== "object") throw new Error("规则配置无效");
  if (!Array.isArray(merged.targets)) throw new Error("rules.targets 必须为数组");
  if (!Array.isArray(merged.whitelist)) throw new Error("rules.whitelist 必须为数组");
  if (!Array.isArray(merged.protect)) throw new Error("rules.protect 必须为数组");
  return { config: merged, configPathUsed: abs };
}

function toAbsPath(repoRoot, p, defaultBase) {
  if (!p) return null;
  if (path.isAbsolute(p)) return p;
  if (defaultBase) return path.join(defaultBase, p);
  return path.join(repoRoot, p);
}

function shouldSkipDirName(name, skipDirNames) {
  return skipDirNames.some((d) => d.toLowerCase() === name.toLowerCase());
}

function segmentGlobToRegExp(seg) {
  let out = "^";
  for (let i = 0; i < seg.length; i += 1) {
    const ch = seg[i];
    if (ch === "*") out += ".*";
    else if (ch === "?") out += ".";
    else out += escapeRegExp(ch);
  }
  out += "$";
  return new RegExp(out, "i");
}

async function expandGlob(repoRoot, pattern, skipDirNames) {
  const norm = normalizePattern(pattern);
  const segments = norm.split("/").filter((s) => s.length > 0);
  const results = new Set();

  async function rec(curAbs, idx) {
    if (idx >= segments.length) {
      if (await pathExists(curAbs)) results.add(curAbs);
      return;
    }

    const seg = segments[idx];
    if (seg === "**") {
      await rec(curAbs, idx + 1);
      let dirents;
      try {
        dirents = await fsp.readdir(curAbs, { withFileTypes: true });
      } catch {
        return;
      }
      for (const d of dirents) {
        if (!d.isDirectory()) continue;
        if (shouldSkipDirName(d.name, skipDirNames)) continue;
        await rec(path.join(curAbs, d.name), idx);
      }
      return;
    }

    const hasWild = seg.includes("*") || seg.includes("?");
    if (!hasWild) {
      const next = path.join(curAbs, seg);
      if (await pathExists(next)) await rec(next, idx + 1);
      return;
    }

    let dirents;
    try {
      dirents = await fsp.readdir(curAbs, { withFileTypes: true });
    } catch {
      return;
    }

    const segRe = segmentGlobToRegExp(seg);
    for (const d of dirents) {
      if (!segRe.test(d.name)) continue;
      await rec(path.join(curAbs, d.name), idx + 1);
    }
  }

  await rec(repoRoot, 0);
  return Array.from(results);
}

async function computeDirStats(dirAbs) {
  let bytes = 0;
  let filesCount = 0;
  let dirsCount = 0;
  const stack = [dirAbs];

  while (stack.length > 0) {
    const cur = stack.pop();
    let dirents;
    try {
      dirents = await fsp.readdir(cur, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const d of dirents) {
      const p = path.join(cur, d.name);
      try {
        if (d.isDirectory()) {
          dirsCount += 1;
          stack.push(p);
        } else {
          const st = await fsp.lstat(p);
          filesCount += 1;
          if (st.isFile()) bytes += st.size;
        }
      } catch {
      }
    }
  }

  return { bytes, filesCount, dirsCount };
}

async function getPathStats(absPath) {
  const st = await statSafe(absPath);
  if (!st) return null;
  if (st.isDirectory()) {
    const ds = await computeDirStats(absPath);
    return { kind: "dir", bytes: ds.bytes, filesCount: ds.filesCount, dirsCount: ds.dirsCount };
  }
  if (st.isFile()) {
    return { kind: "file", bytes: st.size, filesCount: 1, dirsCount: 0 };
  }
  return { kind: "other", bytes: 0, filesCount: 0, dirsCount: 0 };
}

function isProtectedByExtension(absPath, protectedExts) {
  const ext = path.extname(absPath).toLowerCase();
  return protectedExts.some((e) => e.toLowerCase() === ext);
}

function joinReasons(existing, add) {
  const s = new Set(existing || []);
  for (const r of add || []) s.add(r);
  return Array.from(s);
}

async function buildPlan(repoRoot, cfg) {
  const skipDirNames = Array.isArray(cfg.scan?.skipDirs) ? cfg.scan.skipDirs : [];
  const whitelist = cfg.whitelist.map(normalizePattern);
  const protect = cfg.protect.map(normalizePattern);
  const protectedExts = Array.isArray(cfg.protectedFileExtensions) ? cfg.protectedFileExtensions : [];

  const candidatesMap = new Map();

  for (const rule of cfg.targets) {
    if (!rule || typeof rule !== "object") continue;
    const kind = rule.kind;
    const pattern = rule.pattern;
    if (!kind || !pattern) continue;

    let matched = [];
    if (kind === "path") {
      const p = path.isAbsolute(pattern) ? pattern : path.join(repoRoot, pattern);
      if (await pathExists(p)) matched = [p];
    } else if (kind === "glob") {
      matched = await expandGlob(repoRoot, pattern, skipDirNames);
    } else {
      continue;
    }

    for (const absPath of matched) {
      if (!isSubPath(absPath, repoRoot)) continue;
      const rel = safeRelPath(absPath, repoRoot);
      const prev = candidatesMap.get(rel) || {
        absPath,
        relPath: rel,
        ruleIds: [],
        reasons: [],
        action: rule.action || "quarantine",
        allowInProtected: false,
      };
      prev.ruleIds = joinReasons(prev.ruleIds, [rule.id || "rule"]);
      prev.reasons = joinReasons(prev.reasons, [rule.reason || "命中规则"]);
      if (prev.action !== "delete" && rule.action === "delete") prev.action = "delete";
      if (rule.allowInProtected === true) prev.allowInProtected = true;
      candidatesMap.set(rel, prev);
    }
  }

  const candidates = [];
  const skipped = [];
  const refused = [];

  for (const item of candidatesMap.values()) {
    const rel = item.relPath;
    const whitelisted = whitelist.some((p) => isGlobMatch(rel, p));
    if (whitelisted) {
      skipped.push({ ...item, decision: "whitelist" });
      continue;
    }

    const protectedByPath = protect.some((p) => isGlobMatch(rel, p));
    const st = await getPathStats(item.absPath);
    if (!st) continue;

    const protectedByExt = st.kind === "file" && isProtectedByExtension(item.absPath, protectedExts);
    const protectedRefuse = protectedByPath && item.allowInProtected !== true;
    if (protectedRefuse || protectedByExt) {
      refused.push({
        ...item,
        kind: st.kind,
        bytes: st.bytes,
        decision: "refuse",
        refuseReason: protectedRefuse ? "命中保护规则" : "命中默认保护扩展名",
      });
      continue;
    }

    candidates.push({
      ...item,
      kind: st.kind,
      bytes: st.bytes,
      filesCount: st.filesCount,
      dirsCount: st.dirsCount,
      decision: "candidate",
    });
  }

  candidates.sort((a, b) => b.bytes - a.bytes);

  const totals = candidates.reduce(
    (acc, it) => {
      acc.items += 1;
      acc.bytes += it.bytes || 0;
      acc.files += it.filesCount || 0;
      acc.dirs += it.dirsCount || 0;
      return acc;
    },
    { items: 0, bytes: 0, files: 0, dirs: 0 }
  );

  return { candidates, skipped, refused, totals };
}

async function promptYesNo(question) {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  const answer = await new Promise((resolve) => rl.question(question, resolve));
  rl.close();
  const a = String(answer || "").trim().toLowerCase();
  return a === "y" || a === "yes";
}

async function copyRecursive(src, dst) {
  const st = await fsp.lstat(src);
  if (st.isDirectory()) {
    await ensureDir(dst);
    const dirents = await fsp.readdir(src, { withFileTypes: true });
    for (const d of dirents) {
      await copyRecursive(path.join(src, d.name), path.join(dst, d.name));
    }
    return;
  }
  await ensureDir(path.dirname(dst));
  await fsp.copyFile(src, dst);
}

async function removeRecursive(p) {
  await fsp.rm(p, { recursive: true, force: true });
}

async function movePath(src, dst) {
  await ensureDir(path.dirname(dst));
  try {
    await fsp.rename(src, dst);
    return;
  } catch (e) {
    if (e && (e.code === "EXDEV" || e.code === "EPERM")) {
      await copyRecursive(src, dst);
      await removeRecursive(src);
      return;
    }
    throw e;
  }
}

async function uniqueQuarantinePath(quarantineBase, relPosixPath) {
  const relFs = relPosixPath.split("/").join(path.sep);
  const base = path.join(quarantineBase, "items", relFs);
  if (!(await pathExists(base))) return base;

  for (let i = 1; i <= 9999; i += 1) {
    const cand = `${base}__${i}`;
    if (!(await pathExists(cand))) return cand;
  }
  throw new Error(`无法生成唯一隔离路径: ${base}`);
}

async function writeJson(filePath, obj) {
  await ensureDir(path.dirname(filePath));
  await fsp.writeFile(filePath, JSON.stringify(obj, null, 2), "utf8");
}

async function writeText(filePath, text) {
  await ensureDir(path.dirname(filePath));
  await fsp.writeFile(filePath, text, "utf8");
}

function renderPlanText(cmd, runId, plan, outputDir, manifestPath) {
  const lines = [];
  lines.push(`runId: ${runId}`);
  lines.push(`cmd: ${cmd}`);
  lines.push(`候选项: ${plan.totals.items} 项，预估释放: ${formatBytes(plan.totals.bytes)}`);
  lines.push(`输出目录: ${outputDir}`);
  if (manifestPath) lines.push(`manifest: ${manifestPath}`);
  lines.push("");
  if (plan.refused.length > 0) {
    lines.push(`拒绝处理（保护规则命中）: ${plan.refused.length} 项`);
    for (const it of plan.refused.slice(0, 50)) {
      lines.push(`- ${it.relPath} (${formatBytes(it.bytes)}): ${it.refuseReason}`);
    }
    if (plan.refused.length > 50) lines.push(`- ... 省略 ${plan.refused.length - 50} 项`);
    lines.push("");
  }
  if (plan.skipped.length > 0) {
    lines.push(`跳过（白名单命中）: ${plan.skipped.length} 项`);
    for (const it of plan.skipped.slice(0, 50)) {
      lines.push(`- ${it.relPath}`);
    }
    if (plan.skipped.length > 50) lines.push(`- ... 省略 ${plan.skipped.length - 50} 项`);
    lines.push("");
  }
  lines.push("候选清理项（按大小降序，最多 200 项）:");
  for (const it of plan.candidates.slice(0, 200)) {
    lines.push(`- ${it.relPath} (${formatBytes(it.bytes)}) [${it.action}] (${it.ruleIds.join(", ")})`);
  }
  if (plan.candidates.length > 200) lines.push(`- ... 省略 ${plan.candidates.length - 200} 项`);
  return lines.join("\n");
}

async function findLatestManifest(outputDir) {
  let dirents;
  try {
    dirents = await fsp.readdir(outputDir, { withFileTypes: true });
  } catch {
    return null;
  }
  const candidates = dirents
    .filter((d) => d.isFile() && /^clean-manifest-.*\.json$/i.test(d.name))
    .map((d) => path.join(outputDir, d.name));

  let best = null;
  let bestMtime = 0;
  for (const p of candidates) {
    try {
      const st = await fsp.stat(p);
      const t = st.mtimeMs || 0;
      if (t > bestMtime) {
        bestMtime = t;
        best = p;
      }
    } catch {
    }
  }
  return best;
}

async function detectRepoRoot(startDir) {
  let cur = path.resolve(startDir);
  for (let i = 0; i < 20; i += 1) {
    const marker = path.join(cur, ".trae", "specs", "clean-repo-junk", "spec.md");
    if (await pathExists(marker)) return cur;
    const parent = path.dirname(cur);
    if (parent === cur) break;
    cur = parent;
  }
  return path.resolve(startDir);
}

async function runScan(repoRoot, cfg, outputDir, runId) {
  const plan = await buildPlan(repoRoot, cfg);
  const report = {
    schemaVersion: 1,
    createdAt: new Date().toISOString(),
    repoRoot,
    runId,
    cmd: "scan",
    totals: plan.totals,
    candidates: plan.candidates,
    refused: plan.refused,
    skipped: plan.skipped,
  };

  const jsonPath = path.join(outputDir, `scan-report-${runId}.json`);
  const txtPath = path.join(outputDir, `scan-report-${runId}.txt`);
  await writeJson(jsonPath, report);
  await writeText(txtPath, renderPlanText("scan", runId, plan, outputDir, null));
  return { plan, reportPath: jsonPath };
}

async function runClean(repoRoot, cfg, outputDir, quarantineDirBase, runId, dryRun, yes) {
  const plan = await buildPlan(repoRoot, cfg);
  const quarantineDir =
    quarantineDirBase ||
    path.join(os.tmpdir(), "rk3288_opencv-cleanup-quarantine", runId);
  await ensureDir(quarantineDir);

  const manifest = {
    schemaVersion: 1,
    createdAt: new Date().toISOString(),
    repoRoot,
    runId,
    cmd: "clean",
    dryRun,
    quarantineDir,
    items: [],
    totals: {
      plannedItems: plan.candidates.length,
      plannedBytes: plan.totals.bytes,
      movedItems: 0,
      movedBytes: 0,
      deletedItems: 0,
      deletedBytes: 0,
      skippedItems: plan.skipped.length,
      refusedItems: plan.refused.length,
      failedItems: 0,
    },
  };

  const manifestPath = path.join(outputDir, `clean-manifest-${runId}.json`);
  const txtPath = path.join(outputDir, `clean-report-${runId}.txt`);

  if (!dryRun && !yes) {
    const ok = await promptYesNo(
      `将执行清理（默认隔离），候选 ${plan.totals.items} 项，预估释放 ${formatBytes(plan.totals.bytes)}。继续？(y/N): `
    );
    if (!ok) {
      await writeText(
        txtPath,
        renderPlanText("clean(取消)", runId, plan, outputDir, manifestPath)
      );
      await writeJson(manifestPath, manifest);
      return { plan, manifestPath, cancelled: true };
    }
  }

  for (const it of plan.candidates) {
    const entry = {
      relPath: it.relPath,
      originalPath: it.absPath,
      quarantinePath: null,
      kind: it.kind,
      bytes: it.bytes,
      action: it.action || "quarantine",
      ruleIds: it.ruleIds || [],
      reasons: it.reasons || [],
      status: dryRun ? "planned" : "pending",
      error: null,
    };

    if (dryRun) {
      if (entry.action === "quarantine") {
        entry.quarantinePath = await uniqueQuarantinePath(quarantineDir, it.relPath);
      }
      manifest.items.push(entry);
      continue;
    }

    try {
      if (entry.action === "delete") {
        await removeRecursive(it.absPath);
        entry.status = "deleted";
        manifest.totals.deletedItems += 1;
        manifest.totals.deletedBytes += it.bytes || 0;
      } else {
        entry.quarantinePath = await uniqueQuarantinePath(quarantineDir, it.relPath);
        await movePath(it.absPath, entry.quarantinePath);
        entry.status = "moved";
        manifest.totals.movedItems += 1;
        manifest.totals.movedBytes += it.bytes || 0;
      }
    } catch (e) {
      entry.status = "failed";
      entry.error = e && e.message ? e.message : String(e);
      manifest.totals.failedItems += 1;
    }

    manifest.items.push(entry);
  }

  for (const it of plan.refused) {
    manifest.items.push({
      relPath: it.relPath,
      originalPath: it.absPath,
      quarantinePath: null,
      kind: it.kind,
      bytes: it.bytes,
      action: it.action || "quarantine",
      ruleIds: it.ruleIds || [],
      reasons: joinReasons(it.reasons || [], [it.refuseReason || "命中保护规则"]),
      status: "refused",
      error: it.refuseReason || "命中保护规则",
    });
  }

  for (const it of plan.skipped) {
    manifest.items.push({
      relPath: it.relPath,
      originalPath: it.absPath,
      quarantinePath: null,
      kind: null,
      bytes: null,
      action: it.action || "quarantine",
      ruleIds: it.ruleIds || [],
      reasons: joinReasons(it.reasons || [], ["白名单命中"]),
      status: "skipped",
      error: null,
    });
  }

  await writeJson(manifestPath, manifest);
  await writeText(txtPath, renderPlanText(dryRun ? "clean(dry-run)" : "clean", runId, plan, outputDir, manifestPath));
  return { plan, manifestPath, cancelled: false };
}

async function runRestore(repoRoot, cfg, outputDir, manifestPathArg, runId, dryRun, yes) {
  const manifestPath =
    manifestPathArg ||
    (await findLatestManifest(outputDir));
  if (!manifestPath) {
    throw new Error("未找到 manifest，请使用 --manifest 指定 clean-manifest-*.json 路径");
  }

  const absManifest = path.isAbsolute(manifestPath) ? manifestPath : path.join(repoRoot, manifestPath);
  const raw = await fsp.readFile(absManifest, "utf8");
  const manifest = JSON.parse(raw);
  if (!manifest || typeof manifest !== "object" || !Array.isArray(manifest.items)) {
    throw new Error("manifest 文件格式无效");
  }

  const restoreItems = manifest.items.filter((it) => it && it.action === "quarantine" && it.quarantinePath && (it.status === "moved" || it.status === "planned" || it.status === "pending"));

  const restoreTotals = restoreItems.reduce(
    (acc, it) => {
      acc.items += 1;
      acc.bytes += Number(it.bytes) || 0;
      return acc;
    },
    { items: 0, bytes: 0 }
  );

  const report = {
    schemaVersion: 1,
    createdAt: new Date().toISOString(),
    repoRoot,
    runId,
    cmd: "restore",
    dryRun,
    manifestPath: absManifest,
    totals: {
      plannedItems: restoreTotals.items,
      plannedBytes: restoreTotals.bytes,
      restoredItems: 0,
      restoredBytes: 0,
      failedItems: 0,
      skippedItems: 0,
    },
    items: [],
  };

  const jsonPath = path.join(outputDir, `restore-report-${runId}.json`);
  const txtPath = path.join(outputDir, `restore-report-${runId}.txt`);

  if (!dryRun && !yes) {
    const ok = await promptYesNo(
      `将执行恢复（从隔离区移回原位置），共 ${restoreTotals.items} 项（${formatBytes(restoreTotals.bytes)}）。继续？(y/N): `
    );
    if (!ok) {
      await writeJson(jsonPath, report);
      await writeText(txtPath, `runId: ${runId}\ncmd: restore(取消)\nmanifest: ${absManifest}\n`);
      return { reportPath: jsonPath, cancelled: true };
    }
  }

  for (const it of restoreItems) {
    const original = it.originalPath;
    const quarantine = it.quarantinePath;
    const rel = safeRelPath(original, repoRoot);
    const outItem = {
      relPath: rel,
      originalPath: original,
      quarantinePath: quarantine,
      bytes: it.bytes || 0,
      status: dryRun ? "planned" : "pending",
      error: null,
    };

    if (dryRun) {
      report.items.push(outItem);
      continue;
    }

    try {
      if (await pathExists(original)) {
        outItem.status = "skipped";
        outItem.error = "目标路径已存在，跳过恢复";
        report.totals.skippedItems += 1;
        report.items.push(outItem);
        continue;
      }
      if (!(await pathExists(quarantine))) {
        outItem.status = "failed";
        outItem.error = "隔离区路径不存在";
        report.totals.failedItems += 1;
        report.items.push(outItem);
        continue;
      }
      await movePath(quarantine, original);
      outItem.status = "restored";
      report.totals.restoredItems += 1;
      report.totals.restoredBytes += Number(it.bytes) || 0;
    } catch (e) {
      outItem.status = "failed";
      outItem.error = e && e.message ? e.message : String(e);
      report.totals.failedItems += 1;
    }
    report.items.push(outItem);
  }

  await writeJson(jsonPath, report);
  const lines = [];
  lines.push(`runId: ${runId}`);
  lines.push(`cmd: restore`);
  lines.push(`manifest: ${absManifest}`);
  lines.push(`计划恢复: ${restoreTotals.items} 项，${formatBytes(restoreTotals.bytes)}`);
  lines.push(`实际恢复: ${report.totals.restoredItems} 项，${formatBytes(report.totals.restoredBytes)}`);
  lines.push(`失败: ${report.totals.failedItems} 项，跳过: ${report.totals.skippedItems} 项`);
  lines.push("");
  for (const r of report.items.slice(0, 200)) {
    lines.push(`- ${r.relPath}: ${r.status}${r.error ? ` (${r.error})` : ""}`);
  }
  if (report.items.length > 200) lines.push(`- ... 省略 ${report.items.length - 200} 项`);
  await writeText(txtPath, lines.join("\n"));
  return { reportPath: jsonPath, cancelled: false, manifestPath: absManifest };
}

async function main() {
  const args = parseArgs(process.argv);
  if (!args.cmd || !["scan", "clean", "restore"].includes(args.cmd)) {
    console.error(usage());
    process.exitCode = 2;
    return;
  }

  const repoRoot = await detectRepoRoot(args.rootDir || process.cwd());
  const { config: cfg, configPathUsed } = await loadConfig(repoRoot, args.configPath);

  const errorLogDir = await resolveErrorLogDir(repoRoot);
  const defaultOutputDir = path.join(errorLogDir, "cleanup");
  const outputDirInput = args.outputDir || cfg.outputDir;
  const outputDir =
    outputDirInput && outputDirInput !== "ErrorLog/cleanup"
      ? toAbsPath(repoRoot, outputDirInput, null)
      : defaultOutputDir;
  const normalizedOutputDir = path.isAbsolute(outputDir) ? outputDir : path.join(repoRoot, outputDir);
  await ensureDir(normalizedOutputDir);

  const quarantineDirBase = toAbsPath(repoRoot, args.quarantineDir || cfg.quarantineDir, null);
  const runId = createRunId();

  const metaPath = path.join(normalizedOutputDir, `run-meta-${runId}.json`);
  const runMeta = {
    schemaVersion: 1,
    createdAt: new Date().toISOString(),
    repoRoot,
    runId,
    cmd: args.cmd,
    dryRun: args.dryRun,
    yes: args.yes,
    ci: args.ci,
    configPathUsed,
    outputDir: normalizedOutputDir,
    quarantineDirBase,
  };
  await writeJson(metaPath, runMeta);

  let before = null;
  try {
    before = await writeWorkspaceSnapshots(repoRoot, normalizedOutputDir, cfg, runId, args.cmd, "before");
  } catch (e) {
    await writeSnapshotError(normalizedOutputDir, runId, args.cmd, "before", e);
  }

  if (args.cmd === "scan") {
    const r = await runScan(repoRoot, cfg, normalizedOutputDir, runId);
    if (before) {
      const afterTree = path.join(normalizedOutputDir, `tree-${makeSnapshotLabel(args.cmd, "after")}-${runId}.txt`);
      const afterDiskJson = path.join(
        normalizedOutputDir,
        `disk-usage-${makeSnapshotLabel(args.cmd, "after")}-${runId}.json`
      );
      const afterDiskTxt = path.join(
        normalizedOutputDir,
        `disk-usage-${makeSnapshotLabel(args.cmd, "after")}-${runId}.txt`
      );
      const treeContent = await fsp.readFile(before.treePath, "utf8");
      await writeText(afterTree, treeContent);
      await writeJson(afterDiskJson, before.diskReport);
      await writeText(afterDiskTxt, renderDiskUsageText(before.diskReport));

      const diff = diffDiskReports(before.diskReport, before.diskReport);
      const summaryJson = path.join(normalizedOutputDir, `snapshot-summary-${runId}.json`);
      const summaryTxt = path.join(normalizedOutputDir, `snapshot-summary-${runId}.txt`);
      await writeJson(summaryJson, { ...diff, runMeta });
      await writeText(summaryTxt, renderSnapshotSummaryText(runMeta, before.diskReport, before.diskReport, diff));
    }
    console.log(`scan 完成: ${r.reportPath}`);
    console.log(`候选项: ${r.plan.totals.items}，预估释放: ${formatBytes(r.plan.totals.bytes)}`);
    console.log(args.dryRun ? "dry-run: 未进行任何移动/删除" : "提示: scan 不执行移动/删除");
    return;
  }

  if (args.cmd === "clean") {
    const r = await runClean(repoRoot, cfg, normalizedOutputDir, quarantineDirBase, runId, args.dryRun, args.yes);
    let after = null;
    if (!args.dryRun && !r.cancelled && before) {
      try {
        after = await writeWorkspaceSnapshots(repoRoot, normalizedOutputDir, cfg, runId, args.cmd, "after");
      } catch (e) {
        await writeSnapshotError(normalizedOutputDir, runId, args.cmd, "after", e);
      }
    }
    if (before) {
      const summaryJson = path.join(normalizedOutputDir, `snapshot-summary-${runId}.json`);
      const summaryTxt = path.join(normalizedOutputDir, `snapshot-summary-${runId}.txt`);
      if (after) {
        const diff = diffDiskReports(before.diskReport, after.diskReport);
        await writeJson(summaryJson, { ...diff, runMeta });
        await writeText(summaryTxt, renderSnapshotSummaryText(runMeta, before.diskReport, after.diskReport, diff));
      } else {
        await writeJson(summaryJson, { schemaVersion: 1, runMeta, before: before.diskReport, after: null });
        await writeText(summaryTxt, renderSnapshotSummaryText(runMeta, before.diskReport, null, null));
      }
    }
    console.log(`clean 完成: ${r.manifestPath}`);
    console.log(`候选项: ${r.plan.totals.items}，预估释放: ${formatBytes(r.plan.totals.bytes)}`);
    console.log(args.dryRun ? "dry-run: 未进行任何移动/删除" : "已执行清理（默认隔离/或按规则删除）");
    if (r.cancelled) {
      console.log("已取消执行");
      process.exitCode = 3;
    }
    return;
  }

  if (args.cmd === "restore") {
    const r = await runRestore(repoRoot, cfg, normalizedOutputDir, args.manifestPath, runId, args.dryRun, args.yes);
    let after = null;
    if (!args.dryRun && !r.cancelled && before) {
      try {
        after = await writeWorkspaceSnapshots(repoRoot, normalizedOutputDir, cfg, runId, args.cmd, "after");
      } catch (e) {
        await writeSnapshotError(normalizedOutputDir, runId, args.cmd, "after", e);
      }
    }
    if (before) {
      const summaryJson = path.join(normalizedOutputDir, `snapshot-summary-${runId}.json`);
      const summaryTxt = path.join(normalizedOutputDir, `snapshot-summary-${runId}.txt`);
      if (after) {
        const diff = diffDiskReports(before.diskReport, after.diskReport);
        await writeJson(summaryJson, { ...diff, runMeta });
        await writeText(summaryTxt, renderSnapshotSummaryText(runMeta, before.diskReport, after.diskReport, diff));
      } else {
        await writeJson(summaryJson, { schemaVersion: 1, runMeta, before: before.diskReport, after: null });
        await writeText(summaryTxt, renderSnapshotSummaryText(runMeta, before.diskReport, null, null));
      }
    }
    console.log(`restore 完成: ${r.reportPath}`);
    console.log(args.dryRun ? "dry-run: 未进行任何移动/删除" : "已执行恢复");
    if (r.cancelled) {
      console.log("已取消执行");
      process.exitCode = 3;
    }
    return;
  }
}

main().catch((e) => {
  console.error(e && e.stack ? e.stack : String(e));
  process.exitCode = 1;
});

