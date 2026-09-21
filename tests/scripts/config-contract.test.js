// B08-004 契约测试：config.schema.json ↔ openapi.yaml ↔ web/src/app/api/types.ts 三处字段一致。
//
// 判别逻辑（修复前 RED，修复后 GREEN）：
//   - openapi.yaml Settings.properties 缺 inference/model 两个段；
//     RecognitionSettings 缺 arcFaceModelPath（required + properties）；
//     AccelerationSettings 缺 enableLibyuv（required + properties）。
//   - types.ts model.int8Enabled 被标记可选，而运行时 toSettingsDocObject 无条件输出全部字段。
//
// 运行：node --test tests/scripts/config-contract.test.js
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const SCHEMA_PATH = path.join(REPO_ROOT, "docs", "windows-web-spa", "config.schema.json");
const OPENAPI_PATH = path.join(REPO_ROOT, "docs", "windows-web-spa", "openapi.yaml");
const TYPES_PATH = path.join(REPO_ROOT, "web", "src", "app", "api", "types.ts");
// FACE-ARCWIN-004：recognition 段的两处 C++ 字段集拷贝（此前无机械看护）。
const WIN_JSON_CONFIG_PATH = path.join(REPO_ROOT, "src", "win", "src", "WinJsonConfig.cpp");
const JSON_SCHEMA_VALIDATOR_PATH = path.join(REPO_ROOT, "src", "win", "src", "JsonSchemaValidator.cpp");
// FACE-ARCWIN-001 新增、必须同时出现在全部字段集拷贝中的锚键。
const RECOGNITION_ANCHOR_KEYS = ["arcFaceInputName", "arcFaceOutputName"];

// 从 config.schema.json 提取规范字段树：{ section: { fields:Set, required:Set } }
function canonicalFromSchema(schemaText) {
  const schema = JSON.parse(schemaText);
  const tree = {};
  for (const [section, def] of Object.entries(schema.properties)) {
    if (def.type !== "object") continue; // schemaVersion 顶层标量段跳过
    tree[section] = {
      fields: new Set(Object.keys(def.properties || {})),
      required: new Set(def.required || []),
    };
  }
  return tree;
}

// 最小缩进式 YAML 提取器：只抽取组件级的 required / properties 键 与 Settings 的 $ref 目标。
// 不做完整 YAML 语义解析，仅按本文件 Settings 区段（schemas: 之下 4 空格组件缩进）的结构工作。
function extractSettingsComponents(yamlText) {
  const lines = [];
  for (const raw of yamlText.split(/\r?\n/)) {
    const s = raw.replace(/\s+$/, "");
    const trimmed = s.trimStart();
    if (!trimmed || trimmed.startsWith("#")) continue;
    lines.push({ indent: s.length - s.trimStart().length, text: trimmed });
  }
  const si = lines.findIndex((l) => l.indent === 2 && l.text === "schemas:");
  if (si < 0) throw new Error("openapi.yaml 未找到 schemas:");
  const comps = {};
  let i = si + 1;
  while (i < lines.length && lines[i].indent >= 4) {
    if (lines[i].indent !== 4) { i++; continue; }
    const name = lines[i].text.replace(/:$/, "").trim();
    const comp = { required: [], props: new Set(), refs: new Set() };
    i++;
    while (i < lines.length && lines[i].indent >= 6) {
      const l = lines[i];
      if (l.indent === 6 && l.text === "required:") {
        i++;
        while (i < lines.length && lines[i].indent === 8 &&
               (lines[i].text.startsWith("- ") || lines[i].text === "-")) {
          comp.required.push(lines[i].text.replace(/^-/, "").trim());
          i++;
        }
      } else if (l.indent === 6 && l.text === "properties:") {
        i++;
        while (i < lines.length && lines[i].indent === 8) {
          const m = lines[i].text.match(/^([^:]+):\s*$/);
          if (!m) { i++; continue; }
          const key = m[1].trim();
          comp.props.add(key);
          i++;
          while (i < lines.length && lines[i].indent >= 10) {
            const rm = lines[i].text.match(/^\$ref:\s*["']?#\/components\/schemas\/([^"']+)["']?/);
            if (rm) comp.refs.add(rm[1]);
            i++;
          }
        }
      } else {
        i++;
      }
    }
    comps[name] = comp;
  }
  return comps;
}

// schema 段名 → openapi 组件名（首字母大写 + Settings 后缀），与 Settings.properties 的 $ref 一致。
function componentName(section) {
  return section.charAt(0).toUpperCase() + section.slice(1) + "Settings";
}

// 提取 types.ts ServerSettingsDoc：{ section: Set<字段> }，可选字段以 "?" 前缀标记。
function extractServerSettingsDoc(typesText) {
  const m = typesText.match(/export interface ServerSettingsDoc \{([\s\S]*?)\n\}/);
  if (!m) throw new Error("types.ts 未找到 ServerSettingsDoc");
  const sections = {};
  let current = null;
  for (const raw of m[1].split(/\r?\n/)) {
    const trimmed = raw.trim();
    if (!trimmed || trimmed.startsWith("//")) continue;
    const indent = raw.length - raw.trimStart().length;
    if (indent === 2) {
      if (trimmed === "}") { current = null; continue; }
      const sm = trimmed.match(/^(\w+): \{/);
      if (sm) { current = sm[1]; sections[current] = new Set(); }
      continue;
    }
    if (indent === 4 && current) {
      const fm = trimmed.match(/^(\w+)(\??):/);
      if (fm) sections[current].add(fm[2] ? "?" + fm[1] : fm[1]);
    }
  }
  return sections;
}

const schemaText = fs.readFileSync(SCHEMA_PATH, "utf8");
const openapiText = fs.readFileSync(OPENAPI_PATH, "utf8");
const typesText = fs.readFileSync(TYPES_PATH, "utf8");
const winJsonConfigText = fs.readFileSync(WIN_JSON_CONFIG_PATH, "utf8");
const jsonSchemaValidatorText = fs.readFileSync(JSON_SCHEMA_VALIDATOR_PATH, "utf8");
const canonical = canonicalFromSchema(schemaText);
// 组件名 → schema 段名（反向映射，供“多余组件”哨兵使用）。
const COMPONENT_TO_SECTION = new Map(Object.keys(canonical).map((s) => [componentName(s), s]));
const comps = extractSettingsComponents(openapiText);
const ts = extractServerSettingsDoc(typesText);
const schemaRoot = JSON.parse(schemaText);

test("openapi.yaml Settings.required ↔ config.schema.json 顶层 required", () => {
  assert.deepEqual(
    [...comps.Settings.required].sort(),
    [...schemaRoot.required].sort(),
    "Settings.required 与 schema 顶层 required 不一致"
  );
});

test("openapi.yaml Settings.properties ↔ config.schema.json 顶层段集", () => {
  const openapiSections = new Set([...comps.Settings.props].filter((k) => k !== "schemaVersion"));
  const schemaSections = new Set(Object.keys(canonical));
  assert.deepEqual(
    [...openapiSections].sort(),
    [...schemaSections].sort(),
    "Settings.properties 段集与 schema 不一致（缺 inference/model？）"
  );
});

test("openapi.yaml Settings.properties 的 $ref ↔ schema 段名映射", () => {
  const expectedRefs = new Set(Object.keys(canonical).map(componentName));
  assert.deepEqual(
    [...comps.Settings.refs].sort(),
    [...expectedRefs].sort(),
    "Settings $ref 目标与 schema 段名映射不一致（缺 InferenceSettings/ModelSettings？）"
  );
});

test("openapi.yaml 各 *Settings 组件 ↔ config.schema.json 对应段", () => {
  for (const [section, def] of Object.entries(canonical)) {
    const compName = componentName(section);
    const comp = comps[compName];
    assert.ok(comp, `openapi 缺 ${compName} 组件`);
    assert.deepEqual(
      [...comp.required].sort(),
      [...def.required].sort(),
      `openapi ${compName} required 与 schema ${section} 不一致`
    );
    assert.deepEqual(
      [...comp.props].sort(),
      [...def.fields].sort(),
      `openapi ${compName} properties 与 schema ${section} 不一致`
    );
  }
  // 反向：openapi 不应声明 schema 之外的 *Settings 组件（拼写/遗漏哨兵）
  for (const [name] of Object.entries(comps)) {
    if (name === "Settings" || COMPONENT_TO_SECTION.has(name)) continue;
    assert.ok(
      !name.endsWith("Settings"),
      `openapi 声明了 schema 之外的 settings 组件 ${name}`
    );
  }
});

test("types.ts ServerSettingsDoc ↔ config.schema.json 字段集", () => {
  for (const [section, def] of Object.entries(canonical)) {
    assert.ok(ts[section], `types.ts 缺 ${section} 段`);
    const tsFields = new Set([...ts[section]].map((f) => f.replace(/^\?/, "")));
    assert.deepEqual(
      [...tsFields].sort(),
      [...def.fields].sort(),
      `types.ts ${section} 字段与 schema 不一致`
    );
    // 运行时 toSettingsDocObject 无条件输出每段每字段 → 段内字段不得标记可选；
    // 标记可选即与序列化契约脱节（例：model.int8Enabled 曾被标 ? 而序列化恒输出）。
    for (const f of ts[section]) {
      assert.ok(
        !f.startsWith("?"),
        `types.ts ${section}.${f.slice(1)} 被标记可选，但序列化恒输出该字段`
      );
    }
  }
  const extra = Object.keys(ts).filter((s) => !canonical[s]);
  assert.deepEqual(extra, [], `types.ts 多余段: ${extra}`);
});

// ---- FACE-ARCWIN-004：recognition 段六处字段集机械校验（补 ①② 两处 C++ 拷贝） ----
//
// 需要一致的六处拷贝：
//   ① src/win/src/WinJsonConfig.cpp  序列化处（r.o["..."] = ...）
//   ① src/win/src/WinJsonConfig.cpp  解析处（getString/getNumber(*r, "...")）
//   ② src/win/src/JsonSchemaValidator.cpp  buildSettingsDocSchema()（rp.o["..."]）
//   ③ docs/windows-web-spa/config.schema.json
//   ④ docs/windows-web-spa/openapi.yaml  RecognitionSettings
//   ⑤ web/src/app/api/types.ts  ServerSettingsDoc.recognition
//
// 设计要点（见任务 FACE-ARCWIN-004）：
//   1) 作用域锚定——先按起止行切出 recognition 段，再在段内提键，
//      而非全文件 grep（否则会混入 camera/dnn/model/inference 等段的键）。
//   2) 防「静默通过」——若源码改写导致正则抽空，绝不能「空 == 空」通过；
//      故对每个来源断言：非空 + 含锚键 + 字面量出现次数 == 去重键数。
//   3) 失败信息可操作——主断言逐键给出「缺 [..]；多 [..]」，而非只报 sets differ。

// 在 [startPred 首次命中行, 其后 endPred 首次命中行) 的行区间内切出识别段。
// 返回行数组（含起点行，不含终点行）。
function sliceRecognitionSection(text, startPred, endPred, label) {
  const lines = text.split(/\r?\n/);
  const s = lines.findIndex(startPred);
  assert.ok(s >= 0, `${label}: 未定位到识别段起点`);
  const e = lines.findIndex((l, i) => i > s && endPred(l));
  assert.ok(e >= 0 && e > s, `${label}: 未定位到识别段终点`);
  return lines.slice(s, e);
}

// 在给定行区间内按 keyRe 提取键名（保留全部命中，以便检测重复/漏抽）。
function extractKeysInLines(lines, keyRe) {
  const keys = [];
  for (const line of lines) {
    keyRe.lastIndex = 0;
    let m;
    while ((m = keyRe.exec(line)) !== null) keys.push(m[1]);
  }
  return keys;
}

test("recognition 段字段集：WinJsonConfig 序列化/解析 ↔ 运行时 schema ↔ schema.json ↔ openapi ↔ types.ts", () => {
  // ①a WinJsonConfig.cpp 序列化处：形如 r.o["cascadePath"] = JsonValue::makeString(...)
  const serLines = sliceRecognitionSection(
    winJsonConfigText,
    (l) => l.includes("// recognition"),
    (l) => l.includes('root.o["recognition"]'),
    "WinJsonConfig.cpp 序列化段"
  );
  const serKeys = extractKeysInLines(serLines, /\br\.o\["([^"]+)"\]/g);

  // ①b WinJsonConfig.cpp 解析处：形如 getString(*r, "cascadePath", s) / getNumber(*r, ...)
  const parseLines = sliceRecognitionSection(
    winJsonConfigText,
    (l) => l.includes('doc.find("recognition")'),
    (l) => l.includes("// inference"),
    "WinJsonConfig.cpp 解析段"
  );
  const parseKeys = extractKeysInLines(parseLines, /get(?:String|Number)\(\*r,\s*"([^"]+)"/g);

  // ② JsonSchemaValidator.cpp buildSettingsDocSchema()：形如 rp.o["cascadePath"] = objStr()
  const runtimeLines = sliceRecognitionSection(
    jsonSchemaValidatorText,
    (l) => l.includes("// recognition"),
    (l) => l.includes('props.o["recognition"]'),
    "JsonSchemaValidator.cpp 识别段"
  );
  const runtimeKeys = extractKeysInLines(runtimeLines, /rp\.o\["([^"]+)"\]/g);

  // ③④⑤ 复用既有提取器结论（config.schema.json / openapi.yaml / types.ts）。
  const sources = [
    { label: "①a WinJsonConfig.cpp 序列化", keys: serKeys },
    { label: "①b WinJsonConfig.cpp 解析", keys: parseKeys },
    { label: "② JsonSchemaValidator.cpp 运行时 schema", keys: runtimeKeys },
    { label: "③ config.schema.json", keys: [...canonical.recognition.fields] },
    { label: "④ openapi.yaml RecognitionSettings", keys: [...(comps.RecognitionSettings || { props: new Set() }).props] },
    { label: "⑤ types.ts ServerSettingsDoc.recognition", keys: [...(ts.recognition || new Set())].map((f) => f.replace(/^\?/, "")) },
  ];

  // 防「静默通过」：每个来源都必须非空、含锚键、且无重复（字面量数 == 去重键数）。
  for (const { label, keys } of sources) {
    assert.ok(keys.length > 0, `${label}: recognition 段未抽出任何键（提取正则与源码写法脱节？）`);
    for (const must of RECOGNITION_ANCHOR_KEYS) {
      assert.ok(
        keys.includes(must),
        `${label}: 未抽到锚键 ${must}（该处漏改 FACE-ARCWIN-001 字段，或提取正则失效）`
      );
    }
    assert.equal(
      new Set(keys).size,
      keys.length,
      `${label}: 键名重复（字面量 ${keys.length} 条 ≠ 去重 ${new Set(keys).size} 条）`
    );
  }

  // 主断言：六处键集与 ③ config.schema.json 两两一致。失败信息逐键列出缺/多。
  const reference = canonical.recognition.fields; // Set<string>
  for (const { label, keys } of sources) {
    const actual = new Set(keys);
    const missing = [...reference].filter((k) => !actual.has(k));
    const extra = [...actual].filter((k) => !reference.has(k));
    assert.deepEqual(
      [...actual].sort(),
      [...reference].sort(),
      `${label}: recognition 段字段集与 config.schema.json 不一致；缺 [${missing.join(", ")}]；多 [${extra.join(", ")}]`
    );
  }
});

// ---- 任务 F1：recognition 段 required 集机械校验（运行时 schema ↔ schema.json ↔ openapi） ----
//
// 与上面的「字段集（properties）」校验互补：properties 描述「允许出现哪些键」，
// required 描述「启动校验时必须出现哪些键」。二者是**独立的契约面**。
//
// 本批（FACE-ARCWIN-006/007）新增 identifyThresholdArcFace 时**刻意只进 properties、不进 required**：
// 既有部署的 config.json 都没有该键，若设为必填会在启动校验时报「缺少必填字段」而被拒 ⇒ 升级即损坏配置。
// 反之，cascadePath 等 6 个既有键是 required——它们是历史契约，不能悄悄从 required 里掉出去。
//
// 若不机械看护 required，未来有人误把 identifyThresholdArcFace 加进 required（迁移安全静默破坏），
// 或改了运行时 required 却漏同步 config.schema.json/openapi.yaml，都不会被本测试发现。
//
// 需要一致的 required 三处拷贝：
//   ① src/win/src/JsonSchemaValidator.cpp  buildSettingsDocSchema()（**运行时**；形如
//      req.a.push_back(JsonValue::makeString("k"))，是字面量，可静态可靠抽取）
//   ② docs/windows-web-spa/config.schema.json  recognition.required
//   ③ docs/windows-web-spa/openapi.yaml  RecognitionSettings.required
const RECOGNITION_OPTIONAL_KEYS = ["identifyThresholdArcFace", "arcFaceInputName", "arcFaceOutputName"];

test("recognition 段 required 集：JsonSchemaValidator 运行时 ↔ schema.json ↔ openapi", () => {
  // ① 运行时 required：先切出识别段，再在段内抽 req.a.push_back(JsonValue::makeString("k"))。
  //    该形态在 buildSettingsDocSchema 的 recognition 块内为字面量，可静态可靠抽取。
  //    若未来被改写为运行时构造，下面的「非空」断言会先行失败示警（而非静默抽空后误判一致）。
  const runtimeReqLines = sliceRecognitionSection(
    jsonSchemaValidatorText,
    (l) => l.includes("// recognition"),
    (l) => l.includes('props.o["recognition"]'),
    "JsonSchemaValidator.cpp required 段"
  );
  const runtimeRequired = extractKeysInLines(
    runtimeReqLines,
    /req\.a\.push_back\(JsonValue::makeString\("([^"]+)"\)\)/g
  );

  const sources = [
    { label: "① JsonSchemaValidator.cpp 运行时 required", keys: runtimeRequired },
    { label: "② config.schema.json recognition.required", keys: [...canonical.recognition.required] },
    {
      label: "③ openapi.yaml RecognitionSettings.required",
      keys: [...((comps.RecognitionSettings || { required: [] }).required)],
    },
  ];

  // 防「静默通过」：每处 required 必须非空、无重复（字面量数 == 去重键数）、且**不含刻意可选的键**。
  // 最后一条把「identifyThresholdArcFace 等刻意不进 required」钉死在测试里——这正是迁移安全的护栏。
  for (const { label, keys } of sources) {
    assert.ok(
      keys.length > 0,
      `${label}: recognition.required 未抽出任何键（提取正则与源码写法脱节？）`
    );
    assert.equal(
      new Set(keys).size,
      keys.length,
      `${label}: required 键重复（字面量 ${keys.length} 条 ≠ 去重 ${new Set(keys).size} 条）`
    );
    for (const opt of RECOGNITION_OPTIONAL_KEYS) {
      assert.ok(
        !keys.includes(opt),
        `${label}: required 含刻意可选键 ${opt}（既有 config.json 缺该键会在启动校验被拒 ⇒ 迁移破坏）`
      );
    }
  }

  // 主断言：三处 required 集与 ② config.schema.json 两两一致，失败信息逐键列出缺/多。
  const reference = [...canonical.recognition.required].sort();
  for (const { label, keys } of sources) {
    const actual = [...new Set(keys)].sort();
    const missing = reference.filter((k) => !actual.includes(k));
    const extra = actual.filter((k) => !reference.includes(k));
    assert.deepEqual(
      actual,
      reference,
      `${label}: recognition.required 与 config.schema.json 不一致；缺 [${missing.join(", ")}]；多 [${extra.join(", ")}]`
    );
  }
});

