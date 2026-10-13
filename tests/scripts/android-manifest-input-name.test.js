// 安卓模型 manifest 的**节点名**护栏（2026-09-26 二次审计补的缺口）。
//
// 背景：`674f91a` 修了一个 **High**——两份 Android profile 的 manifest 把**检测器**的输入层名写成
// `in0`，而 manifest 用 sha256 逐字节钉住的那两份 SCRFD 模型的 Input 层实际叫 `input.1`
// （`models/scrfd_face_detect_ncnn/scrfd_{500m,2.5g}_kps-opt2.param` **第 3 行**皆为
//  `Input input.1 0 1 input.1`；`in0` 是 **ArcFace 侧**的名字，被误抄到检测器侧）。
// 消费链：manifest → `FaceRuntimeProfile` → `ScrfdNcnnAdapter` 的 `cfg.inputName` → `ex.input(...)`，
// **失败且无回落**（`err = "ncnn_set_input_failed"`）。
//
// 为什么当时**没有**护栏（本测试要补的正是这个）：那一批加的两条护栏
// （`tests/cpp/test_face_runtime_profile.cpp` 的 `MissingDetectorInputFallsBackToRealScrfdBlobName`
//  与 `FaceModelManifestTest` 的 `assertEquals("input.1", r.profile.detectorInput)`）
// 判别的都是**缺键回落值**，判不到 **manifest 里的字面值** —— 把 manifest 改回 `in0` 全绿。
//
// 为什么这里用「钉字面值」而不是「与 .param 交叉核对」：`.param`/`.bin` 被 `models/*` 忽略、
// **不入库** ⇒ CI 全新检出后读不到，任何交叉核对只会恒红或恒跳。故本测试钉两件事：
//   ① 两份 manifest 的字面值（防退回已知为假的值）；
//   ② **五处镜像/默认值**（C++ 头默认 / C++ 解析器缺键回落 / Java 镜像缺键回落 /
//      `ScrfdNcnnConfig::inputName` 结构体默认 / `MobileFaceNetAdapter::inputName_` 成员默认）
//      与 manifest **同向** —— 防「一处改了、另一处没改」的静默漂移。
//      （★ 2026-09-26 二次审计补：原先只有前三条，漏了后两条 —— 而其中 `ScrfdNcnnConfig::inputName`
//        恰是「检测器侧被误抄成 in0」的同一处历史错值，只是位于结构体默认层。）
//
// ⚠️ 边界（如实登记）：本测试**不能**证明 manifest 与真实模型的 Input 层名一致
//    —— 那个证据是 2026-09-26 的人工核对（sha256 对齐 + 读 `.param` 第 3 行）。
//    换句话说：**把 manifest 与三处镜像一起改成同一个错误值，本测试不会红。** 这是本仓
//    「模型二进制不入库」的直接后果，不是本测试可以绕过的。
//
// 判别性实测（2026-09-26）：把任一 manifest 的 `detector.input` 改回 `in0` ⇒ 红；
//    把 `FaceRuntimeProfile.h` 的 `embedderInput` 改成 `input.1` ⇒ 红。
//
// 运行：node --test tests/scripts/android-manifest-input-name.test.js

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");

// 检测器：本仓全部 SCRFD ncnn 模型的 Input 层名。嵌入器：ArcFace 侧确实是 in0。
const DETECTOR_INPUT = "input.1";
const EMBEDDER_INPUT = "in0";

const MANIFESTS = [
  "models/android/rk3288_light/manifest.json",
  "models/android/arm64_accuracy/manifest.json",
];

// 「缺键回落值」的三处镜像：文件 → { 正则, 期望值, 说明 }。
// 每个正则都必须**恰命中一次**（防注释/多份拷贝造成误抽或漏抽）。
const FALLBACK_SITES = [
  {
    file: "src/cpp/include/FaceRuntimeProfile.h",
    re: /std::string detectorInput\s*=\s*"([^"]*)"/g,
    want: DETECTOR_INPUT,
    what: "C++ 头文件里的 detectorInput 成员默认值",
  },
  {
    file: "src/cpp/include/FaceRuntimeProfile.h",
    re: /std::string embedderInput\s*=\s*"([^"]*)"/g,
    want: EMBEDDER_INPUT,
    what: "C++ 头文件里的 embedderInput 成员默认值",
  },
  {
    file: "src/cpp/src/FaceRuntimeProfile.cpp",
    re: /out\.detectorInput\s*=\s*"([^"]*)"/g,
    want: DETECTOR_INPUT,
    what: "C++ 解析器的 detectorInput 缺键回落值",
  },
  {
    file: "src/cpp/src/FaceRuntimeProfile.cpp",
    re: /out\.embedderInput\s*=\s*"([^"]*)"/g,
    want: EMBEDDER_INPUT,
    what: "C++ 解析器的 embedderInput 缺键回落值",
  },
  {
    file: "src/java/com/example/rk3288_opencv/FaceModelManifest.java",
    re: /strOr\(asString\(det\.get\("input"\)\),\s*"([^"]*)"\)/g,
    want: DETECTOR_INPUT,
    what: "Java 镜像的 detectorInput 缺键回落值",
  },
  {
    file: "src/java/com/example/rk3288_opencv/FaceModelManifest.java",
    re: /strOr\(asString\(emb\.get\("input"\)\),\s*"([^"]*)"\)/g,
    want: EMBEDDER_INPUT,
    what: "Java 镜像的 embedderInput 缺键回落值",
  },
  {
    // 2026-09-26 二次审计补：这一处**结构体默认值**此前不在清单里，而它恰是「检测器侧被误抄成 in0」
    // 的同一处历史错值（现为 `input.1`）。它当前**不可达**（`configFromProfile` 会被 profile 覆盖），
    // 属**潜伏默认**；钉它的意义是「不留一个已知为假的默认值」。
    file: "src/cpp/include/adapters/ScrfdNcnnAdapter.h",
    re: /std::string inputName\s*=\s*"([^"]*)"/g,
    want: DETECTOR_INPUT,
    what: "ScrfdNcnnConfig::inputName 的结构体默认值（检测器侧，潜伏）",
  },
  {
    // 对照臂：**嵌入器**侧（MobileFaceNet）确实叫 in0 ⇒ 不得被上一条的订正误改。
    file: "src/cpp/include/adapters/MobileFaceNetAdapter.h",
    re: /std::string inputName_\s*=\s*"([^"]*)"/g,
    want: EMBEDDER_INPUT,
    what: "MobileFaceNetAdapter::inputName_ 的成员默认值（嵌入器侧）",
  },
];

const parsedManifests = MANIFESTS.map((rel) => {
  const abs = path.join(REPO_ROOT, rel);
  assert.ok(fs.existsSync(abs), `manifest 不存在：${rel}`);
  return { rel, doc: JSON.parse(fs.readFileSync(abs, "utf8")) };
});

test("前置锚：两份 manifest 都解析出了 detector/embedder 段与 input 键", () => {
  // 防「JSON 结构变了 ⇒ 下面的取值拿到 undefined ⇒ 断言恒真/恒假」这一形态的静默失效。
  assert.equal(parsedManifests.length, 2);
  for (const { rel, doc } of parsedManifests) {
    assert.ok(doc.detector && typeof doc.detector === "object", `${rel} 缺 detector 段`);
    assert.ok(doc.embedder && typeof doc.embedder === "object", `${rel} 缺 embedder 段`);
    assert.equal(typeof doc.detector.input, "string", `${rel} 的 detector.input 不是字符串`);
    assert.equal(typeof doc.embedder.input, "string", `${rel} 的 embedder.input 不是字符串`);
  }
});

test("两份 manifest：detector.input == input.1、embedder.input == in0", () => {
  for (const { rel, doc } of parsedManifests) {
    assert.equal(
      doc.detector.input,
      DETECTOR_INPUT,
      `${rel} 的 detector.input 不是 ${DETECTOR_INPUT}：` +
        "in0 是 ArcFace 侧的名字，检测器模型（sha256 钉住的 scrfd_*-opt2.param）的 Input 层是 input.1；" +
        "写错会让 ScrfdNcnnAdapter 的 ex.input() 报 ncnn_set_input_failed（失败且无回落）"
    );
    assert.equal(
      doc.embedder.input,
      EMBEDDER_INPUT,
      `${rel} 的 embedder.input 不是 ${EMBEDDER_INPUT}：` +
        "嵌入器侧确实叫 in0，别把检测器那处的订正顺手扩到这里"
    );
  }
});

test("五处镜像 / 默认值的取值与 manifest 同向（防单边漂移）", () => {
  for (const { file, re, want, what } of FALLBACK_SITES) {
    const src = fs.readFileSync(path.join(REPO_ROOT, file), "utf8");
    const found = [...src.matchAll(re)].map((m) => m[1]);
    assert.equal(
      found.length,
      1,
      `${file} 中「${what}」的抽取命中 ${found.length} 次（应为 1）——抽取器与该文件形态已脱节`
    );
    assert.equal(
      found[0],
      want,
      `${file} 的「${what}」是 "${found[0]}"，应为 "${want}"：` +
        "该回落值必须与本仓真实模型的 Input 层名一致（检测器 input.1 / 嵌入器 in0），且三处镜像不得各说各话"
    );
  }
});
