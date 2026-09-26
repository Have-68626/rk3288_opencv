// FACE-TEST-005② 守护：CI 里调用仓内 Python 脚本时传的**每个** `--flag` 都必须在该脚本的
// argparse 表里**有定义**，且脚本里没有「同名但未定义」的幽灵参数。
//
// 为什么需要它（本测试的 RED 证据）：`Quantize models (INT8)` 步骤曾传 `--size 640`，而
// `scripts/quantize_ncnn_int8.py` 的 argparse 表**从来没有** `--size` ⇒ 该命令必然以
// 「unrecognized arguments」退出（argparse 退出码 2）。而它**两条性质叠加后完全静默**：
//   ① 步骤级 `continue-on-error: true` ⇒ 失败不阻断 job；
//   ② `if: steps.changed-int8.outputs.changed == 'true'` ⇒ 多数运行**根本不执行**该步骤。
// 结果：口径上「有量化步骤」，实际上它可能长期跑都没跑成，且**没有任何东西会变红**。
// 该参数已于 2026-09-26 删除；本测试的作用是让它**再也回不来**（加回来即红）。
//
// ⚠️ 本测试**不是**在证明量化成功或精度达标——那两个步骤仍是非阻断，本测试只钉「命令行与
//    argparse 表一致」这一**静态事实**。这条边界必须保留，不得据本测试宣称量化链可用。
//
// 判据：
//   ① 在 ci.yml 的 `run:` 块里找出全部 `python[3] scripts/<x>.py …` 调用；每条的 `--flag`
//      都必须是该脚本 argparse 表的成员；
//   ② 非空锚：至少找到 1 条此类调用（否则本测试会静默变成空转）；
//   ③ 解析器未脱节：脚本里 `add_argument("--x"` 的**字面量总数**必须等于解析出的选项数
//      （防止正则与源码形态脱节 ⇒ 空集合 ⇒ 子集断言恒真）；
//   ④ 反向钉住：`--size` 不得出现在 quantize 脚本的表里（若它真被实现为合法参数，
//      须连同本测试与 `docs/plans/int8-quantization-plan.md` 的校准表一并复核）。
//
// 范式来源：本仓 `tests/scripts/face-model-table-consistency.test.js`（`parsePythonTable()` +
// 「字面量数 == 解析数」防漏抽）与 `workflow-annotation-titles.test.js`（块标量感知的行扫描）。
//
// 运行：node --test tests/scripts/quantize-ci-args-defined.test.js

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const CI_YML = path.join(REPO_ROOT, ".github", "workflows", "ci.yml");

// 取出所有块标量（`run: |` / `run: >`）的正文行，附上其所属 step 名。
function collectRunBlocks(yaml) {
  const lines = String(yaml).split(/\r?\n/);
  const blocks = [];
  let stepName = "(未命名 step)";
  for (let i = 0; i < lines.length; i++) {
    const nameM = lines[i].match(/^\s*-\s*name:\s*(.+?)\s*$/);
    if (nameM) stepName = nameM[1];
    const openM = lines[i].match(/^(\s*)run:\s*[|>][-+\d]*\s*$/);
    if (!openM) continue;
    const indent = openM[1].length;
    const body = [];
    let j = i + 1;
    for (; j < lines.length; j++) {
      const line = lines[j];
      if (line.trim() !== "" && line.match(/^\s*/)[0].length <= indent) break;
      body.push(line);
    }
    blocks.push({ stepName, body });
    i = j - 1;
  }
  return blocks;
}

// pwsh 续行符是行尾反引号 ⇒ 先把跨行的逻辑命令行拼起来，再抠参数。
function logicalCommands(body) {
  const out = [];
  let buf = "";
  for (const raw of body) {
    const line = raw.replace(/`\s*$/, "");
    buf = buf === "" ? line.trim() : `${buf} ${line.trim()}`;
    if (!/`\s*$/.test(raw)) {
      if (buf !== "") out.push(buf);
      buf = "";
    }
  }
  if (buf !== "") out.push(buf);
  return out;
}

// 脚本 argparse 表里的长选项：{长选项集合, add_argument 字面量计数}
function parseArgparseLongOptions(src) {
  const options = new Set();
  let literalCount = 0;
  const re = /add_argument\(([\s\S]*?)\)\s*(?=,|$)/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    const argsPart = m[1];
    for (const lit of argsPart.matchAll(/"(--?[A-Za-z0-9][A-Za-z0-9-]*)"/g)) {
      literalCount += 1;
      if (lit[1].startsWith("--")) options.add(lit[1]);
    }
  }
  return { options, literalCount };
}

const ciYaml = fs.readFileSync(CI_YML, "utf8");
const runBlocks = collectRunBlocks(ciYaml);

// ① 找出 ci.yml 里所有 `python[3] scripts/<x>.py …` 调用
const invocations = [];
for (const { stepName, body } of runBlocks) {
  for (const cmd of logicalCommands(body)) {
    const m = cmd.match(/python3?\s+(scripts[\\/][\w.-]+\.py)\b(.*)$/);
    if (!m) continue;
    const scriptRel = m[1].replace(/\\/g, "/");
    const flags = [...m[2].matchAll(/(?:^|\s)(--[A-Za-z0-9][A-Za-z0-9-]*)/g)].map((x) => x[1]);
    invocations.push({ stepName, scriptRel, flags });
  }
}

test("② 非空锚：ci.yml 里确实存在对仓内 scripts/*.py 的调用", () => {
  assert.ok(
    invocations.length >= 1,
    "未在 ci.yml 的 run: 块里找到任何 `python scripts/<x>.py` 调用——本测试的提取器已与 workflow 形态脱节"
  );
});

test("③ 解析器未脱节：quantize 脚本的 argparse 表被完整、无重复地解析出来", () => {
  const rel = "scripts/quantize_ncnn_int8.py";
  const src = fs.readFileSync(path.join(REPO_ROOT, rel), "utf8");
  const { options, literalCount } = parseArgparseLongOptions(src);
  // 该脚本的 9 个长选项是已逐行核对过的稳定事实；数量骤变即说明抽取器或脚本形态已变。
  assert.ok(options.size >= 8, `${rel} 只解析出 ${options.size} 个长选项，抽取器可能已失效`);
  // 该脚本每个 add_argument 只声明一个长选项 ⇒ 字面量数必须等于去重后的选项数。
  // 不等即说明抽取器重复计数（⇒ 下面的子集断言可能恒真）或脚本里有同名重复声明。
  assert.equal(
    literalCount,
    options.size,
    `${rel}: add_argument 里的长选项字面量数(${literalCount}) != 去重后的选项数(${options.size})`
  );
});

test("① CI 用到的每个 --flag 都必须在对应脚本的 argparse 表里定义", () => {
  const problems = [];
  for (const { stepName, scriptRel, flags } of invocations) {
    const abs = path.join(REPO_ROOT, scriptRel);
    assert.ok(fs.existsSync(abs), `ci.yml 的 step「${stepName}」调用了不存在的脚本：${scriptRel}`);
    const { options } = parseArgparseLongOptions(fs.readFileSync(abs, "utf8"));
    for (const flag of flags) {
      if (!options.has(flag)) {
        problems.push(`step「${stepName}」调用 ${scriptRel} 时传了脚本未定义的参数 ${flag}`);
      }
    }
  }
  assert.deepEqual(problems, [], "CI 命令行与脚本 argparse 表不一致（该命令会以「未知参数」失败，且因 continue-on-error 而静默）");
});

test("④ quantize 脚本的表里不得出现 --size（该参数从未实现）", () => {
  const src = fs.readFileSync(path.join(REPO_ROOT, "scripts", "quantize_ncnn_int8.py"), "utf8");
  const { options } = parseArgparseLongOptions(src);
  assert.ok(
    !options.has("--size"),
    "quantize_ncnn_int8.py 出现了 --size：若确实实现了它，须同时复核本测试与 " +
      "docs/plans/int8-quantization-plan.md 的校准表（否则是把已修的缺陷重新放行）"
  );
});
