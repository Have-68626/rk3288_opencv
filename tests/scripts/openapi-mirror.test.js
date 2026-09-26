// FACE-API-001 守护：运行时内嵌 OpenAPI（kOpenApiJson）↔ docs/windows-web-spa/openapi.yaml 一致。
//
// 为什么需要它：仓库有**两份** OpenAPI 描述，且**没有任何机械比对**：
//   ① 运行时事实源 = `src/win/src/JsonEndpointHandlers.cpp` 里的单行原始字符串 `R"JSON(…)"`，
//      由 `GET /api/v1/openapi`（与 `/openapi.json` 别名）原样返回；
//   ② 文档镜像 = `docs/windows-web-spa/openapi.yaml`。
// 修复前的两处实测问题（本测试的 RED 证据）：
//   - **语义矛盾**：`/api/faces` 与 `/api/v1/faces` 的 summary 在内嵌版写「人脸库快照」，
//     而 yaml 写「当前帧人脸快照」。**yaml 对**：该 handler 取的是
//     `FramePipeline::snapshotFaces()` 的当前帧渲染态（无帧即 503），`buildFacesJson` 逐字
//     **不输出 personId**，全仓也不存在「列出人脸库内容」的端点。
//   - **注释失真**：该字符串上方的注释曾声称「S3 三方校验要求两者 paths 集合一致」，
//     而仓内**从无**该实现——连 paths 集合都没比过。本测试即该断言的落地，注释已同步改写。
//
// 判据（三条）：
//   ① `METHOD path` 集合**双向相等**（此前只被注释口头声称的那件事）；
//   ② 「语义承重」端点（`SEMANTIC_PINS`，当前 4 条：faces×2 / models/reload / db·clear）两侧各钉
//      期望子串 + 反向措辞；
//   ③ 逐条 summary 的**已知分歧**以诊断形式报出（不判红——见下）。
//
// ⚠️ 为什么第 ③ 条**不做**成断言：实测 20 个操作中**十四个**的 summary 措辞本就不同（内嵌偏简、
//    yaml 偏详）。其中**大多数**只是风格/互补差异，但**并非全部** —— 2026-09-26 二次审计独立复核时
//    就在其中查出一条**语义矛盾**（`POST /api/v1/models/reload`：内嵌写「按 id 重载」，
//    而 handler 调的是 `reloadRuntime(current())`、id 根本没传进去 ⇒ 两条 summary 对同一必填参数的
//    角色给出**互斥**陈述）。⇒ 处置结论不变（要求全量逐字一致是错误目标），但
//    ★ **「其余皆为风格差异」这句话是错判，不得再写**：承重端点必须逐个纳入上面的 `SEMANTIC_PINS`。
//    本测试**只报数、不判语义**——人眼这次就漏了一条，这正是它不做断言的原因（断言会给出虚假的安心）。
// ⚠️ 边界（如实登记）：本测试**只覆盖 path/method 集合与 faces 两条的语义**。
//    内嵌版的 operation 对象只有 `summary` 与一个 `200:{description}`，没有 parameters /
//    schema / 503 / operationId ⇒ **不能**证明两份规范在响应体、错误码、参数层面一致——
//    那些仍只存在于 yaml。要收紧须先给内嵌版补字段。
//
// 范式来源：本仓既有 `tests/scripts/config-contract.test.js`（同一对「读 C++ 源码当文本」+
// 「按缩进扫 openapi.yaml」工具，`:22-23` 与 `:70-99`）与 `workflow-annotation-titles.test.js`
// 的「不引入 YAML 库、行级扫描」约定。
//
// 运行：node --test tests/scripts/openapi-mirror.test.js

"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const HANDLERS_PATH = path.join(REPO_ROOT, "src", "win", "src", "JsonEndpointHandlers.cpp");
const OPENAPI_PATH = path.join(REPO_ROOT, "docs", "windows-web-spa", "openapi.yaml");

const HTTP_METHODS = ["get", "post", "put", "delete", "patch"];

// 从 JsonEndpointHandlers.cpp 抠出内嵌 OpenAPI 并 JSON.parse。
// 抽取失败**必须抛**（宁可红，也不静默返回空文档 ⇒ 空集合会让相等断言变成恒真）。
function extractEmbeddedOpenApi(src) {
  const re = /kOpenApiJson\s*=\s*R"JSON\(([\s\S]*?)\)JSON"/;
  const m = src.match(re);
  assert.ok(m, '未能在 JsonEndpointHandlers.cpp 中定位 kOpenApiJson 的 R"JSON(…) 字面量——抽取器与源码形态已脱节');
  return JSON.parse(m[1]);
}

// 按缩进扫描 openapi.yaml 的 paths 段 → Map<"METHOD /path", summary>。
// 刻意不引入 YAML 解析器：全仓 scripts/ 与 tests/scripts/ 均无此类依赖。
function parseYamlOperations(yaml) {
  const lines = String(yaml).split(/\r?\n/);
  const out = new Map();
  let inPaths = false;
  let curPath = null;
  let curMethod = null;
  for (const line of lines) {
    if (/^paths:\s*$/.test(line)) {
      inPaths = true;
      continue;
    }
    // 顶格的其他键（components / info / …）⇒ 离开 paths 段
    if (inPaths && /^[^\s#]/.test(line)) {
      inPaths = false;
      continue;
    }
    if (!inPaths) continue;

    const pathKey = line.match(/^ {2}(\/\S+):\s*$/);
    if (pathKey) {
      curPath = pathKey[1];
      curMethod = null;
      continue;
    }
    const methodKey = line.match(/^ {4}([a-z]+):\s*$/);
    if (methodKey && curPath) {
      curMethod = HTTP_METHODS.includes(methodKey[1]) ? methodKey[1] : null;
      if (curMethod) out.set(`${curMethod.toUpperCase()} ${curPath}`, null);
      continue;
    }
    // operation 级 summary：6 空格缩进（description 的 `|` 正文为 8 空格，不会误匹配）
    const summaryKey = line.match(/^ {6}summary:\s*(.+?)\s*$/);
    if (summaryKey && curPath && curMethod) {
      const key = `${curMethod.toUpperCase()} ${curPath}`;
      assert.equal(out.get(key), null, `${key} 在 yaml 中出现重复的 operation 级 summary`);
      out.set(key, summaryKey[1].replace(/^["']|["']$/g, ""));
    }
  }
  return out;
}

const embeddedSrc = fs.readFileSync(HANDLERS_PATH, "utf8");
const yamlSrc = fs.readFileSync(OPENAPI_PATH, "utf8");
const embeddedDoc = extractEmbeddedOpenApi(embeddedSrc);
const yamlOps = parseYamlOperations(yamlSrc);

const embeddedOps = new Map();
for (const [p, item] of Object.entries(embeddedDoc.paths || {})) {
  for (const [meth, op] of Object.entries(item || {})) {
    if (!HTTP_METHODS.includes(meth)) continue;
    embeddedOps.set(`${meth.toUpperCase()} ${p}`, op.summary === undefined ? null : op.summary);
  }
}

// 「语义承重」端点：这些端点的 summary 一旦写错就会**误导调用方**（不是风格问题），故两侧各钉一条
// 期望子串，并钉一条「不得出现」的反向措辞。子串而非全串：两侧允许风格差异，但必须表达同一件事。
//
// ⚠️ 这份清单是**被审计逼出来的**（2026-09-26 二次审计 S3 独立复核）：
//   - faces×2：内嵌曾写「人脸库快照」，与实现（当前帧快照、无 personId）**语义相反**；
//   - `POST /api/v1/models/reload`：内嵌曾写「**按 id 重载**模型运行时」——而 handler
//     （`JsonEndpointHandlers.cpp` 的 reload 分支）调的是 `reloadRuntime(current())`，
//     **id 根本没被传进去**，它只作存在性门槛；yaml 特意写明「id **不决定**重载对象」。
//     ⇒ 这同样是一条**语义矛盾**（不是风格差异），故纳入本清单。
const SEMANTIC_PINS = [
  {
    key: "GET /api/faces",
    embedded: "当前帧人脸快照",
    yaml: "当前帧人脸快照",
    forbidden: "人脸库",
  },
  {
    key: "GET /api/v1/faces",
    embedded: "当前帧人脸快照（版本化路径）",
    yaml: "当前帧人脸快照（版本化路径）",
    forbidden: "人脸库",
  },
  {
    key: "POST /api/v1/models/reload",
    embedded: "按 id 作存在性校验",
    yaml: "按 id 作存在性校验",
    forbidden: "按 id 重载",
  },
  {
    // 反向对照：真正的「库」操作**仍须**保留「库」语义（防止把上面的订正误扩到别处）。
    key: "POST /api/v1/actions/db/clear",
    embedded: "清空人脸库",
    yaml: "清空人脸数据库",
    forbidden: null,
  },
];

test("抽取器未脱节：两侧都解析出了非空的操作集", () => {
  // 防「解析器失效 ⇒ 空集 ⇒ 相等断言恒真」这一形态的静默失效。
  assert.ok(embeddedOps.size >= 15, `内嵌 OpenAPI 只解析出 ${embeddedOps.size} 个操作，抽取器可能已失效`);
  assert.ok(yamlOps.size >= 15, `openapi.yaml 只解析出 ${yamlOps.size} 个操作，缩进扫描器可能已失效`);
  assert.equal(embeddedDoc.openapi, "3.0.3", "内嵌 OpenAPI 版本号被改动");
  for (const [key, summary] of yamlOps) {
    assert.ok(typeof summary === "string" && summary !== "", `yaml 的 ${key} 未抽到 operation 级 summary`);
  }
});

test("内嵌 OpenAPI ↔ openapi.yaml：`METHOD path` 集合双向相等", (t) => {
  const embOnly = [...embeddedOps.keys()].filter((k) => !yamlOps.has(k)).sort();
  const yamlOnly = [...yamlOps.keys()].filter((k) => !embeddedOps.has(k)).sort();
  assert.deepEqual(embOnly, [], "以下操作只在内嵌 OpenAPI 中声明，openapi.yaml 缺失");
  assert.deepEqual(yamlOnly, [], "以下操作只在 openapi.yaml 中声明，内嵌 OpenAPI 缺失");
  // 已知分歧**只报数、不判红**（理由见文件头 ⚠️）：任何一次对齐或措辞改动都会改变这个数，
  // 让它当判据只会制造噪声门禁。它在这里的作用是**可见性**——复核者应当看到它、并判断
  // 新增的那一条是风格差异还是又一次语义矛盾。
  const divergent = [];
  for (const [key, embSummary] of embeddedOps) {
    if (!yamlOps.has(key)) continue;
    const yamlSummary = yamlOps.get(key);
    if (embSummary !== yamlSummary) divergent.push(`${key} | 内嵌: ${embSummary} | yaml: ${yamlSummary}`);
  }
  t.diagnostic(`逐条 summary 措辞分歧 ${divergent.length} / ${embeddedOps.size} 条：`);
  for (const d of divergent) t.diagnostic(`  ${d}`);
  t.diagnostic("⚠️ 本清单只报数、**不判语义**：这些分歧里既可能有纯风格差异，也可能藏着语义矛盾");
  t.diagnostic("   （2026-09-26 二次审计就在其中查出过一条：POST /api/v1/models/reload）。");
  t.diagnostic("   复核者须逐条读；承重的端点请加进 SEMANTIC_PINS，不要靠人眼一次过关。");
});

test("语义承重端点的 summary：两侧都必须表达同一件事，且不得出现反向措辞", () => {
  for (const pin of SEMANTIC_PINS) {
    const emb = embeddedOps.get(pin.key);
    const yml = yamlOps.get(pin.key);
    assert.ok(typeof emb === "string", `内嵌 OpenAPI 缺 ${pin.key}`);
    assert.ok(typeof yml === "string", `openapi.yaml 缺 ${pin.key}`);
    assert.ok(
      emb.includes(pin.embedded),
      `内嵌 OpenAPI 的 ${pin.key} 语义被改写：期望包含「${pin.embedded}」，实际「${emb}」`
    );
    assert.ok(
      yml.includes(pin.yaml),
      `openapi.yaml 的 ${pin.key} 语义被改写：期望包含「${pin.yaml}」，实际「${yml}」`
    );
    if (pin.forbidden !== null) {
      assert.ok(!emb.includes(pin.forbidden), `内嵌 OpenAPI 的 ${pin.key} 出现反向措辞「${pin.forbidden}」`);
      assert.ok(!yml.includes(pin.forbidden), `openapi.yaml 的 ${pin.key} 出现反向措辞「${pin.forbidden}」`);
    }
  }
});
