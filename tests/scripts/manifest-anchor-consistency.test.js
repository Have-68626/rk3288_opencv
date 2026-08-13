// 治理任务 Q4-2（P2）：cascade 资产平台锚点 —— 消除行尾导致的 manifest hash 漂移。
// 背景：app/src/main/assets/lbpcascade_frontalface.xml 声明 binary 前走 * text=auto eol=lf，
// Windows checkout 会把行尾转成 CRLF，使 working tree 字节 ≠ git blob（LF），
// 二者 SHA256 分别为 6f5f20…（本机 CRLF 工作区）与 529f21…（blob / CREDITS.md），
// FramePipeline.cpp 的 manifest 自检在 LF 环境（CI / Linux checkout）会误报 mismatch。
// 本测试断言三处一致：FramePipeline.cpp 硬编码期望值 == CREDITS.md 记录值 == git blob 值，
// 且 .gitattributes 已将 assets 声明 binary（git check-attr binary 返回 set）。
const assert = require("node:assert/strict");
const crypto = require("node:crypto");
const fs = require("node:fs");
const path = require("node:path");
const { spawnSync } = require("node:child_process");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const CASCADE = "app/src/main/assets/lbpcascade_frontalface.xml";
const FRAME_PIPELINE = path.join(REPO_ROOT, "src", "win", "src", "FramePipeline.cpp");

function git(args) {
  const r = spawnSync("git", args, { cwd: REPO_ROOT, encoding: "buffer" });
  assert.equal(r.status, 0, `git ${args.join(" ")} 失败: ${r.stderr && r.stderr.toString()}`);
  return r.stdout;
}

// 规范值来源：git blob（LF 归一化入库的字节），与 working tree 行尾状态无关。
function blobSha256() {
  return crypto.createHash("sha256").update(git(["show", ":" + CASCADE])).digest("hex");
}

// 从 FramePipeline.cpp 的 cascade 快照块提取硬编码期望 hash（64 位 hex），要求唯一。
function codeExpectedSha256() {
  const src = fs.readFileSync(FRAME_PIPELINE, "utf8");
  const found = [...src.matchAll(/m\.hash == "([0-9a-f]{64})"/g)].map((m) => m[1]);
  assert.equal(found.length, 1, `FramePipeline.cpp 中 m.hash 期望值应唯一，实际 ${found.length} 个`);
  return found[0];
}

// 从 CREDITS.md 中 LBP 资产所在行提取 SHA-256（首个 64 位 hex）。
function creditsSha256() {
  const md = fs.readFileSync(path.join(REPO_ROOT, "CREDITS.md"), "utf8");
  const line = md.split("\n").find((l) => l.includes(CASCADE));
  assert.ok(line, `CREDITS.md 中未找到含 ${CASCADE} 的行`);
  const m = line.match(/[0-9a-f]{64}/);
  assert.ok(m, "CREDITS.md 该行未找到 SHA-256");
  return m[0];
}

test("Q4-2：cascade 资产三处 hash 一致（FramePipeline.cpp / CREDITS.md / git blob）", () => {
  const blob = blobSha256();
  const code = codeExpectedSha256();
  const credits = creditsSha256();
  assert.equal(code, blob, `FramePipeline.cpp 期望值 ${code} 与 git blob ${blob} 不一致`);
  assert.equal(credits, blob, `CREDITS.md 记录值 ${credits} 与 git blob ${blob} 不一致`);
});

test("Q4-2：.gitattributes 将 assets 声明为 binary（checkout 不做行尾转换）", () => {
  const out = git(["check-attr", "text", "eol", "binary", "--", CASCADE]).toString("utf8");
  assert.match(out, /binary:\s*set/, `git check-attr 应含 binary: set，实际输出:\n${out}`);
  assert.match(out, /text:\s*unset/, `git check-attr 应含 text: unset，实际输出:\n${out}`);
});

// 行尾漂移诊断（仅信息，不断言）：声明 binary 后若 working tree 仍与 blob 不同，
// 说明该文件尚未重新 checkout 归一化，需 git restore/重新 checkout 使其与 blob 同值。
test("Q4-2：汇报 working tree 与 git blob 字节一致性（行尾漂移诊断）", (t) => {
  const blob = blobSha256();
  const wt = crypto
    .createHash("sha256")
    .update(fs.readFileSync(path.join(REPO_ROOT, ...CASCADE.split("/"))))
    .digest("hex");
  t.diagnostic(`git blob SHA256     = ${blob}`);
  t.diagnostic(`working tree SHA256 = ${wt}`);
  if (wt !== blob) {
    t.diagnostic("警告：working tree 与 blob 不一致（行尾尚未归一化）。");
    t.diagnostic("声明 binary 后需重新 checkout/restore 该文件，使其字节与 git blob 同值。");
  }
});
