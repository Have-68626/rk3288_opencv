// B08-006 契约测试：scripts/run-web-e2e.ps1 子命令失败传播 + 后台 job 兜底清理。
//
// 判别逻辑（修复前 RED，修复后 GREEN）：
//   - RED1：脚本不含 $LASTEXITCODE，pnpm 子命令失败被吞掉，
//     $ErrorActionPreference="Stop" 不影响原生命令退出码，脚本整体仍以 0 退出。
//   - RED2：Stop-Job/Remove-Job 仅在 try 体内、finally 无清理；
//     throw 提前退出（如 Vite 未就绪、子命令失败）时后台 devJob 泄漏。
//
// 运行：node --test tests/scripts/run-web-e2e.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const PS1_PATH = path.join(REPO_ROOT, "scripts", "run-web-e2e.ps1");

const ps = fs.readFileSync(PS1_PATH, "utf8");

// 去除注释与空行后的有效行（识别 # 注释与纯空行）。
function meaningfulLines(text) {
  return text
    .split(/\r?\n/)
    .map((l) => l.replace(/\s+$/, ""))
    .filter((l) => {
      const t = l.trim();
      return t && !t.startsWith("#");
    });
}

// 提取 finally 块体文本：从 "finally" 关键字后的 "{" 到文件末 "}"（脚本末块）。
function finallyBody(text) {
  const fIdx = text.indexOf("finally");
  assert.ok(fIdx >= 0, "脚本缺少 finally 块");
  const braceStart = text.indexOf("{", fIdx);
  assert.ok(braceStart >= 0, "finally 后缺少 {");
  return text.slice(braceStart + 1, text.lastIndexOf("}"));
}

test("RED1: 子命令失败必须传播为非零退出码", () => {
  const lines = meaningfulLines(ps);
  // 需对 install / e2e:run:coverage / coverage:report 三个子命令逐个检查。
  const exitChecks = lines.filter((l) => /\$LASTEXITCODE/.test(l));
  assert.ok(
    exitChecks.length >= 3,
    `RED1 判别失败：未对每个 pnpm 子命令检查 $LASTEXITCODE（现 ${exitChecks.length} 处，需 >=3）。`
  );
  // 必须 throw 传播（finally 会随 throw 执行；exit 不会触发 finally）。
  assert.ok(
    exitChecks.some((l) => /if\s*\(\s*\$LASTEXITCODE/.test(l)) &&
      /\bthrow\b/.test(lines.join("\n")),
    "RED1 判别失败：找到 $LASTEXITCODE 但缺少 throw 传播。"
  );
});

test("RED2: 任何退出路径都必须清理后台 job（finally/trap 含 Stop-Job+Remove-Job）", () => {
  const guards = [];
  guards.push(finallyBody(ps));
  const trapIdx = ps.search(/\btrap\s*\{/);
  if (trapIdx >= 0) {
    const tEnd = ps.indexOf("}", trapIdx);
    guards.push(ps.slice(trapIdx, tEnd + 1));
  }
  const guardText = guards.join("\n");
  assert.ok(
    /Stop-Job/.test(guardText) && /Remove-Job/.test(guardText),
    "RED2 判别失败：finally/trap 未含 Stop-Job+Remove-Job，提前退出会泄漏后台 devJob。"
  );
  // 兜底清理必须引用 devJob 且判空（Start-Job 未执行时不得在 finally 报错）。
  assert.ok(
    /\$devJob/.test(guardText) && /\$devJob\s+-ne\s+\$null/.test(guardText),
    "RED2 判别失败：finally/trap 未以判空方式清理 $devJob。"
  );
});
