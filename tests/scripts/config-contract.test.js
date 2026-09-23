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

// 在属性子树行内收集「嵌套 type: object」节点，并判定其映射域内是否显式 additionalProperties: false。
// 节点判定：文本为 `type: object` 或列表项 `- type: object`（属性值内部，缩进 ≥ 10）。
// 映射域 = 自该节点行起的同层键；键缩进取 `type` token 的列（列表项 `- ` 占两列）。
// 返回 [{ label, hasNoExtra }]。与下方提取器同风格：按缩进工作，不做完整 YAML 语义解析。
function scanNestedObjects(subLines) {
  const nodes = [];
  for (let k = 0; k < subLines.length; k++) {
    const node = subLines[k];
    if (!/^(?:-\s+)?type:\s*object\s*$/.test(node.text)) continue;
    const isListItem = /^-\s/.test(node.text);
    const keyIndent = node.indent + (isListItem ? 2 : 0);
    let hasNoExtra = false;
    for (let j = k + 1; j < subLines.length; j++) {
      const l = subLines[j];
      if (l.indent < keyIndent) break; // 脱离该节点映射域
      if (isListItem && l.indent === node.indent && /^-\s/.test(l.text)) break; // 同级兄弟列表项
      if (l.text === "additionalProperties: false") hasNoExtra = true;
    }
    nodes.push({ label: node.text, hasNoExtra });
  }
  return nodes;
}

// 最小缩进式 YAML 提取器：抽取组件级的 required / properties 键、Settings 的 $ref 目标，
// 组件级 additionalProperties 是否为 false（FACE-TEST-001），以及属性值内**嵌套** type: object
// 节点及其 additionalProperties 收束（FACE-TEST-001①：postUrl 加密分支等嵌套对象的射程补强）。
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
    const comp = { required: [], props: new Set(), refs: new Set(), addPropsFalse: false, nestedObjects: [] };
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
          const subStart = i;
          while (i < lines.length && lines[i].indent >= 10) {
            const rm = lines[i].text.match(/^\$ref:\s*["']?#\/components\/schemas\/([^"']+)["']?/);
            if (rm) comp.refs.add(rm[1]);
            i++;
          }
          // FACE-TEST-001①：属性值内的嵌套 type: object 亦须收束（如 postUrl 加密分支，缩进 14）。
          for (const nested of scanNestedObjects(lines.slice(subStart, i))) {
            comp.nestedObjects.push({ owner: key, label: nested.label, hasNoExtra: nested.hasNoExtra });
          }
        }
      } else if (l.indent === 6 && l.text === "additionalProperties: false") {
        // 仅组件级（缩进 6）的键收束计入；嵌套对象（缩进 ≥ 10，如 postUrl 的加密分支）
        // 由上方 properties 内层循环消费，不会被误判为组件级 additionalProperties。
        comp.addPropsFalse = true;
        i++;
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
// 本批（FACE-ARCWIN-006/007）把 identifyThresholdArcFace **收束进 required**（三处同步）。
// 迁移安全由**键级规范化**承担：WinJsonConfig.cpp 的 normalizeRecognitionKeys 在 schema 校验前
// 对 recognition 段做「缺键补默认」（补入值取 defaultAppConfig()，与解析侧缺键回退一致），
// 故既有缺该键的 config.json 仍能通过校验、不会被拒 ⇒ 不会降级为默认值运行。
// 而 cascadePath 等 6 个既有键本就是历史 required，不能悄悄掉出去。
//
// 反之，arcFaceInputName / arcFaceOutputName 仍**刻意不进 required**：它们的「空串 = OpenCV 自动」
// 是合法默认值，语义与阈值不同，键级规范化亦不为其补位。若不机械看护，未来有人误把它们加进
// required（迁移破坏），或改了运行时 required 却漏同步 config.schema.json/openapi.yaml，都不会被发现。
//
// 需要一致的 required 三处拷贝：
//   ① src/win/src/JsonSchemaValidator.cpp  buildSettingsDocSchema()（**运行时**；形如
//      req.a.push_back(JsonValue::makeString("k"))，是字面量，可静态可靠抽取）
//   ② docs/windows-web-spa/config.schema.json  recognition.required
//   ③ docs/windows-web-spa/openapi.yaml  RecognitionSettings.required
const RECOGNITION_OPTIONAL_KEYS = ["arcFaceInputName", "arcFaceOutputName"];

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
  // 「刻意可选」= 两个节点名（空串即 OpenCV 自动，属合法默认值）；identifyThresholdArcFace 已在本批
  // 收束进 required，其迁移安全由「规范化先补键」保证，故下方另有一条**必须包含**它的反向护栏。
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

  // 反向护栏（本批收束）：identifyThresholdArcFace 必须出现在三处 required——其迁移安全由
  // WinJsonConfig.cpp 的键级规范化承担（校验前补默认），故可以且应当设为必填。
  for (const { label, keys } of sources) {
    assert.ok(
      keys.includes("identifyThresholdArcFace"),
      `${label}: required 缺 identifyThresholdArcFace（本批已收束进 required；缺失即回退到迁移破坏风险）`
    );
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

// ---- FACE-TEST-001：additionalProperties 收束的机械防线 ----
//
// 背景：上一轮把 Settings 与各 *Settings 段收束为 additionalProperties: false（拒绝未知键），
// 三处拷贝（openapi.yaml / config.schema.json / JsonSchemaValidator.cpp 运行时 schema）。
// 但本文件此前只对拍 required 与 properties，**不查 additionalProperties** ⇒ 该收束无任何
// 自动防线，未来任一处漂移（删掉/改成 true）都不会被发现。
//
// 判别逻辑（GREEN）：三处逐段均为显式 additionalProperties: false。
//   · openapi：复用 extractSettingsComponents 扩展字段 addPropsFalse（仅组件级缩进 6 计入；
//     postUrl 等嵌套对象由 properties 内层循环消费，不误判）；
//   · config.schema.json：JSON 解析后逐段读 def.additionalProperties；
//   · 运行时：在 buildSettingsDocSchema 内按 `props.o["<段>"] = std::move(` 标记切段，逐段回扫
//     `X.o["additionalProperties"] = JsonValue::makeBool(false);`（同 FACE-ARCWIN-004/006 的切段手法）。
//
// 会在下列改法下变红（逐条）：
//   1) openapi 删掉任一 *Settings 组件的 `additionalProperties: false` 行 → addPropsFalse 为 false；
//   2) openapi 把某组件改成 `additionalProperties: true`（或其它值）→ 不置 true；
//   3) openapi 删除 Settings 根组件的该行；
//   4) config.schema.json 删掉根或任一段的 "additionalProperties": false → 读为 undefined；
//   5) config.schema.json 把任一处改成 true；
//   6) JsonSchemaValidator.cpp 删/注释任一段的 `X.o["additionalProperties"] = JsonValue::makeBool(false);`；
//   7) JsonSchemaValidator.cpp 把任一处改成 makeBool(true)；
//   8) 新增一个段（openapi + schema 同时加）但运行时漏加 → 运行时逐段回扫缺该段即红。
test("additionalProperties：openapi Settings/各 *Settings 与 config.schema.json 逐段均为 false", () => {
  // ① config.schema.json 根 + 每个 object 段
  assert.equal(
    schemaRoot.additionalProperties,
    false,
    "config.schema.json 根缺 \"additionalProperties\": false"
  );
  for (const [section, def] of Object.entries(schemaRoot.properties)) {
    if (def.type !== "object") continue; // schemaVersion 标量段跳过
    assert.equal(
      def.additionalProperties,
      false,
      `config.schema.json ${section} 缺 "additionalProperties": false`
    );
  }

  // ② openapi.yaml Settings 根 + 每个 *Settings 组件（复用扩展后的提取器 addPropsFalse）
  assert.ok(comps.Settings, "openapi 缺 Settings 组件");
  assert.equal(
    comps.Settings.addPropsFalse,
    true,
    "openapi Settings 组件缺 additionalProperties: false"
  );
  for (const [section] of Object.entries(canonical)) {
    const compName = componentName(section);
    const comp = comps[compName];
    assert.ok(comp, `openapi 缺 ${compName} 组件`);
    assert.equal(
      comp.addPropsFalse,
      true,
      `openapi ${compName} 组件缺 additionalProperties: false`
    );
  }
});

// 运行时 schema 逐段回扫：在 [上一段 props.o 标记, 本段 props.o 标记] 行区间内，
// 是否出现组件级 `X.o["additionalProperties"] = JsonValue::makeBool(false);`。
function runtimeSectionHasNoExtra(lines, section, contextLabel) {
  const endIdx = lines.findIndex((l) => l.includes(`props.o["${section}"] = std::move(`));
  assert.ok(
    endIdx >= 0,
    `${contextLabel}: 未定位 props.o["${section}"]（提取标记与源码写法脱节？）`
  );
  let startIdx = 0;
  for (let j = endIdx - 1; j >= 0; j--) {
    if (/props\.o\["\w+"\]\s*=\s*std::move\(/.test(lines[j])) { startIdx = j + 1; break; }
  }
  for (let j = startIdx; j <= endIdx; j++) {
    if (/\.o\["additionalProperties"\]\s*=\s*JsonValue::makeBool\(false\)/.test(lines[j])) {
      return true;
    }
  }
  return false;
}

test("additionalProperties：JsonSchemaValidator 运行时 schema 逐段均为 false", () => {
  // 边界说明（FACE-TEST-001②，如实披露）：本断言守的是**三处副本的 schema 文本/声明一致性**——
  // 即运行时 schema 的源码里仍写着 `X.o["additionalProperties"] = JsonValue::makeBool(false);`；
  // 它**不**证明 runtime 的 validateSchemaRec 真的执行了未知键收束（把该强制块删掉本断言仍绿）。
  // 运行时是否真的拒绝未知键（行为级），由 Win 侧行为用例覆盖（tests/win/test_win_json_config.cpp，FIX-1 补）。
  const lines = jsonSchemaValidatorText.split(/\r?\n/);
  const fnStart = lines.findIndex((l) =>
    l.includes("JsonValue JsonSchemaValidator::buildSettingsDocSchema()")
  );
  assert.ok(fnStart >= 0, "未定位 JsonSchemaValidator::buildSettingsDocSchema()");
  const firstPropMarker = lines.findIndex(
    (l, i) => i > fnStart && /props\.o\["\w+"\]\s*=\s*std::move\(/.test(l)
  );
  assert.ok(firstPropMarker > fnStart, "buildSettingsDocSchema 内未找到 props.o 段标记");

  // 根 Settings：函数起点到首个 props.o 段标记之间须有 schema.o["additionalProperties"] = false。
  assert.ok(
    lines.slice(fnStart, firstPropMarker).some((l) =>
      /^\s*schema\.o\["additionalProperties"\]\s*=\s*JsonValue::makeBool\(false\)/.test(l)
    ),
    "JsonSchemaValidator.cpp 根 Settings 缺 additionalProperties: false"
  );

  // 各段逐段回扫（段名取自 config.schema.json 的权威段集）。
  for (const [section] of Object.entries(canonical)) {
    assert.ok(
      runtimeSectionHasNoExtra(lines, section, "JsonSchemaValidator.cpp"),
      `JsonSchemaValidator.cpp 运行时 schema 的 ${section} 段缺 additionalProperties: false`
    );
  }

  // 敏感字段加密对象（postUrl 的 object 分支）：buildEncryptedStringSchema 亦须 additionalProperties: false。
  assert.ok(
    lines.some((l) =>
      /^\s*s\.o\["additionalProperties"\]\s*=\s*JsonValue::makeBool\(false\)/.test(l)
    ),
    "JsonSchemaValidator.cpp buildEncryptedStringSchema 缺 additionalProperties: false"
  );
});

// ---- FACE-TEST-001①：additionalProperties 收束的**嵌套对象**射程补强 ----
//
// 背景：此前的 addPropsFalse 断言只覆盖 openapi **组件级（缩进 6）**；给 openapi
// PosterSettings.postUrl 的**嵌套**加密对象（缩进 14）补的 `additionalProperties: false`
// 不在射程内 ⇒ 删掉它用例仍绿（四代理审查 S4 抓出的盲区，本批收口）。
//
// 权威来源：config.schema.json 的对应嵌套对象 `poster.postUrl.oneOf[1]` 亦为 false
// （由下方 JSON 侧断言逐节点校验），故嵌套层三处副本（openapi 嵌套 / config.schema.json /
// 运行时 buildEncryptedStringSchema）同样必须一致。
//
// 会在下列改法下变红（逐条）：
//   a) openapi 删掉 postUrl 加密分支（缩进 14）的 `additionalProperties: false`；
//   b) openapi 把该行改成 `additionalProperties: true` 或其它值（非精确 false 不置位）；
//   c) openapi 新增任一 *Settings 属性值内的嵌套 object 却漏配该行；
//   d) 提取射程失效（scanNestedObjects 抽到 0 个节点）→ 下方 checked 护栏先行失败示警；
//   e) config.schema.json 删/改任一 type: object 节点（含 postUrl oneOf[1]）的 additionalProperties。

// 递归收集 JSON schema 中所有 type: object 节点（含 oneOf/anyOf 分支与 properties 子树）。
function collectObjectNodes(node, nodePath, out) {
  if (!node || typeof node !== "object") return;
  (node.oneOf || []).forEach((b, i) => collectObjectNodes(b, `${nodePath}.oneOf[${i}]`, out));
  (node.anyOf || []).forEach((b, i) => collectObjectNodes(b, `${nodePath}.anyOf[${i}]`, out));
  if (node.type === "object") {
    out.push({ path: nodePath || "(root)", additionalProperties: node.additionalProperties });
  }
  for (const [k, v] of Object.entries(node.properties || {})) {
    collectObjectNodes(v, `${nodePath}.${k}`, out);
  }
}

test("additionalProperties：openapi 各 *Settings 的**嵌套** object 分支亦为 false（postUrl 加密分支在射程内）", () => {
  // 射程 = config 契约组件（Settings + 各 *Settings）。Envelope 等**非契约**响应组件不纳入：
  // 其 data/error 为自由结构，不属配置文档契约，刻意不加该约束（纳入会误报）。
  const contractComps = ["Settings", ...Object.keys(canonical).map(componentName)];
  let checked = 0;
  for (const name of contractComps) {
    const comp = comps[name];
    assert.ok(comp, `openapi 缺 ${name} 组件`);
    for (const node of comp.nestedObjects) {
      checked++;
      assert.ok(
        node.hasNoExtra,
        `openapi ${name}.${node.owner} 的嵌套对象「${node.label}」缺 additionalProperties: false（未知键收束在嵌套层失效）`
      );
    }
  }
  // 防「静默通过」：射程必须真的命中至少 1 个嵌套对象（当前即 postUrl 加密分支）；
  // 若提取器与 YAML 写法脱节而抽空，此处先行变红，而非「空 == 空」误判一致。
  assert.ok(
    checked > 0,
    "openapi 未抽到任何嵌套 type: object（postUrl 加密分支应至少在射程内）——提取射程失效"
  );
});

test("additionalProperties：config.schema.json 全部 type: object 节点（含嵌套 oneOf 分支）均为 false", () => {
  const nodes = [];
  collectObjectNodes(schemaRoot, "", nodes);
  // 射程自证：必须真的抽到嵌套分支 poster.postUrl.oneOf[1]（否则仅覆盖顶层段 = 旧射程）。
  assert.ok(
    nodes.some((n) => n.path === ".poster.postUrl.oneOf[1]"),
    "config.schema.json 未抽到 poster.postUrl.oneOf[1] 嵌套 object——射程失效"
  );
  for (const n of nodes) {
    assert.equal(
      n.additionalProperties,
      false,
      `config.schema.json ${n.path} 缺 "additionalProperties": false`
    );
  }
});



