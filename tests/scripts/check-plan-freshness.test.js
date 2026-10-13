/**
 * @file check-plan-freshness.test.js
 * @brief `scripts/check-plan-freshness.js` 的**纯函数**单测（合成夹具，不依赖 git、不读真清单）
 *
 * ⚠️ 边界说明：本文件覆盖的是脚本的**解析与判定逻辑**；**清单本身在 git 外**，
 *    CI 检出后不存在 ⇒ 清单内容**永远不被 CI 覆盖**，那是刻意的（见脚本头部「为什么不进 CI」）。
 */

"use strict";

const assert = require("node:assert/strict");
const test = require("node:test");

const {
  extractFencedBlocks,
  parseFreshnessBlock,
  splitLiteralAtScope,
  parseAnchors,
  classifyAnchor,
  summarize,
} = require("../../scripts/check-plan-freshness.js");

/** 造一个可控的解析器集合（不碰 git / fs）。 */
function fakeFns(overrides) {
  return Object.assign(
    {
      grepCount: () => 0,
      symbolInFile: () => false,
      fileExists: () => false,
      shaIsAncestor: () => false,
    },
    overrides || {}
  );
}

test("extractFencedBlocks 只取指定围栏的内容，忽略其它围栏与散文", () => {
  const md = [
    "# 标题",
    "```plan-anchors",
    "A | file | x",
    "```",
    "散文",
    "```bash",
    "echo hi",
    "```",
    "```plan-anchors",
    "B | file | y",
    "```",
  ].join("\n");
  const blocks = extractFencedBlocks(md, "plan-anchors");
  assert.equal(blocks.length, 2);
  assert.equal(blocks[0], "A | file | x");
  assert.equal(blocks[1], "B | file | y");
});

test("extractFencedBlocks 对未闭合围栏不返回半截块", () => {
  const blocks = extractFencedBlocks("```plan-anchors\nA | file | x\n", "plan-anchors");
  assert.equal(blocks.length, 0);
});

test("parseFreshnessBlock 取到两个字段；缺失时为空", () => {
  const md = "```plan-freshness\nverified_at_head = abc1234\nverified_at_date = 2026-09-25\n```";
  const m = parseFreshnessBlock(md);
  assert.equal(m.verified_at_head, "abc1234");
  assert.equal(m.verified_at_date, "2026-09-25");
  assert.deepEqual(parseFreshnessBlock("无块"), { verified_at_head: null, verified_at_date: null });
});

test("splitLiteralAtScope 按**最后一个** @ 切分（字面串自身可含 @）", () => {
  const a = splitLiteralAtScope("as_of=2026-09-16@CREDITS.md");
  assert.equal(a.literal, "as_of=2026-09-16");
  assert.equal(a.scope, "CREDITS.md");
  assert.equal(a.expect, null);

  const b = splitLiteralAtScope("foo@bar@src/x#expect=3");
  assert.equal(b.literal, "foo@bar");
  assert.equal(b.scope, "src/x");
  assert.equal(b.expect, 3);
});

test("splitLiteralAtScope 无 @ 时 scope 为空（调用方据此判 UNKNOWN）", () => {
  assert.equal(splitLiteralAtScope("裸字面串").scope, "");
});

test("parseAnchors 跳过注释与空行、保留字段、把格式错的行标成 __malformed__", () => {
  const md = [
    "```plan-anchors",
    "# 注释行",
    "",
    "A | file | x",
    "B | grep | lit@dir | 2",
    "C | 只有两段",
    "```",
  ].join("\n");
  const a = parseAnchors(md);
  assert.equal(a.length, 3);
  assert.deepEqual(a[0], { id: "A", kind: "file", payload: "x", expect: null });
  assert.deepEqual(a[1], { id: "B", kind: "grep", payload: "lit@dir", expect: 2 });
  assert.equal(a[2].kind, "__malformed__");
});

test("classifyAnchor：file 三种结局", () => {
  assert.equal(classifyAnchor({ kind: "file", payload: "a", expect: null }, fakeFns({ fileExists: () => true })).verdict, "OK");
  assert.equal(classifyAnchor({ kind: "file", payload: "a", expect: null }, fakeFns({ fileExists: () => false })).verdict, "STALE");
  assert.equal(classifyAnchor({ kind: "file", payload: "a", expect: null }, fakeFns({ fileExists: () => null })).verdict, "UNKNOWN");
});

test("classifyAnchor：sym 缺 # 判 UNKNOWN，找不到判 STALE", () => {
  const noHash = classifyAnchor({ kind: "sym", payload: "a.h", expect: null }, fakeFns());
  assert.equal(noHash.verdict, "UNKNOWN");
  assert.match(noHash.detail, /<file>#<Symbol>/);

  const found = classifyAnchor({ kind: "sym", payload: "a.h#Foo", expect: null }, fakeFns({ symbolInFile: () => true }));
  assert.equal(found.verdict, "OK");
  const gone = classifyAnchor({ kind: "sym", payload: "a.h#Foo", expect: null }, fakeFns({ symbolInFile: () => false }));
  assert.equal(gone.verdict, "STALE");
});

test("classifyAnchor：test 命中即 OK、零命中即 STALE", () => {
  assert.equal(classifyAnchor({ kind: "test", payload: "Foo", expect: null }, fakeFns({ grepCount: () => 2 })).verdict, "OK");
  assert.equal(classifyAnchor({ kind: "test", payload: "Foo", expect: null }, fakeFns({ grepCount: () => 0 })).verdict, "STALE");
});

test("classifyAnchor：grep 有 expect 必须相等；无 expect 只要求 > 0", () => {
  const withExpect = { kind: "grep", payload: "lit@dir", expect: 3 };
  assert.equal(classifyAnchor(withExpect, fakeFns({ grepCount: () => 3 })).verdict, "OK");
  assert.equal(classifyAnchor(withExpect, fakeFns({ grepCount: () => 4 })).verdict, "STALE");

  const noExpect = { kind: "grep", payload: "lit@dir", expect: null };
  assert.equal(classifyAnchor(noExpect, fakeFns({ grepCount: () => 1 })).verdict, "OK");
  assert.equal(classifyAnchor(noExpect, fakeFns({ grepCount: () => 0 })).verdict, "STALE");
});

test("classifyAnchor：grep/noref 无 scope 一律 UNKNOWN（本仓硬拒绝无范围 grep）", () => {
  assert.equal(classifyAnchor({ kind: "grep", payload: "裸串", expect: null }, fakeFns()).verdict, "UNKNOWN");
  assert.equal(classifyAnchor({ kind: "noref", payload: "裸串", expect: null }, fakeFns()).verdict, "UNKNOWN");
});

test("classifyAnchor：noref 零命中才 OK", () => {
  assert.equal(classifyAnchor({ kind: "noref", payload: "S@s", expect: null }, fakeFns({ grepCount: () => 0 })).verdict, "OK");
  const stale = classifyAnchor({ kind: "noref", payload: "S@s", expect: null }, fakeFns({ grepCount: () => 5 }));
  assert.equal(stale.verdict, "STALE");
  assert.match(stale.detail, /期望 0 命中，实得 5/);
});

test("classifyAnchor：sha 需是 HEAD 的祖先", () => {
  assert.equal(classifyAnchor({ kind: "sha", payload: "abc", expect: null }, fakeFns({ shaIsAncestor: () => true })).verdict, "OK");
  assert.equal(classifyAnchor({ kind: "sha", payload: "abc", expect: null }, fakeFns({ shaIsAncestor: () => false })).verdict, "STALE");
});

test("classifyAnchor：cmd 锚点**永不自动执行**，一律 UNKNOWN 并提示人工", () => {
  const r = classifyAnchor({ kind: "cmd", payload: "rm -rf / ⇒ 0", expect: null }, fakeFns());
  assert.equal(r.verdict, "UNKNOWN");
  assert.match(r.detail, /不自动执行/);
});

test("classifyAnchor：未知类型与格式错行都判 UNKNOWN，不静默当 OK", () => {
  assert.equal(classifyAnchor({ kind: "头", payload: "x", expect: null }, fakeFns()).verdict, "UNKNOWN");
  assert.equal(classifyAnchor({ kind: "__malformed__", payload: "", expect: null }, fakeFns()).verdict, "UNKNOWN");
});

test("summarize 单列 UNKNOWN，绝不并入 OK", () => {
  const s = summarize([{ verdict: "OK" }, { verdict: "OK" }, { verdict: "STALE" }, { verdict: "UNKNOWN" }]);
  assert.deepEqual(s, { ok: 2, stale: 1, unknown: 1, total: 4 });
});

test("端到端（合成夹具）：解析 + 判定 + 汇总的串通", () => {
  const md = [
    "```plan-freshness",
    "verified_at_head = 0123456789abcdef0123456789abcdef01234567",
    "verified_at_date = 2026-09-25",
    "```",
    "```plan-anchors",
    "存在的文件 | file | scripts/x.js",
    "丢了的文件 | file | scripts/gone.js",
    "零引用 | noref | NoSuchSym@src | 0",
    "```",
  ].join("\n");
  const anchors = parseAnchors(md);
  assert.equal(anchors.length, 3);
  const results = anchors.map((a) =>
    classifyAnchor(
      a,
      fakeFns({
        fileExists: (p) => p === "scripts/x.js",
        grepCount: () => 0,
      })
    )
  );
  const s = summarize(results);
  assert.deepEqual(s, { ok: 2, stale: 1, unknown: 0, total: 3 });
  assert.equal(parseFreshnessBlock(md).verified_at_head.length, 40);
});
