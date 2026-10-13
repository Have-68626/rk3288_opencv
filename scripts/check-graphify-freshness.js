/**
 * @file check-graphify-freshness.js
 * @brief graphify 知识图谱新鲜度自检（**本机手动工具，未接入 CI**）
 *
 * 背景：`graphify-out/graph.json` 记录建图时的提交（`built_at_commit`），但 graphify CLI
 * （0.9.64）**没有任何能在本仓判定「图谱是否落后于 HEAD」的子命令** —— 逐个核对过：
 *   · `check-update <path>`（0.9.64 **确有**此子命令）只看 graph.json 里的 `needs_update` 标志，
 *     而本仓 graph.json 的顶层键只有
 *     `directed` / `multigraph` / `graph` / `nodes` / `links` / `hyperedges` / `built_at_commit`
 *     ——**没有 `needs_update` 字段**（实测 `node -e` 读为 undefined）⇒ 它在本仓恒无判据可依，
 *     与「HEAD 落后几个提交」是两回事；
 *   · 其余子命令为 `path` / `explain` / `affected` / `god-nodes` / `query` / `diagnose` / `tree` /
 *     `cluster-only` / `label` / `watch` / `update` / `merge-graphs` / `add` / `clone` / `install`
 *     ——**都不比较 HEAD**；
 *   · **不存在** `status` 或其它等价的新鲜度子命令。
 *   ⇒ 「图谱是否落后于 HEAD」此前只能靠人肉比对，必然漏。
 *   （订正记录：本行此前写「**没有任何 status/check/新鲜度子命令**」，属**过强**表述——
 *   `check-update` 确实存在，只是在本仓无从判定；现按逐条核对结果改写。）
 *
 * 本脚本只做四件事：
 *   1. `graphify-out/graph.json` 不存在 → 打印 `GRAPHIFY_NOT_BUILT`，退出码 0
 *      （图谱是本机可选检索件，不是交付物，缺了不算错误）；
 *   2. 用 `git merge-base --is-ancestor` + `git rev-list --count` 判定 `built_at_commit`
 *      落后 HEAD 多少提交，落后 ≥1 时打印落后的提交清单与修复提示；
 *   3. 交叉校验 `graphify-out/.graphify_health.json` 的 `node_count` 与 `graph.json` 的
 *      `nodes.length`，不一致即报「产物目录内自相矛盾 —— 副产物已陈旧」（此判据**不依赖 git**，
 *      可在无 git 环境下独立成立）；
 *   4. 全部检查**只打印、不阻断**（非零退出码只来自「执行性错误」，不来自任何检查结论 ——
 *      见下方退出码表）。
 *
 * ⚠️ 为什么【不】放进 `tests/scripts/`：那里被 `repo-hygiene` job 以目录级 glob
 *    （`node --test tests/scripts/*.test.js`）整体接入 CI，而 `graphify-out/` 在 `.gitignore`
 *    中被忽略 ⇒ CI 全新检出后该目录**不存在**，任何断言只会恒红或恒绿，纯噪声。
 *    故本脚本做成与 `scripts/check-raii-violations.sh` 同类的「存在但未接入 CI」手动工具。
 *
 * 退出码（刻意保守）——只有 0 / 1 两个码，与**全部 6 个 verdict** 的对应关系如下（穷举；
 * 判定实现见本文件 `evaluateFreshness()`）：
 *   verdict                     退出码  语义
 *   GRAPHIFY_NOT_BUILT             0    graph.json 不存在（本机可选件，不算错误）
 *   GRAPHIFY_UNREADABLE            1    graph.json 存在但读不动 / JSON 语法错
 *   GRAPHIFY_FRESHNESS_UNKNOWN     0    无法比较 HEAD（git 不可用 / built_at_commit 非 40 位 SHA）
 *   GRAPHIFY_FRESH                0    built_at_commit == HEAD
 *   GRAPHIFY_STALE                 0    落后 HEAD ≥ 1 个提交（只提示，不阻断）
 *   GRAPHIFY_DIVERGED              0    built_at_commit 不是 HEAD 的祖先（历史被改写）
 * 另有三条**横幅行**（不是 verdict，与上面并列输出、同样不改退出码）：
 *   GRAPHIFY_HEALTH_STALE（副产物自相矛盾）/ GRAPHIFY_HEALTH_OK / GRAPHIFY_HEALTH_UNCHECKED。
 * 故除 `GRAPHIFY_UNREADABLE` 为 1 外，**其余 verdict 一律 0**；`main()` 另有两处直接 `exit 1`：
 * `--repo-root` 指向不存在或**非目录**的路径、以及顶层未捕获异常。
 * 为什么不给「副产物陈旧」设非零退出码：它本来就**很可能长期为真**（见下方实测），
 * 一个恒非零的退出码只会训练人忽略本脚本；「报得响一些」由横幅文案承担，不靠退出码。
 * 若将来有人要把它接进 CI，必须先重新论证非零退出码的必要性。
 *
 * 用法：
 *   node scripts/check-graphify-freshness.js [--repo-root <dir>]
 *
 * 实测（2026-09-23，本机）：graph.json 的 built_at_commit = c250b2952605c7d2f7aeccfbb0d01ba481079400
 * （即 c250b29），落后 HEAD 3 个提交；且 .graphify_health.json（mtime 2026-08-19，node_count=7839）
 * 与 graph.json（nodes.length=10279）对不上 —— 即在无人察觉的情况下，产物目录里已存在一个
 * 自相矛盾的副产物。
 */
"use strict";

const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

const GRAPH_REL = "graphify-out/graph.json";
const HEALTH_REL = "graphify-out/.graphify_health.json";

/** git 对象名形态：只接受完整 40 位十六进制，避免把任意字符串拼进 git 命令 */
const FULL_SHA_RE = /^[0-9a-f]{40}$/;

/** 落后的提交清单最多打印多少条（图谱可能落后很多，避免刷屏） */
const MAX_AHEAD_LINES = 20;

/**
 * 读并解析一个 JSON 文件，永不抛异常。
 * @param {string} absPath
 * @returns {{exists:boolean, ok:boolean, value:object|null, error:string|null, mtimeIso:string|null}}
 */
function tryReadJson(absPath) {
  if (!fs.existsSync(absPath)) {
    return { exists: false, ok: false, value: null, error: null, mtimeIso: null };
  }
  try {
    const value = JSON.parse(fs.readFileSync(absPath, "utf8"));
    return {
      exists: true,
      ok: true,
      value,
      error: null,
      mtimeIso: fs.statSync(absPath).mtime.toISOString(),
    };
  } catch (e) {
    return {
      exists: true,
      ok: false,
      value: null,
      error: String((e && e.message) || e),
      mtimeIso: null,
    };
  }
}

/**
 * 读取 graph.json 关键字段。
 * @param {string} repoRoot
 * @returns {{exists:boolean, ok:boolean, builtAtCommit:string|null, nodeCount:number|null, error:string|null}}
 */
function readGraphSnapshot(repoRoot) {
  const r = tryReadJson(path.join(repoRoot, GRAPH_REL));
  if (!r.exists) return { exists: false, ok: false, builtAtCommit: null, nodeCount: null, error: null };
  if (!r.ok) return { exists: true, ok: false, builtAtCommit: null, nodeCount: null, error: r.error };
  const g = r.value || {};
  return {
    exists: true,
    ok: true,
    builtAtCommit: typeof g.built_at_commit === "string" ? g.built_at_commit : null,
    nodeCount: Array.isArray(g.nodes) ? g.nodes.length : null,
    error: null,
  };
}

/**
 * 读取 .graphify_health.json 关键字段。
 * @param {string} repoRoot
 * @returns {{exists:boolean, ok:boolean, nodeCount:number|null, mtimeIso:string|null, error:string|null}}
 */
function readHealthSnapshot(repoRoot) {
  const r = tryReadJson(path.join(repoRoot, HEALTH_REL));
  if (!r.exists) {
    return { exists: false, ok: false, nodeCount: null, mtimeIso: null, error: null };
  }
  if (!r.ok) {
    return { exists: true, ok: false, nodeCount: null, mtimeIso: null, error: r.error };
  }
  const h = r.value || {};
  return {
    exists: true,
    ok: true,
    nodeCount: typeof h.node_count === "number" ? h.node_count : null,
    mtimeIso: r.mtimeIso,
    error: null,
  };
}

/**
 * 交叉校验两份副产物是否互相一致（**不依赖 git**）。
 * @param {{exists:boolean, ok:boolean, nodeCount:number|null}} graph
 * @param {{exists:boolean, ok:boolean, nodeCount:number|null, mtimeIso:string|null}} health
 * @returns {{checked:boolean, consistent:boolean|null, reason:string|null}}
 *   checked=false 表示无判据可查（缺文件 / 读不动 / 字段缺失）—— 不报错，但也不谎报「一致」。
 */
function compareProducedArtifacts(graph, health) {
  if (!graph.exists || !graph.ok || typeof graph.nodeCount !== "number") {
    return { checked: false, consistent: null, reason: "graph.json 缺失或不可解析，无判据" };
  }
  if (!health.exists || !health.ok || typeof health.nodeCount !== "number") {
    return { checked: false, consistent: null, reason: ".graphify_health.json 缺失或不可解析，无判据" };
  }
  if (graph.nodeCount === health.nodeCount) {
    return { checked: true, consistent: true, reason: null };
  }
  return {
    checked: true,
    consistent: false,
    reason:
      `${HEALTH_REL}(mtime=${health.mtimeIso || "未知"}) 的 node_count=${health.nodeCount} ` +
      `与 ${GRAPH_REL} 的 nodes.length=${graph.nodeCount} 不一致`,
  };
}

/**
 * 执行 git 命令；失败不抛异常，返回 null 供调用方按上下文降级处理。
 * @param {string} repoRoot
 * @param {string[]} args
 * @returns {string|null} stdout（trim 后）
 */
function runGit(repoRoot, args) {
  try {
    return execFileSync("git", args, {
      cwd: repoRoot,
      encoding: "utf8",
      stdio: ["ignore", "pipe", "pipe"],
    }).trim();
  } catch (e) {
    return null;
  }
}

/**
 * 判定 `builtAtCommit` 相对 HEAD 的落后情况。
 * @param {string} repoRoot
 * @param {string|null} builtAtCommit
 * @returns {{available:boolean, headSha:string|null, isAncestor:boolean|null, aheadCount:number|null,
 *            aheadList:string[], error:string|null}}
 */
function readGitDelta(repoRoot, builtAtCommit) {
  const unavailable = {
    available: false,
    headSha: null,
    isAncestor: null,
    aheadCount: null,
    aheadList: [],
    error: null,
  };
  if (!builtAtCommit || !FULL_SHA_RE.test(builtAtCommit)) {
    return { ...unavailable, error: builtAtCommit ? `built_at_commit 非 40 位 SHA：${builtAtCommit}` : null };
  }
  const headSha = runGit(repoRoot, ["rev-parse", "HEAD"]);
  if (!headSha) return { ...unavailable, error: "git 不可用或当前目录不是 git 仓库" };

  if (builtAtCommit === headSha) {
    return { available: true, headSha, isAncestor: true, aheadCount: 0, aheadList: [], error: null };
  }
  // `git merge-base --is-ancestor` 以退出码表达结论：0=是祖先，1=不是（上游被改写/force-push）。
  let isAncestor = false;
  try {
    execFileSync("git", ["merge-base", "--is-ancestor", builtAtCommit, "HEAD"], {
      cwd: repoRoot,
      stdio: ["ignore", "pipe", "pipe"],
    });
    isAncestor = true;
  } catch (e) {
    isAncestor = false;
  }

  const countStr = runGit(repoRoot, ["rev-list", "--count", `${builtAtCommit}..HEAD`]);
  const aheadCount = countStr !== null && /^\d+$/.test(countStr) ? Number(countStr) : null;
  const listStr = runGit(repoRoot, ["log", "--oneline", `-n${MAX_AHEAD_LINES}`, `${builtAtCommit}..HEAD`]);
  const aheadList = listStr ? listStr.split(/\r?\n/).filter((l) => l.trim() !== "") : [];

  return { available: true, headSha, isAncestor, aheadCount, aheadList, error: null };
}

/**
 * 纯决策函数：把三份快照归约为判定结论 + 打印行 + 退出码。无 IO、无副作用，便于单测与推演。
 * @param {object} input
 * @returns {{verdict:string, lines:string[], exitCode:number}}
 */
function evaluateFreshness(input) {
  const { graph, health, git } = input;
  const lines = [];
  const artifacts = compareProducedArtifacts(graph, health);

  // ① 图谱未构建：本机可选件，正常结束。
  if (!graph.exists) {
    lines.push(`GRAPHIFY_NOT_BUILT ${GRAPH_REL} 不存在（图谱为本机可选检索件，非错误）`);
    lines.push("  提示：本机需要图谱时运行 `graphify update .` 生成。");
    return { verdict: "GRAPHIFY_NOT_BUILT", lines, exitCode: 0 };
  }

  // ② 图谱在但读不动：这是**执行出错**（不是「陈旧」），退出码 1。
  if (!graph.ok) {
    lines.push(`GRAPHIFY_UNREADABLE ${GRAPH_REL} 存在但无法解析：${graph.error}`);
    return { verdict: "GRAPHIFY_UNREADABLE", lines, exitCode: 1 };
  }

  // ③ 落后判定。
  let verdict;
  if (!git.available) {
    verdict = "GRAPHIFY_FRESHNESS_UNKNOWN";
    lines.push(
      `GRAPHIFY_FRESHNESS_UNKNOWN built_at_commit=${graph.builtAtCommit || "缺失"}` +
        `${git.error ? ` —— ${git.error}` : ""}（无法比较 HEAD，仅提示）`
    );
  } else if (git.aheadCount === 0) {
    verdict = "GRAPHIFY_FRESH";
    lines.push(
      `GRAPHIFY_FRESH built_at_commit=${graph.builtAtCommit.slice(0, 7)} == head=${git.headSha.slice(0, 7)}`
    );
  } else if (git.isAncestor) {
    verdict = "GRAPHIFY_STALE";
    lines.push(
      `GRAPHIFY_STALE built_at_commit=${graph.builtAtCommit.slice(0, 7)} head=${git.headSha.slice(0, 7)} ` +
        `behind=${git.aheadCount === null ? "未知" : git.aheadCount}`
    );
    for (const l of git.aheadList) lines.push(`  [BEHIND] ${l}`);
    if (git.aheadCount !== null && git.aheadCount > git.aheadList.length) {
      lines.push(`  [BEHIND] …（共 ${git.aheadCount} 个提交，此处仅列前 ${git.aheadList.length} 个）`);
    }
    lines.push("  提示：图谱落后于 HEAD，请在本机运行 `graphify update .`（本脚本只提示、不阻断）。");
  } else {
    // 非祖先：built_at_commit 不在 HEAD 的历史里（force-push / 改了分支基准）——
    // 此时「落后几个提交」没有意义，不能说成 STALE 的具体数字。
    verdict = "GRAPHIFY_DIVERGED";
    lines.push(
      `GRAPHIFY_DIVERGED built_at_commit=${graph.builtAtCommit.slice(0, 7)} 不是 head=${git.headSha.slice(0, 7)} ` +
        `的祖先（历史被改写？）——落后提交数无意义，请直接重新建图。`
    );
  }

  // ④ 副产物自相矛盾：**报得响一些**，但不改变退出码（理由见文件头）。
  if (artifacts.checked && artifacts.consistent === false) {
    lines.push("");
    lines.push("==================================================================");
    lines.push("GRAPHIFY_HEALTH_STALE —— 产物目录内自相矛盾：副产物已陈旧");
    lines.push(`  ${artifacts.reason}`);
    lines.push("  含义：`.graphify_health.json` 未被最近一次建图重写，它描述的**不是**当前 graph.json。");
    lines.push("  影响：任何读 `.graphify_health.json` 计数的人会得到错误结论（此前无人察觉）。");
    lines.push("  处置：在本机运行 `graphify update .` 后复核；若它仍不刷新，则该副产物已失效，应弃用。");
    lines.push("==================================================================");
  } else if (artifacts.checked && artifacts.consistent === true) {
    lines.push(
      `GRAPHIFY_HEALTH_OK node_count 一致（graph.json nodes=${graph.nodeCount} == .graphify_health.json）`
    );
  } else if (artifacts.reason) {
    lines.push(`GRAPHIFY_HEALTH_UNCHECKED ${artifacts.reason}`);
  }

  return { verdict, lines, exitCode: 0 };
}

function printHelp() {
  process.stdout.write(
    "用法: node scripts/check-graphify-freshness.js [选项]\n" +
      "自检 graphify-out/graph.json 是否落后于 HEAD，并交叉校验 .graphify_health.json 的计数。\n" +
      "本脚本**未接入 CI**（graphify-out/ 被 gitignore，CI 检出后不存在），仅供本机手动运行。\n" +
      "  --repo-root <dir>   仓库根目录（默认脚本上级目录）\n" +
      "  --help / -h         显示帮助\n" +
      "退出码：0=正常（含 NOT_BUILT/FRESH/STALE/副产物陈旧，均只提示不阻断），1=执行出错\n"
  );
}

function main() {
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
  // 必须是**目录**：只判 existsSync 会把「存在的普通文件」当合法仓库根 —— 随后
  // readGraphSnapshot 在 该文件/graphify-out/graph.json 处找不到文件，于是落到
  // GRAPHIFY_NOT_BUILT 且**退出码 0**，把「路径传错了」静默说成「图谱未构建」。
  // 与文件头「1 = --repo-root 指向非法目录」的承诺对齐。
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) {
    process.stderr.write(`仓库根目录不存在或不是目录：${root}\n`);
    process.exit(1);
  }

  const graph = readGraphSnapshot(root);
  const health = readHealthSnapshot(root);
  const git = readGitDelta(root, graph.builtAtCommit);
  const result = evaluateFreshness({ graph, health, git });

  process.stdout.write(result.lines.join("\n") + "\n");
  process.exit(result.exitCode);
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
  GRAPH_REL,
  HEALTH_REL,
  tryReadJson,
  readGraphSnapshot,
  readHealthSnapshot,
  compareProducedArtifacts,
  readGitDelta,
  evaluateFreshness,
};
