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
