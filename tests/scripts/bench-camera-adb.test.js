// Q-CQ-B08-008 治理测试：bench_camera_adb 两份 ADB benchmark 脚本在「零样本 / 启动失败」时
// 必须以非零码退出，且不得输出「已生成基准结果」宣称完成。
//
// 判别逻辑（修复前 RED，修复后 GREEN）：
//   - 修复前：两份脚本的「已生成基准结果」完成行均无条件输出，零样本时仍 exit 0，
//     把「空数据」当作成功完成；
//   - 修复后：循环后出现「零样本守卫」（captured_samples / capturedSamples 计数器），
//     守卫以非零码退出，完成行被守卫阻隔 → 零样本时不可达。
//
// 运行：node --test tests/scripts/bench-camera-adb.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const SH_PATH = path.join(REPO_ROOT, "scripts", "bench_camera_adb.sh");
const PS1_PATH = path.join(REPO_ROOT, "scripts", "bench_camera_adb.ps1");

const sh = fs.readFileSync(SH_PATH, "utf8").split(/\r?\n/);
const ps1 = fs.readFileSync(PS1_PATH, "utf8").split(/\r?\n/);

// 断言：完成行之前必须存在引用样本计数器的「零样本守卫」，且守卫块内以非零码退出。
// 修复前脚本无样本计数、完成行无条件输出 → guardIdx = -1 → RED。
function assertZeroSampleGuard(lines, countName) {
  const doneIdx = lines.findIndex((l) => l.includes("已生成基准结果"));
  assert.ok(doneIdx >= 0, `未找到完成行：已生成基准结果`);
  const before = lines.slice(0, doneIdx);
  // 定位「零样本守卫」行：引用计数且含 -eq 0（比循环内的计数行更精确）。
  const guardIdx = before.findIndex((l) => l.includes(countName) && l.includes("-eq 0"));
  assert.ok(guardIdx >= 0, `完成行之前未找到零样本守卫（引用 ${countName}）`);
  // 守卫行之后（至完成行之前）必须出现非零退出码。
  const tail = before.slice(guardIdx);
  assert.ok(
    tail.some((l) => /exit [1-9]/.test(l)),
    `零样本守卫必须以非零码退出，实际未见 exit [1-9]`
  );
}

test("bench_camera_adb.sh 零样本/启动失败时退出非零且不得宣称完成", () => {
  assertZeroSampleGuard(sh, "captured_samples");
  assert.ok(
    sh.some((l) => l.includes("captured_samples") && /\+=\s*1|captured_samples\s*=.*\+ 1/.test(l)),
    ".sh 循环内应递增 captured_samples 样本计数"
  );
});

test("bench_camera_adb.ps1 零样本/启动失败时退出非零且不得宣称完成", () => {
  assertZeroSampleGuard(ps1, "capturedSamples");
  assert.ok(
    ps1.some((l) => l.includes("capturedSamples") && /capturedSamples\s*(\+\+|=\s*\$capturedSamples\s*\+ 1)/.test(l)),
    ".ps1 循环内应递增 capturedSamples 样本计数"
  );
});
