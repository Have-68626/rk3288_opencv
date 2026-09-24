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
// 组件级 additionalProperties 是否为 false（FACE-TEST-001），属性值内**嵌套** type: object
// 节点及其 additionalProperties 收束（FACE-TEST-001①：postUrl 加密分支等嵌套对象的射程补强），
// 以及**属性自身的值域** minimum/maximum（FACE-VALUE-RANGE：三处值域机械对拍）。
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
    const comp = {
      required: [], props: new Set(), refs: new Set(), addPropsFalse: false,
      nestedObjects: [], bounds: new Map(),
    };
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
          // FACE-VALUE-RANGE：该属性**自身**的值域只取缩进 10 的 minimum/maximum；
          // 更深缩进属嵌套对象（如 postUrl 的 oneOf/object 分支），不在对拍射程（理由见文末射程说明）。
          let bMin = null;
          let bMax = null;
          for (const sl of lines.slice(subStart, i)) {
            if (sl.indent !== 10) continue;
            const mm = /^minimum:\s*(\S+)$/.exec(sl.text);
            if (mm) bMin = Number(mm[1]);
            const xm = /^maximum:\s*(\S+)$/.exec(sl.text);
            if (xm) bMax = Number(xm[1]);
          }
          if (bMin !== null || bMax !== null) comp.bounds.set(key, { min: bMin, max: bMax });
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
  // 射程 = config 契约组件。判据（可机械复核）：契约组件 ≡ 出现在 config.schema.json 顶层段的
  //   对象组件 ∪ {Settings 根}，即 ["Settings", ...Object.keys(canonical).map(componentName)]；
  //   不在此集合内者即**非契约**，不纳入本断言（见下方 contractComps 定义）。
  //   非契约者 = 不在 contractComps 中的组件（当前为 Envelope 系；另有 BadRequest / NotFound /
  //   ServiceUnavailable 等响应/错误组件，其结构不在配置文档契约的提取器辖区）。
  //   此处**刻意不写死「N 个组件 / M 个 type:object 节点」之类的计数**：openapi 一演进计数即静默
  //   过期，正属本仓批评过的「注释漂移」；判据以 contractComps 的机械定义为准，由代码自证。
  // 为何刻意排斥 Envelope 系（非契约 ⇒ 不应加 additionalProperties: false 约束）：
  //   · Envelope.data 是**纯自由结构**——该节点只有 `type: object`，既无 properties 也无
  //     additionalProperties（逐字见 openapi.yaml：description 为「响应数据（具体结构因端点而异）」）；
  //     响应 data 的形态随端点而异（Settings / 模型列表 / 人脸库快照 …），本就不可枚举。
  //   · Envelope.error 是 **API 响应错误结构**（code/message/details），属**响应**契约而非**配置文档**
  //     契约；其 details 承载自由诊断串，对之收束未知键与其用途冲突。
  //   故纳入会**误报**：探针实测二者的 hasNoExtra 均为 false，必然令下方断言变红。
  // 契约组件内当前的嵌套 type:object 仅 PosterSettings.postUrl（oneOf 加密分支）一个，已在射程内；
  //   若未来在契约组件属性值内新增嵌套对象而漏配该行，scanNestedObjects 会自动纳入并令断言变红。
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

// ---- FACE-VALUE-RANGE：三处值域（minimum/maximum）机械对拍 ----
//
// 补的是什么缺口（本批审计）：config.schema.json / openapi.yaml / JsonSchemaValidator.cpp 的
// **运行时 schema** 是三份**值域拷贝**，但本文件此前只对拍「键集」与「required 集」，
// **没有任何 min/max 断言**——本批把 `recognition.identifyThreshold` 的 `maximum` 从 `100000`
// 收到 `64`，**若漏改任一处，不会有任何测试变红**（三路审计独立指出）。本断言闭合该缺口。
//
// 射程（三处**同口径**是对拍成立的前提）：
//   ① 每个**段**的直接数值属性；② 顶层标量 `schemaVersion`；
//   ③ **不含** poster.postUrl 的加密对象分支——本用例的三个提取器都只抽**属性级**值域
//      （schema.json 的段直接属性 / openapi 缩进 10 的属性自身值域 / buildSettingsDocSchema 内的
//      语句），而该分支嵌在 oneOf 里（openapi 缩进 16–18；schema.json 在 `oneOf[1].properties`；
//      运行时在**另一个函数** `buildEncryptedStringSchema()` 内）⇒ 纳入需同时改三处提取器口径，
//      不属本用例的射程。它由紧随其后的「补漏」用例单独承担。
//      ❌ 更正旧说法（原文）：此处曾写「openapi 未声明该处值域 ⇒ 纳入会让 openapi 单边少一键」——
//      该理由**已不成立**：本批同批给 openapi 的加密分支补上了 `v: minimum/maximum = 1` 与
//      `alg` 的 enum，三处**都**声明了 `v = [1, 1]`；真正的排除理由只有「提取器口径只到属性级」。
//      ❌ 同时更正原文后半句「该分支的值域由上方 additionalProperties 的嵌套射程与 C++ 侧行为用例
//      各自覆盖」——**两半皆不成立**（S4 独立实测）：additionalProperties 系列断言判的是该关键字
//      **是否出现**，与 minimum/maximum **无关**；`tests/win/test_win_json_config.cpp` 中检索
//      `buildEncryptedStringSchema` / 加密对象 / AES-256-GCM **0 命中** ⇒ 在补漏用例之前，
//      `v ∈ [1,1]` 与 `alg` 的取值域在全仓**无任何机械或行为看护**（已知盲区，本批已补 `v` 的一半）。
// 判据：三处「带界键 → (min, max)」映射**逐键相等**；每处**非空**且含全部锚键
//   （防「提取器与源码写法脱节 ⇒ 空 == 空」静默通过）。
//
// 会在下列改法下变红：任一处改 min/max、任一处删掉某一键的上/下界、三处新增/删除带界键而只改
//   其中一两处、源码改成提取器不认识的写法（此时 evalNumericExpr/非空断言先行失败示警）。

/** 锚键：分布在各段且量纲各异，抽空/漏抽时最先暴露 */
const BOUNDS_ANCHOR_KEYS = ["schemaVersion", "camera.width", "http.port", "recognition.identifyThreshold"];

/**
 * 解析 C++ 源里的数值字面量表达式。本仓既有写法只有「乘积」形态（如 `60 * 60 * 1000`）。
 * 遇到不认识的写法**报错**而非静默取错值——将来出现新形态时先红再补。
 * @param {string} expr
 * @param {string} label 出错信息用的上下文
 * @returns {number}
 */
function evalNumericExpr(expr, label) {
  const s = String(expr).replace(/\s+/g, "");
  const m = /^([+-]?\d+(?:\.\d+)?)((?:[*/]\d+(?:\.\d+)?)*)$/.exec(s);
  assert.ok(
    m,
    `${label}: 无法解析的数值表达式「${expr}」—— 本判据只支持 [+-]?字面量 与 * / 乘积；` +
      `若源码改用了别的写法，请扩展 evalNumericExpr（不要静默取错值）`
  );
  let v = Number(m[1]);
  for (const op of m[2].match(/[*/]\d+(?:\.\d+)?/g) || []) {
    const n = Number(op.slice(1));
    v = op[0] === "*" ? v * n : v / n;
  }
  return v;
}

/**
 * 从 config.schema.json 抽值域：Map<path, {min, max, label}>。
 * path = `段.键`（顶层标量则为裸键名）；只取段的**直接**数值属性——对象值属性
 * （如 poster.postUrl 的 oneOf 加密分支）不在射程（理由见本节射程说明）。
 * @param {object} schema 已 JSON.parse 的 schema 根
 * @returns {Map<string, {min:number, max:number, label:string}>}
 */
function extractSchemaJsonBounds(schema) {
  const out = new Map();
  for (const [key, def] of Object.entries(schema.properties || {})) {
    if (def.type !== "object") {
      if (def.type === "number" && (def.minimum !== undefined || def.maximum !== undefined)) {
        out.set(key, { min: def.minimum, max: def.maximum, label: `config.schema.json ${key}` });
      }
      continue;
    }
    for (const [k, v] of Object.entries(def.properties || {})) {
      if (v.type !== "number") continue;
      if (v.minimum === undefined && v.maximum === undefined) continue;
      out.set(`${key}.${k}`, {
        min: v.minimum,
        max: v.maximum,
        label: `config.schema.json ${key}.${k}`,
      });
    }
  }
  return out;
}

/**
 * 从 openapi.yaml 的**契约组件**抽值域：Map<path, {min, max, label}>。
 * path 用 schema 段名（`COMPONENT_TO_SECTION` 反查），Settings 根组件的属性用裸键名
 * ⇒ 与 extractSchemaJsonBounds 的口径一致。非契约组件（Envelope 系）不在射程。
 * @returns {Map<string, {min:number, max:number, label:string}>}
 */
function extractOpenapiBounds() {
  const out = new Map();
  for (const [compName, comp] of Object.entries(comps)) {
    if (compName !== "Settings" && !COMPONENT_TO_SECTION.has(compName)) continue;
    const section = compName === "Settings" ? "" : COMPONENT_TO_SECTION.get(compName);
    for (const [key, b] of comp.bounds) {
      const path = section ? `${section}.${key}` : key;
      out.set(path, { min: b.min, max: b.max, label: `openapi.yaml ${compName}.${key}` });
    }
  }
  return out;
}

/**
 * 从 JsonSchemaValidator.cpp 的 `buildSettingsDocSchema()` 抽运行时值域：Map<path, {min,max,label}>。
 * 同一函数内混用两种写法，都要处理：
 *   a) 内联：`X.o["<key>"] = objBoolInt(true, MIN, MAX);`（首参 `false` = 无界，跳过）；
 *   b) 显式块：`t.o["minimum"] = JsonValue::makeNumber(MIN);` + `…maximum…` +
 *      `X.o["<key>"] = std::move(t);`（向上回扫收集，遇其它语句即停）。
 * 段名（path 前缀）取自**段注释标记** `    // <section>`（与本文件既有 required 段切片同一手法），
 * **不能**用变量名——变量名会跨段复用（`dp` 同时是 dnn 与 display），作段标识会串段。
 * 射程止于 buildSettingsDocSchema：`buildEncryptedStringSchema()` 属加密对象分支，不在射程。
 * @param {string} text
 * @returns {Map<string, {min:number, max:number, label:string}>}
 */
function extractRuntimeBounds(text) {
  const lines = text.split(/\r?\n/);
  const fnStart = lines.findIndex((l) =>
    l.includes("JsonValue JsonSchemaValidator::buildSettingsDocSchema()")
  );
  assert.ok(fnStart >= 0, "未定位 JsonSchemaValidator::buildSettingsDocSchema()");
  const fnEnd = lines.findIndex(
    (l, i) => i > fnStart && /^\s*schema\.o\["properties"\]\s*=\s*std::move\(props\);/.test(l)
  );
  assert.ok(fnEnd > fnStart, 'buildSettingsDocSchema 末尾未定位 schema.o["properties"] = std::move(props);');

  // 段标记：**恰好 4 空格**缩进的 `// <小写段名>`（可带全角括号说明）。段内注释缩进更深（8 格），
  // 故不会被误当段标记。
  const markers = [];
  for (let i = fnStart + 1; i < fnEnd; i++) {
    const m = /^    \/\/ ([a-z][a-z0-9]*)(?:[ (（].*)?$/.exec(lines[i]);
    if (m) markers.push({ idx: i, name: m[1] });
  }
  assert.ok(markers.length > 0, "JsonSchemaValidator.cpp 未定位到任何段注释标记（提取射程失效）");

  const regions = [{ name: "", start: fnStart, end: markers[0].idx }];
  markers.forEach((mk, k) => {
    regions.push({
      name: mk.name,
      start: mk.idx,
      end: k + 1 < markers.length ? markers[k + 1].idx : fnEnd,
    });
  });

  const inlineRe = /^\s*\w+\.o\["([^"]+)"\]\s*=\s*objBoolInt\((true|false),\s*([^,]+),\s*([^)]+)\);/;
  const moveRe = /^\s*\w+\.o\["([^"]+)"\]\s*=\s*std::move\(/;
  const minRe = /\bo\["minimum"\]\s*=\s*JsonValue::makeNumber\(([^)]+)\)/;
  const maxRe = /\bo\["maximum"\]\s*=\s*JsonValue::makeNumber\(([^)]+)\)/;

  const out = new Map();
  for (const region of regions) {
    for (let i = region.start; i < region.end; i++) {
      const line = lines[i];
      // 段装配行 `props.o["<段>"] = std::move(...)`：只在**非 root** 段跳过；
      // root 段里唯一的一条正是顶层标量 schemaVersion 的装配行，必须保留。
      if (region.name && /^\s*props\.o\[/.test(line)) continue;

      const inl = inlineRe.exec(line);
      if (inl) {
        const [, key, hasMinMax, minRaw, maxRaw] = inl;
        if (hasMinMax !== "true") continue; // objBoolInt(false, …) = 无界，与 schema.json 的无界属性对应
        const path = region.name ? `${region.name}.${key}` : key;
        out.set(path, {
          min: evalNumericExpr(minRaw, `${path} 的下界`),
          max: evalNumericExpr(maxRaw, `${path} 的上界`),
          label: `JsonSchemaValidator.cpp ${path}`,
        });
        continue;
      }

      const mv = moveRe.exec(line);
      if (!mv) continue;
      const key = mv[1];
      let min = null;
      let max = null;
      for (let j = i - 1; j >= region.start; j--) {
        const prev = lines[j];
        if (prev.trim() === "" || /^\s*\/\//.test(prev)) continue; // 块内注释很长，须跳过
        const mm = minRe.exec(prev);
        const xm = maxRe.exec(prev);
        if (mm || xm) {
          if (mm) min = evalNumericExpr(mm[1], `${key} 的下界`);
          if (xm) max = evalNumericExpr(xm[1], `${key} 的上界`);
          continue;
        }
        break; // 其它语句 ⇒ 该显式块结束（无界）
      }
      if (min === null || max === null) continue; // 无界（如 properties / required 的装配行）
      const path = region.name ? `${region.name}.${key}` : key;
      out.set(path, { min, max, label: `JsonSchemaValidator.cpp ${path}` });
    }
  }
  return out;
}

test("值域：config.schema.json ↔ openapi.yaml ↔ JsonSchemaValidator 运行时 schema 逐键对拍 minimum/maximum", () => {
  const sources = [
    { label: "③ config.schema.json", map: extractSchemaJsonBounds(schemaRoot) },
    { label: "④ openapi.yaml", map: extractOpenapiBounds() },
    { label: "② JsonSchemaValidator.cpp 运行时 schema", map: extractRuntimeBounds(jsonSchemaValidatorText) },
  ];

  // 防「静默通过」：三处都必须非空且含全部锚键。抽空 ⇒ 直接报错，而不是「空 == 空」通过。
  for (const { label, map } of sources) {
    assert.ok(
      map.size > 0,
      `${label}: 未抽到任何带界（minimum/maximum）键——提取器与源码写法脱节？`
    );
    for (const anchor of BOUNDS_ANCHOR_KEYS) {
      assert.ok(
        map.has(anchor),
        `${label}: 锚键 ${anchor} 未抽到值域（该处漏改或提取失效）；实际键：` +
          `${[...map.keys()].sort().join(", ")}`
      );
    }
  }

  // 数量相等（三处同射程的直接体现；任一处漏抽/多抽都先在此报出，比逐键 diff 更早定位）
  const counts = sources.map((s) => s.map.size);
  assert.deepEqual(
    counts.slice(1),
    [counts[0], counts[0]],
    `三处带界键数量不一致：${sources.map((s, i) => `${s.label}=${counts[i]}`).join("；")}`
  );

  // 主断言：逐键 min/max 相等（以 config.schema.json 为参照；失败信息逐键列出缺/多/值不同）
  const reference = sources[0].map;
  for (const { label, map } of sources.slice(1)) {
    const missing = [...reference.keys()].filter((k) => !map.has(k)).sort();
    const extra = [...map.keys()].filter((k) => !reference.has(k)).sort();
    const mismatched = [...reference.keys()]
      .filter((k) => map.has(k))
      .filter((k) => map.get(k).min !== reference.get(k).min || map.get(k).max !== reference.get(k).max)
      .map(
        (k) =>
          `${k}: ${label}=[${map.get(k).min}, ${map.get(k).max}] vs ` +
          `config.schema.json=[${reference.get(k).min}, ${reference.get(k).max}]`
      )
      .sort();
    assert.deepEqual(
      { missing, extra, mismatched },
      { missing: [], extra: [], mismatched: [] },
      `${label} 的值域与 config.schema.json 不一致；缺键 [${missing.join(", ")}]；` +
        `多键 [${extra.join(", ")}]；值不同 [${mismatched.join(" | ")}]`
    );
  }

  // 本批审计点名的回归护栏：LBPH 识别阈值的上界从 100000 收到 64 ⇒ 三处必须同为 [1, 64]。
  // 单独再断言一次，使失败信息直指该键（不必从上面的大 diff 里找）。
  for (const { label, map } of sources) {
    const b = map.get("recognition.identifyThreshold");
    assert.deepEqual(
      { min: b.min, max: b.max },
      { min: 1, max: 64 },
      `${label}: recognition.identifyThreshold 应为 [1, 64]（本批把 maximum 从 100000 收到 64，` +
        `任一处漏改即须在此红）`
    );
  }
});

// ---- FACE-VALUE-RANGE 补漏：poster.postUrl 加密分支的 `v` 三处值域对拍 ----
//
// 补的是什么缺口：上方射程③刻意把该分支排除（三处提取器都只抽属性级值域），而它此前**没有任何**
// 看护——射程说明里的「由 additionalProperties 与 C++ 行为用例各自覆盖」经实测两半皆不成立
// （详见上方射程说明处的更正）。本用例只补**能三处同口径对拍**的那部分：`v` 的 [1, 1]。
//
// 形态差异（正因如此才需要专用提取器，不能复用上方三个）：
//   · config.schema.json：`poster.postUrl.oneOf[1].properties.v`（JSON，直接取，无解析风险）；
//   · openapi.yaml：PosterSettings.postUrl 的 oneOf object 分支 `properties:` 下的键（缩进 16/18/20）；
//   · 运行时 schema：**另一个函数** `JsonSchemaValidator::buildEncryptedStringSchema()` 内的显式块
//     （`v.o["minimum"] = …` + `props.o["v"] = std::move(v);`；上方 extractRuntimeBounds 的射程
//     止于 buildSettingsDocSchema，刻意不含它）。
//
// 已知的**不对拍**项（如实登记，不假造看护）：`alg` 只在两份**文档**里有取值域（enum: AES-256-GCM），
//   运行时 schema 的 `buildEncryptedStringSchema()` 对 `alg` **只声明 `type: string`、没有 enum**
//   ⇒ 三处同口径对拍不成立，本节只对拍两份文档 + 登记该不对称（是否收口由后续任务定，不在本批）。

/** config.schema.json 的 poster.postUrl.oneOf 的 object（加密）分支。 */
function schemaJsonEncryptedBranch() {
  const oneOf = schemaRoot.properties?.poster?.properties?.postUrl?.oneOf;
  assert.ok(Array.isArray(oneOf), "config.schema.json poster.postUrl 缺 oneOf");
  const branch = oneOf.find((b) => b && b.type === "object" && b.properties);
  assert.ok(branch, "config.schema.json poster.postUrl 无可用的加密 object 分支（提取失效）");
  return branch;
}

/**
 * 从 openapi.yaml 的 PosterSettings.postUrl 加密分支抽属性表：Map<键, {min,max,enum}>。
 * 按缩进定位（与 extractSettingsComponents 同手法）：组件 4 / postUrl 8 / oneOf object 分支的
 * `properties:` 14 / 键 16 / 键下条目 18（enum 项 20）。抽不到就是射程失效 ⇒ 断言报错。
 * @returns {Map<string, {min:number|null, max:number|null, enum:string[]|null}>}
 */
function extractOpenapiEncryptedBranch() {
  const lines = openapiText.split(/\r?\n/).map((raw) => ({
    indent: raw.length - raw.trimStart().length,
    text: raw.trim(),
  }));
  const compStart = lines.findIndex((l) => l.indent === 4 && l.text === "PosterSettings:");
  assert.ok(compStart >= 0, "openapi.yaml 未定位到 PosterSettings 组件");
  let compEnd = lines.length;
  for (let i = compStart + 1; i < lines.length; i++) {
    if (lines[i].text !== "" && lines[i].indent <= 4) {
      compEnd = i;
      break;
    }
  }
  const postUrlIdx = lines.findIndex(
    (l, i) => i > compStart && i < compEnd && l.indent === 8 && l.text === "postUrl:"
  );
  assert.ok(postUrlIdx >= 0, "openapi.yaml PosterSettings 缺 postUrl 属性");
  // 加密分支的 `properties:`（缩进 14）；明文分支 `- type: string` 没有 properties。
  const propsIdx = lines.findIndex(
    (l, i) => i > postUrlIdx && i < compEnd && l.indent === 14 && l.text === "properties:"
  );
  assert.ok(propsIdx >= 0, "openapi.yaml PosterSettings.postUrl 缺加密分支的 properties（提取射程失效）");

  const out = new Map();
  for (let i = propsIdx + 1; i < compEnd && lines[i].indent === 16; i++) {
    const m = /^([^:]+):\s*$/.exec(lines[i].text);
    if (!m) continue;
    const info = { min: null, max: null, enum: null };
    let j = i + 1;
    while (j < compEnd && lines[j].indent > 16) {
      const mm = /^minimum:\s*(\S+)$/.exec(lines[j].text);
      if (mm) info.min = Number(mm[1]);
      const xm = /^maximum:\s*(\S+)$/.exec(lines[j].text);
      if (xm) info.max = Number(xm[1]);
      if (lines[j].text === "enum:") {
        const items = [];
        for (let k = j + 1; k < compEnd && lines[k].indent > lines[j].indent; k++) {
          if (!lines[k].text.startsWith("- ")) break;
          items.push(lines[k].text.slice(2).trim());
        }
        info.enum = items;
      }
      j++;
    }
    out.set(m[1].trim(), info);
    i = j - 1;
  }
  return out;
}

/**
 * 从 JsonSchemaValidator.cpp 的 `buildEncryptedStringSchema()` 抽 `v` 的 [min, max]。
 * 变量名**不写死**：从装配行 `props.o["v"] = std::move(<obj>);` 反读对象名，再向上回扫该对象的
 * `o["minimum"/"maximum"]` 赋值（遇其它语句即停）——改名不会误判。
 * @returns {{min:number, max:number}}
 */
function extractRuntimeEncryptedV() {
  const lines = jsonSchemaValidatorText.split(/\r?\n/);
  const start = lines.findIndex((l) => l.includes("JsonSchemaValidator::buildEncryptedStringSchema()"));
  assert.ok(start >= 0, "未定位 JsonSchemaValidator::buildEncryptedStringSchema()");
  let end = lines.length;
  for (let i = start + 1; i < lines.length; i++) {
    if (/^\}\s*$/.test(lines[i])) {
      end = i;
      break;
    }
  }
  assert.ok(end > start, "buildEncryptedStringSchema 的函数体末尾未定位");

  const moveRe = /^\s*props\.o\["v"\]\s*=\s*std::move\((\w+)\);/;
  const moveIdx = lines.findIndex((l, i) => i > start && i < end && moveRe.test(l));
  assert.ok(moveIdx > start, 'buildEncryptedStringSchema 未定位 props.o["v"] = std::move(...) 装配行');
  const objVar = moveRe.exec(lines[moveIdx])[1];

  const boundRe = new RegExp(
    `^\\s*${objVar}\\.o\\["(minimum|maximum)"\\]\\s*=\\s*JsonValue::makeNumber\\(([^)]+)\\);`
  );
  let min = null;
  let max = null;
  for (let i = moveIdx - 1; i >= start; i--) {
    const l = lines[i];
    const t = l.trim();
    if (t === "" || t === "{" || t === "}" || t.startsWith("//")) continue; // 块界与注释
    const m = boundRe.exec(l);
    if (!m) break; // 其它语句 ⇒ 该显式块结束
    if (m[1] === "minimum") min = evalNumericExpr(m[2], "buildEncryptedStringSchema 的 v 下界");
    else max = evalNumericExpr(m[2], "buildEncryptedStringSchema 的 v 上界");
  }
  assert.ok(
    min !== null && max !== null,
    `buildEncryptedStringSchema 的 v 未抽到完整 [min, max]（提取器与源码写法脱节？实际 min=${min} max=${max}）`
  );
  return { min, max };
}

test("值域：poster.postUrl 加密分支的 v 三处对拍（[1, 1]）；alg 的取值域两处文档一致", () => {
  const schemaBranch = schemaJsonEncryptedBranch();
  const schemaV = schemaBranch.properties.v;
  const schemaAlg = schemaBranch.properties.alg;
  assert.ok(schemaV && schemaAlg, "config.schema.json 加密分支缺 v / alg");

  const openapiBranch = extractOpenapiEncryptedBranch();
  assert.ok(
    openapiBranch.has("v") && openapiBranch.has("alg"),
    `openapi 加密分支未抽到 v / alg；实际键：${[...openapiBranch.keys()].sort().join(", ")}`
  );
  const runtimeV = extractRuntimeEncryptedV();

  const three = [
    {
      label: "config.schema.json poster.postUrl.oneOf[1].v",
      min: schemaV.minimum,
      max: schemaV.maximum,
    },
    {
      label: "openapi.yaml PosterSettings.postUrl 加密分支 v",
      min: openapiBranch.get("v").min,
      max: openapiBranch.get("v").max,
    },
    {
      label: "JsonSchemaValidator buildEncryptedStringSchema 的 v",
      min: runtimeV.min,
      max: runtimeV.max,
    },
  ];
  for (const t of three) {
    assert.deepEqual(
      { min: t.min, max: t.max },
      { min: 1, max: 1 },
      `${t.label}: 应为 [1, 1]（v 是「当前仅此一版」的版本号；任一处漏改或提取失效即须在此红）`
    );
  }
  // 三处必须**同值**（与主用例同一判据；任一处单边改动即在此红）。
  assert.deepEqual(
    three.map((t) => [t.min, t.max]),
    Array(three.length).fill([1, 1]),
    "poster.postUrl 加密分支 v 的三处值域不一致"
  );

  // alg：两份**文档**必须给出同一个 enum（运行时 schema 无 enum，故只登记不对称，不在此假造一致）。
  assert.deepEqual(
    openapiBranch.get("alg").enum,
    ["AES-256-GCM"],
    "openapi.yaml 加密分支 alg 的 enum 应为 [AES-256-GCM]"
  );
  assert.deepEqual(
    schemaAlg.enum,
    openapiBranch.get("alg").enum,
    "config.schema.json 与 openapi.yaml 的 alg enum 不一致"
  );
});



