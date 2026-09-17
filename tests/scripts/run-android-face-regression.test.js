// 契约测试：scripts/run-android-face-regression.ps1 的「稳态窗口剔除 + 稳态中位数 +
// FPS 判据结论」落地，且满足「静默假绿零容忍」。
//
// 背景：验收判据 A1 要求「剔除启动前 5s + 取稳态中位数」，但原脚本只算全量
// count/min/p50/p95/max，不做时间窗剔除。RK3288 设备时钟不可信、logcat -v brief
// 无时间戳、App 日志无单调计数，故采用「采样计数近似」口径（暖机占比 = WarmupSec /
// 主机时钟测得的采集窗口）。本测试为纯静态断言（CI 的 repo-hygiene 在 ubuntu 上跑，
// 无 pwsh），只校验脚本文本；数值口径的端到端验证由人工用 -LogcatFile 接缝完成。
//
// 运行：node --test tests/scripts/run-android-face-regression.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const PS1_PATH = path.join(REPO_ROOT, "scripts", "run-android-face-regression.ps1");
const DOC_PATH = path.join(REPO_ROOT, "docs", "guides", "rk3288-device-acceptance.md");
const ps = fs.readFileSync(PS1_PATH, "utf8");

test("声明稳态参数与离线测试接缝，且可显式关闭稳态块", () => {
  for (const p of [
    "[double]$WarmupSec",
    "[double]$FpsThreshold",
    "[int]$MinSteadySamples",
    "[switch]$NoSteadyState",
    "[string]$LogcatFile",
  ]) {
    assert.ok(ps.includes(p), `缺少参数声明：${p}`);
  }
  // 接缝开关：提供 -LogcatFile 时切换模式；生产路径（adb）不受影响。
  assert.ok(
    /\$fileMode\s*=\s*-not\s*\[string\]::IsNullOrWhiteSpace\(\$LogcatFile\)/.test(ps),
    "缺少 fileMode 接缝开关"
  );
});

test("生产采集路径不变：仍走 adb logcat -d -v brief", () => {
  assert.ok(
    /Invoke-AdbCapture[\s\S]*?logcat", "-d", "-v", "brief"/.test(ps),
    "真机采集路径（logcat -d -v brief）被改动"
  );
  // 设备分支必须以 -not $fileMode 守门，避免接缝模式触碰设备。
  assert.ok(/if\s*\(\s*-not\s+\$fileMode\s*\)\s*\{/.test(ps), "设备采集未受 fileMode 守门");
});

test("报告新增：稳态样本数、稳态中位数与 FPS 判据显式结论", () => {
  assert.ok(/steady_state\s*=\s*\[ordered\]@\{/.test(ps), "报告缺少独立的 steady_state 块");
  assert.ok(/fps_criterion\s*=\s*\[ordered\]@\{/.test(ps), "报告缺少 fps_criterion 结论块");
  assert.ok(
    /steady_sample_count\s*=\s*\$steadyTotal/.test(ps),
    "报告缺少稳态样本数 steady_sample_count"
  );
  assert.ok(/steady_p50\s*=/.test(ps), "报告缺少稳态中位数 steady_p50");
  assert.ok(
    /fps_criterion=\$fpsConclusion/.test(ps),
    "stdout 缺少显式结论行 fps_criterion="
  );
});

test("默认行为与现状一致：既有全量 metrics 块保留，新增块为加法", () => {
  // 既有全量口径块须原样保留。
  assert.ok(/metrics\s*=\s*\$metricObj/.test(ps), "既有全量 metrics 块被改动");
  // 稳态块是新增子对象，不替换全量块；-NoSteadyState 可回到与现状完全一致。
  assert.ok(/\$steadyEnabled\s*=\s*-not\s+\$NoSteadyState/.test(ps), "缺少 NoSteadyState 关闭开关");
});

test("口径可复现：暖机占比建立在主机时钟测得的采集窗口上，而非设备时间戳", () => {
  assert.ok(/Stopwatch\]::StartNew\(\)/.test(ps), "采集窗口未用主机时钟（Stopwatch）测量");
  assert.ok(
    /\$warmupFraction\s*=\s*\$WarmupSec\s*\/\s*\$windowSec/.test(ps),
    "暖机占比未按 WarmupSec / windowSec 计算"
  );
  assert.ok(/Get-SteadySlice/.test(ps), "缺少稳态切片函数 Get-SteadySlice");
  assert.ok(
    /采样计数近似/.test(ps),
    "缺少「采样计数近似」口径说明（设备时钟不可信）"
  );
});

test("静默假绿零容忍：暖机吃光 / 稳态为空 / 无样本必须显式失败或显式标注", () => {
  // 暖机窗口 >= 采集窗口：显式失败（非零码）。
  assert.ok(
    /if\s*\(\s*\$WarmupSec\s*-ge\s*\$windowSec\s*\)/.test(ps),
    "缺少「暖机 >= 采集窗口」守卫"
  );
  assert.ok(/\bFail 7\b/.test(ps), "缺少稳态失败退出码 Fail 7");
  // 稳态全库为空：显式失败。
  assert.ok(/if\s*\(\s*\$steadyTotal\s*-eq 0\s*\)/.test(ps), "缺少「稳态样本为空」守卫");
  // 零样本：既有 Fail 1 守卫须仍在。
  assert.ok(/Fail 1 "回归失败：未从 logcat 采集到任何允许清单内的指标样本/.test(ps), "缺少零样本守卫 Fail 1");
  // 显式标注分支（不得安静地把无法判定当作通过）。
  assert.ok(ps.includes('"missing_fps_samples"'), "缺少 missing_fps_samples 显式标注");
  assert.ok(ps.includes('"insufficient_steady_samples"'), "缺少 insufficient_steady_samples 显式标注");
  assert.ok(ps.includes('"steady_state_disabled"'), "缺少 steady_state_disabled 显式标注");
  // satisfied 必须先初始化为 $null，绝不可默认判为通过。
  assert.ok(/\$fpsSatisfied\s*=\s*\$null/.test(ps), "fpsSatisfied 未初始化为 $null");
});

test("数据源与采样节奏口径固定：指向 inferenceAutoTune 与 ~30s/样本、auto 前置", () => {
  // 脚本报告 notes 与验收文档都须点明真实来源（StatusService/PerfReporter 不在 logcat）。
  assert.ok(ps.includes("inferenceAutoTune"), "脚本报告 notes 未指向真实来源 inferenceAutoTune");
  const doc = fs.readFileSync(DOC_PATH, "utf8");
  // 文档 A1 须指向真实来源、写明 ~30s 节奏、给出 MinSteadySamples 对应的窗口需求、
  // 并点明「不开 auto 模式 → missing_fps_samples」，避免验收人误以为脚本坏了。
  assert.ok(doc.includes("inferenceAutoTune"), "文档 A1 未指向真实来源 inferenceAutoTune");
  assert.ok(/30\s*(s|秒)/.test(doc), "文档 A1 未写明 ui_fps 约 30s 一个样本的采样节奏");
  assert.ok(/MinSteadySamples/.test(doc), "文档 A1 未写明 MinSteadySamples 与所需采集窗口");
  assert.ok(/missing_fps_samples/.test(doc), "文档 A1 未写明不开 auto 模式会报 missing_fps_samples");
  assert.ok(/insufficient_steady_samples/.test(doc), "文档 A1 未写明短窗口会报 insufficient_steady_samples");
});

test("采集模式可从 stdout 判别：input_mode 单点定义并同时进 JSON 与 stdout", () => {
  // 模式值必须单点计算：JSON 与 stdout 各算一次会出现两处漂移。
  assert.ok(
    /\$inputMode\s*=\s*if\s*\(\s*\$fileMode\s*\)\s*\{\s*"logcat_file"\s*\}\s*else\s*\{\s*"adb"\s*\}/.test(ps),
    "缺少 $inputMode 单点定义"
  );
  assert.ok(/input_mode\s*=\s*\$inputMode/.test(ps), "JSON 的 input_mode 未引用单点定义");
  // 下游常只读 stdout；不带模式标记时，误用 -LogcatFile 跑出的报告会被当成真机采集结果
  //（该模式下 device_id_hash/abi 为空、media.file_count 恒为 0，单看指标看不出来）。
  assert.ok(
    /Write-Output\s+"input_mode=\$inputMode"/.test(ps),
    "stdout 缺少 input_mode= —— 只读 stdout 的下游无法判别是 adb 采集还是 -LogcatFile 接缝"
  );
});
