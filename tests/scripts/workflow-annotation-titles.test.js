// FACE-CI-003 守护：.github/workflows/ci.yml 的注解 title 正确性。
//
// 为什么需要它：注解（`::warning title=...::`）是「只读 PR Annotations 面板的人」唯一能看到的
// 分桶信号，而它有两条因果链都无自动化守护：
//   ① 生产者侧——title 写错/写漏/分桶被合并，只改文案不改任何 step 的成败语义，CI 依旧全绿；
//   ② 消费端——`steps.check_native_lib_compile.outputs.{skipped,skipped_reason}` 与
//      `steps.check_native_lib_compile.outcome`（注意 outcome **不在** `outputs` 下，见 ci.yml 中
//      `NL_OUTCOME: ${{ steps.check_native_lib_compile.outcome }}`）是 title 分桶的下游依赖
//      （跳过信息同时被 Step Summary 消费），改了却无人拦。
// 实证：改造前全仓 `grep -rn "title=" tests/` 返回空 ⇒ 存在「静默破坏」通道。
//
// 范式来源：本仓既有的「解析工作流 YAML 并断言结构」写法是 scripts/check-workflow-pins.js
// （导出纯函数 + 行级文本扫描，刻意不依赖任何 YAML 库）+ tests/scripts/check-workflow-pins.test.js
// （纯函数 fixture → 真实 ci.yml 集成 → 断言退出码三层）。本测试照此办理，同样**不引入
// js-yaml**（全仓 tests/scripts 与 scripts 均无 YAML 解析器，js-yaml 只作为 web/package.json
// 的 pnpm overrides 存在）。
//
// ⚠️ 与 check-workflow-pins.js 的关键差别：它有「跳过 run: | 块标量内容」的逻辑，本测试**不能**
//    照抄 —— 我们要找的注解本身就在 `run:` 块里的 echo/Write-Host 行内，必须直接在全文匹配。
//
// 与规格的一处偏离（如实登记）：任务给的纯函数规格是「逐行匹配
// `^::(notice|warning|error)( [^:]*)::(.*)$`」。但 ci.yml 里三处 title 只有两处是这种字面形态
// （`::warning title=native-lib-compile-failed::…` 与 `::warning title=rk3288_cli build failed::…`），
// 第三处是模板形态（`echo "::${level} title=${title}::…"`，title 由 shell 的 `local title="默认"`
// 与 `if [ "$level" = "warning" ]; then title="…"; fi` 共同决定）。
// 纯字面扫描只能抽到 2 条 title，无法满足「title 全集 == 4 条白名单」与「条数 == 白名单基数」。
// 故 scanAnnotations 同时解析两类来源：字面注解行 + 模板注解行（后者按同一 shell 函数体内的
// `<lv>="$1"` / `local <t>="默认"` / `if [ "$<lv>" = "X" ]; then <t>="Y"; fi` 与调用点
// 实参做小型符号求值，得到 (level, title) 对）。同时注解不是在行首（在 echo/Write-Host 引号内），
// 故正则不锚 `^`，而是要求 `::` 前一个字符不是 `:`。
//
// ⚠️ 本文件一律**不写死 ci.yml 的行号**（只在运行时错误信息里输出实际行号）：行号引用本身就是
//    本项目反复出现的陈旧源（改注释即漂移），与本测试要解决的问题同源。
//
// 离线、无网络、无 CI 环境依赖：只读仓内文件 + node: 内置模块。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const CI_YML = path.join(REPO_ROOT, ".github", "workflows", "ci.yml");

/**
 * title 白名单（显式常量表）。ci.yml 里出现的每一个注解 title 都必须登记在此：
 *   - env-transient            —— 瞬时抖动（apt 网络等），notice 级；
 *   - env-not-ready            —— 布局级/配置级环境未就绪，warning 级；
 *   - native-lib-compile-failed —— JNI 源文件（cmake/android.cmake 的全 8 个）语法检查真实失败（编译期错误），warning 级；
 *   - rk3288_cli build failed  —— rk3288_cli 未编过，warning 级。
 * 改动任一 title、或新增第 5 个 title，都必须在此显式登记 —— 这正是本测试要拦的事。
 */
const TITLE_WHITELIST = [
  "env-not-ready",
  "env-transient",
  "native-lib-compile-failed",
  "rk3288_cli build failed",
];

/**
 * title → 严重度绑定关系。env-not-ready/env-transient 的分桶**就是** warning/notice 的分级
 * （见 ci.yml 中 skip_env_not_ready 内的 title 分桶实现）：改错这一对，Annotations 面板的
 * 标题就会与严重度自相矛盾。
 */
const LEVEL_BY_TITLE = {
  "env-not-ready": "warning",
  "env-transient": "notice",
  "native-lib-compile-failed": "warning",
  "rk3288_cli build failed": "warning",
};

/**
 * 消费端接线：这四个 key 必须**在非注释行上**同时存在，才算 title 分桶的下游依赖没被拆掉。
 *
 * ⚠️ 判据强度（如实，本批订正）：旧写法是 `src.includes(needle)` 的**子串存在性**，有两处假绿——
 *   · `id: check_native_lib_compile` 也命中**注释**里提到该 id 的那一行（对应 step 内的
 *     注释即提到它）；即 step 的 id 声明被删、只剩注释，旧判据仍绿；
 *   · `…outputs.skipped` 也命中 `…outputs.skipped_reason`（前者是后者的**前缀**）⇒
 *     只保留 skipped_reason 而删掉 skipped，旧判据仍绿。
 * 现在两条防线：① `stripCommentLines` 先删掉整行注释；② 每个输出键的判据要求出现在**同一个
 * 表达式** `${{ steps.check_native_lib_compile.outputs.<key> }}` 内（以 `${{ }}` 框定），
 * 故 `…outputs.skipped_reason }}` 不再命中 `skipped` 的判据。
 */
const CONSUMER_WIRING = [
  {
    key: "id",
    label: "id: check_native_lib_compile（step 自身声明的 id 行）",
    test: (src) => /^\s*id:\s*check_native_lib_compile\s*$/m.test(src),
  },
  {
    key: "skipped",
    label: "steps.check_native_lib_compile.outputs.skipped（跳过标志）",
    test: (src) => /\$\{\{\s*steps\.check_native_lib_compile\.outputs\.skipped\s*\}\}/.test(src),
  },
  {
    key: "skipped_reason",
    label: "steps.check_native_lib_compile.outputs.skipped_reason（跳过原因）",
    test: (src) =>
      /\$\{\{\s*steps\.check_native_lib_compile\.outputs\.skipped_reason\s*\}\}/.test(src),
  },
  {
    key: "outcome",
    label: "steps.check_native_lib_compile.outcome（step 结论；不在 outputs 下）",
    test: (src) => /steps\.check_native_lib_compile\.outcome(?!\w)/.test(src),
  },
];

/**
 * 删掉**整行**注释（行首空白后即 `#`）。YAML 注释与 run 块内的 shell 注释同形，一并删。
 * 只删整行注释：本仓 ci.yml 的注解都写在 `echo`/`Write-Host` 的引号内，行尾的 `#` 可能落在
 * 字符串里，按行尾切分注释会伤及真判据。
 * @param {string} src
 * @returns {string}
 */
function stripCommentLines(src) {
  return String(src)
    .split(/\r?\n/)
    .filter((l) => !/^\s*#/.test(l))
    .join("\n");
}

/** 注解行形态：`<非冒号>::<level><' title=...' 等属性>::<文案>`（属性组内不含 `:`） */
const LITERAL_ANNOTATION_RE = /(?:^|[^:])::(notice|warning|error)((?: [^:]*)?)::/;
/** 模板注解行形态：级别由 shell 变量给出，如 `::${level} title=${title}::...` */
const TEMPLATED_ANNOTATION_RE = /(?:^|[^:])::\$\{(\w+)\}((?: [^:]*)?)::/;
/** 候选行：真的含 `::` 注解起始（字面级别或 `${` 模板）—— 用来排除 `local title="x"` 这类赋值行 */
const ANNOTATION_CANDIDATE_RE = /(?:^|[^:])::(?:notice|warning|error|\$\{)/;

const TITLE_ATTR = "title=";
/** 查找 shell 函数定义行：`name() {` */
const SHELL_FN_DEF_RE = /^\s*([A-Za-z_]\w*)\s*\(\)\s*\{\s*$/;
/** YAML 块标量起始行：`key: |` / `key: >-` / `key: |2` 等（`run:` 脚本即此形态） */
const BLOCK_SCALAR_OPEN_RE = /^\s*[\w.-]+:\s*[|>][-+\d]*\s*$/;

/** 某行的缩进宽度 */
function indentOf(line) {
  return (line.match(/^ */) || [""])[0].length;
}

/**
 * 求函数定义所在的**块标量作用域**（如 `run: |` 的正文区间），用于隔开同名函数。
 *
 * 为什么不只扫函数体：调用点在函数体**之外**（同一 run 脚本里、函数定义之后——ci.yml 的
 * `skip_env_not_ready` 与既有 fixture 都是这个形状），只扫体会把实参集合扫成空集 ⇒ 模板注解
 * 一律解析不出级别。
 * 为什么不再扫全文：ci.yml 若出现**第二个同名**函数，其调用点实参会并进第一个函数的级别集合
 * ⇒ 4 条断言翻红，且报错列出的位置指向**未被改动**的行（红不静默，但会误导排查者）。
 *
 * 判定：自函数定义向上找**最近的、真的包住它**的块标量起始行 —— 候选行缩进必须小于函数定义
 * 缩进，且必须比它与函数定义之间所有非空行的缩进都浅（否则中间已有更浅的行把它截断，例如上一个
 * job 的 `- name:`）。找不到（函数不在任何块标量内）即**回落全文**并置 fallback，保持旧行为。
 * @param {string[]} lines
 * @param {number} fnStart 0-based 函数定义行下标
 * @param {number} fnIndent 函数定义行缩进
 * @returns {{start:number, end:number, fallback:boolean}} 0-based 半开区间 [start, end)
 */
function findEnclosingBlockScope(lines, fnStart, fnIndent) {
  let opener = -1;
  let minBetween = Infinity; // (候选行, fnStart) 之间非空行的最小缩进
  for (let i = fnStart - 1; i >= 0; i -= 1) {
    const line = lines[i];
    if (line.trim() === "") continue;
    const ind = indentOf(line);
    if (ind < fnIndent && BLOCK_SCALAR_OPEN_RE.test(line) && ind < minBetween) {
      opener = i;
      break;
    }
    if (ind < minBetween) minBetween = ind;
  }
  if (opener < 0) return { start: 0, end: lines.length, fallback: true };
  const openerIndent = indentOf(lines[opener]);
  let end = lines.length;
  for (let i = opener + 1; i < lines.length; i += 1) {
    if (lines[i].trim() === "") continue;
    if (indentOf(lines[i]) <= openerIndent) {
      end = i;
      break;
    }
  }
  return { start: opener + 1, end, fallback: false };
}

/**
 * 从注解属性串里取 title 值（原样返回，可能是 `${title}` 模板）。
 * @param {string} params 如 " title=native-lib-compile-failed" / " title=${title}"
 * @returns {string|null}
 */
function extractTitleFromParams(params) {
  const idx = String(params).indexOf(TITLE_ATTR);
  if (idx < 0) return null;
  const raw = String(params).slice(idx + TITLE_ATTR.length).trim();
  return raw === "" ? null : raw;
}

/**
 * `${name}` → "name"；非模板形态返回 null。
 * @param {string} raw
 * @returns {string|null}
 */
function parseVarRef(raw) {
  const m = /^\$\{(\w+)\}$/.exec(String(raw));
  return m ? m[1] : null;
}

/**
 * 解析模板注解所用 title 变量的取值：对注解所在 shell 函数体做小型符号求值。
 * 只认本仓既有写法（不过度通用）：
 *   local <lv>="$1"                                  —— 级别取函数第一实参；
 *   [local] <t>="<默认>"                             —— 该 title 的默认值；
 *   if [ "$<lv>" = "<X>" ]; then <t>="<Y>"; fi       —— 级别 X 覆盖为 Y。
 * 级别全集 = 调用点实参值 ∪ 覆盖条件里的级别（取并集是**失败关闭**取向：无人调用的覆盖分支
 * 也算「登记过的 title」，会被条数/白名单断言拦住，而不是被静默忽略）。
 * @param {string[]} lines
 * @param {number} annotationLine 1-based 注解行号（用于找所在函数）
 * @returns {{fnName:string, levelVar:string, titleVar:string, byLevel:Map<string,string>}}
 */
function resolveShellAnnotationTitles(lines, annotationLine, expected) {
  const { levelVar, titleVar } = expected;
  let fnStart = -1;
  let fnName = null;
  let fnIndent = -1;
  for (let i = Math.min(annotationLine - 1, lines.length - 1); i >= 0; i -= 1) {
    const m = SHELL_FN_DEF_RE.exec(lines[i]);
    if (m) {
      fnStart = i;
      fnName = m[1];
      fnIndent = indentOf(lines[i]);
      break;
    }
  }
  if (fnStart < 0) {
    throw new Error(`ci.yml:${annotationLine} 的模板注解不在任何 shell 函数内，无法解析 title 变量`);
  }
  // 函数体末尾 = 其后首个「独占 `}` 且缩进不深于定义行」的行
  let fnEnd = lines.length - 1;
  for (let i = fnStart + 1; i < lines.length; i += 1) {
    if (lines[i].trim() === "}" && indentOf(lines[i]) <= fnIndent) {
      fnEnd = i;
      break;
    }
  }
  const body = lines.slice(fnStart + 1, fnEnd);

  // 注解里的 `${level}` 必须就是该函数里绑到第一实参的变量 —— 否则级别取值范围无从确定，
  // 覆盖分支会被静默跳过（两个级别落到同一个 title），此处显式报错给出确切原因。
  if (!body.some((l) => new RegExp(`\\b${levelVar}="\\$1"`).test(l))) {
    throw new Error(
      `ci.yml:${fnName} 内未找到 ${levelVar}="$1" —— 模板注解的级别变量未绑定到函数第一实参，无法解析`
    );
  }

  const defaultRe = new RegExp(`\\b${titleVar}="([^"]*)"`);
  const overrideRe = new RegExp(
    `if\\s+\\[\\s*"\\$${levelVar}"\\s*=\\s*"([^"]+)"\\s*\\]\\s*;\\s*then\\s+${titleVar}="([^"]+)"`
  );
  let defaultTitle = null;
  const overrides = new Map();
  for (const line of body) {
    const ov = overrideRe.exec(line);
    if (ov) {
      overrides.set(ov[1], ov[2]);
      continue; // 覆盖分支里的赋值不是默认值（顺序敏感：先判覆盖、再取默认）
    }
    const def = defaultRe.exec(line);
    if (def && defaultTitle === null) defaultTitle = def[1];
  }

  // 调用点实参：`<fnName> "<level>" "<reason>"`。
  // ⚠️ 射程 = 函数定义所在的**块标量作用域**，不是全文：全文扫描会让**同名**函数的调用点串味
  //    （见 findEnclosingBlockScope 的说明与对应回归用例）。
  const scope = findEnclosingBlockScope(lines, fnStart, fnIndent);
  const callRe = new RegExp(`^\\s*${fnName}\\s+"([^"]+)"`);
  const callLevels = new Set();
  for (const line of lines.slice(scope.start, scope.end)) {
    const m = callRe.exec(line);
    if (m) callLevels.add(m[1]);
  }

  const allLevels = new Set([...callLevels, ...overrides.keys()]);
  if (allLevels.size === 0) {
    throw new Error(
      `ci.yml:${fnName} 未找到任何调用点，无法确定注解级别的取值范围` +
        `（射程＝${scope.fallback ? "全文（未定位到所在块标量，已回落）" : `所在块标量第 ${scope.start + 1}-${scope.end} 行`}）`
    );
  }
  const byLevel = new Map();
  for (const lv of allLevels) {
    const t = overrides.has(lv) ? overrides.get(lv) : defaultTitle;
    if (t === null) {
      throw new Error(
        `ci.yml:${fnName} 的级别 "${lv}" 既无 ${titleVar}="..." 覆盖、也无默认 ${titleVar}="..."，无法解析 title`
      );
    }
    byLevel.set(lv, t);
  }
  return { fnName, levelVar, titleVar, byLevel };
}

/**
 * 扫描 workflow 源文本，抽出所有带 title 的注解条目。
 * 只统计**带 title 的注解**：`::notice::` 这类无 title 的注解、以及注释里顺带提到的注解字面量，
 * 都不属于「title 台账」射程（它们没有 title 可登记）。
 * @param {string} src
 * @returns {Array<{line:number, level:string, title:string, kind:"literal"|"templated"}>}
 */
function scanAnnotations(src) {
  const lines = String(src).split(/\r?\n/);
  const entries = [];
  for (let i = 0; i < lines.length; i += 1) {
    const line = lines[i];
    if (!line.includes(TITLE_ATTR)) continue; // 无 title 的注解不进台账
    if (!ANNOTATION_CANDIDATE_RE.test(line)) continue; // 排除 `local title="x"` 之类的赋值行

    const lit = LITERAL_ANNOTATION_RE.exec(line);
    if (lit) {
      const title = extractTitleFromParams(lit[2]);
      if (title === null) continue; // `::warning::` 形态：有 :: 无 title
      entries.push({ line: i + 1, level: lit[1], title, kind: "literal" });
      continue;
    }

    const tpl = TEMPLATED_ANNOTATION_RE.exec(line);
    if (tpl) {
      const titleRaw = extractTitleFromParams(tpl[2]);
      const titleVar = parseVarRef(titleRaw);
      if (titleVar === null) {
        throw new Error(
          `ci.yml:${i + 1} 模板注解的 title 不是 \${var} 形态（无法解析）：${line.trim()}`
        );
      }
      const res = resolveShellAnnotationTitles(lines, i + 1, { levelVar: tpl[1], titleVar });
      for (const [level, title] of res.byLevel) {
        entries.push({ line: i + 1, level, title, kind: "templated" });
      }
      continue;
    }

    // 既含 title= 又含 :: 注解起始，却不符合任何一种注解形态 —— 静默放过会掩盖正则失效
    throw new Error(
      `ci.yml:${i + 1} 含 title= 与 :: 注解起始，却不匹配 ::<level> title=<t>:: 契约：${line.trim()}`
    );
  }
  return entries;
}

const sortTitles = (arr) => [...arr].sort();

// ---------- 纯函数单元测试（fixture 注入，不依赖仓库状态） ----------

test("FACE-CI-003：scanAnnotations 抽取字面注解的 (level, title)（无 title 的注解与散文里的注解字面量不计）", () => {
  const src = [
    "      - name: demo",
    "        run: |",
    '          echo "::warning title=alpha-warn::body"',
    "          echo '::notice title=beta-note::body'",
    "          # 散文：这里提到 title= 与 ::warning:: 都不是注解",
    '          Write-Host "::warning title=gamma has space::body"',
  ].join("\n");
  const entries = scanAnnotations(src);
  assert.equal(entries.length, 3, `应抽到 3 条带 title 的注解，实际 ${entries.length}`);
  assert.deepEqual(
    entries.map((e) => [e.line, e.level, e.title]),
    [
      [3, "warning", "alpha-warn"],
      [4, "notice", "beta-note"],
      [6, "warning", "gamma has space"],
    ],
    "行号 / 级别 / title 必须逐条精确"
  );
});

test("FACE-CI-003：scanAnnotations 解析模板注解（title 由 shell 变量按级别分桶）", () => {
  const src = [
    "        run: |",
    "          skip_it() {",
    '            local level="$1" reason="$2"',
    '            local title="t-default"',
    '            if [ "$level" = "warning" ]; then title="t-warn"; fi',
    '            echo "::${level} title=${title}::body"',
    "            exit 0",
    "          }",
    '          skip_it "notice" "r1"',
    '          skip_it "warning" "r2"',
  ].join("\n");
  const entries = scanAnnotations(src);
  assert.deepEqual(
    [...entries].sort((a, b) => a.title.localeCompare(b.title)).map((e) => [e.level, e.title]),
    [
      ["notice", "t-default"],
      ["warning", "t-warn"],
    ],
    "模板注解应按 shell 变量解析出 (级别 → title) 绑定"
  );
  assert.ok(entries.every((e) => e.kind === "templated" && e.line === 6), "模板条目应归属发出注解的那一行");
});

test("FACE-CI-003：含 title= 却不符合注解契约的行必须报错（防正则失效后静默通过）", () => {
  const src = '          echo "::warning title=oops-no-terminator"';
  assert.throws(() => scanAnnotations(src), /不匹配/, "缺 :: 终结符的 title 行必须抛错而非被静默忽略");
});

test("FACE-CI-003：同名函数在不同 run 块中的调用点不互相串味（旧全文扫描的实测串扰，已修）", () => {
  const src = [
    "        run: |", // 1
    "          skip_it() {", // 2
    '            local level="$1" reason="$2"', // 3
    '            local title="t-default"', // 4
    '            if [ "$level" = "warning" ]; then title="t-warn"; fi', // 5
    '            echo "::${level} title=${title}::body"', // 6
    "            exit 0", // 7
    "          }", // 8
    '          skip_it "notice" "r1"', // 9
    "      - name: 另一个 job 内的同名函数", // 10
    "        run: |", // 11
    "          skip_it() {", // 12
    '            local level="$1" reason="$2"', // 13
    '            local title="t2-default"', // 14
    '            if [ "$level" = "warning" ]; then title="t2-warn"; fi', // 15
    '            echo "::${level} title=${title}::body"', // 16
    "            exit 0", // 17
    "          }", // 18
    '          skip_it "error" "r2"', // 19
  ].join("\n");
  const entries = scanAnnotations(src);
  assert.deepEqual(
    [...entries].sort((a, b) => a.title.localeCompare(b.title)).map((e) => [e.line, e.level, e.title]),
    [
      [6, "notice", "t-default"],
      [6, "warning", "t-warn"],
      [16, "error", "t2-default"],
      [16, "warning", "t2-warn"],
    ],
    "每个模板注解只应取**自己所在 run 块**内的调用点实参 —— 旧写法按函数名扫全文，会把第二个函数的 " +
      '"error" 并入第一个函数的级别集合（多出 [6,"error",…] 等条目）'
  );
  assert.equal(
    entries.filter((e) => e.line === 6).some((e) => e.level === "error"),
    false,
    "第二个函数独有的级别取值（error）不应并进第一个函数（第 6 行注解）的解析结果"
  );
});

// ---------- 已实测的边界（如实登记，不修） ----------
//
// 本测试是**基于文本/正则的静态断言**，不是 YAML/shell 语义解析器。以下两种改法会让它报错，
// 但它们**未必是真实缺陷**。两条均以「把 ci.yml 按该改法改写后实跑本测试」的方式**实测过**
// （基线副本 9 pass / 0 fail；下列各变体 9 tests / 6 pass / **3 fail**）：
//
// ① 改写模板注解所在 shell 变量的那两行（`local <t>="默认"` 与
//    `if [ "$<lv>" = "warning" ]; then <t>="…"; fi`）——例如把两行合并为一行等价 shell、或把
//    双引号改单引号 ⇒ 符号求值解析不到变量绑定、scanAnnotations 直接抛错
//    （实测信息：`ci.yml:skip_env_not_ready 的级别 "notice" 既无 title="…" 覆盖、也无默认 title="…"`）
//    ⇒ **3 条断言变红**：title 全集 / 严重度绑定 / 抽出的注解条数。
//    判据形态见 resolveShellAnnotationTitles；模板注解的物理形态是它的**隐式前置条件**。
// ② 在任意位置加一行**引用了完整注解字面量**的注释（如 `# 举例：::warning title=xxx::`）
//    ⇒ scanAnnotations 按「行内含 `::<level> … title=…::`」计数，多出 1 条 ⇒ 同样是**3 条断言
//    变红**（title 全集：多出的 title 不在白名单；严重度绑定：白名单外 title 无预期级别；
//    条数：5 ≠ 4）。这是**有意**的取舍：本文件头部已说明「不能照抄 check-workflow-pins.js
//    跳过 run 块内容」，而注解就写在 run 块内的 echo 里；若为容纳注释而去剥注释，会同时放过
//    run 块里被注释掉的 `echo "::…"`（那才是真该报的形态）。故按「宁可误红、不可漏报」保留。
//
// 另有第三条曾经的误红已**修掉**而非登记：级别取值的**同名函数串扰**（旧写法按函数名全文扫描
// 调用点）——ci.yml 一旦出现第二个同名函数，其调用点实参会并进第一个函数的级别集合，使该注解
// 解析出多余的 (级别, title) 条目（进而令 title 全集 / 严重度绑定 / 条数中的若干条变红），且报错
// 位置指向**未被改动**的行。现按「函数定义所在的 run 块」限定射程。**实测**：本文件新增的回归
// 用例在旧写法下变红、新写法下通过（旧写法 9 tests / 8 pass / 1 fail，红的正是该用例；新写法
// 9 tests / 9 pass）。回归用例见上方「同名函数在不同 run 块中的调用点不互相串味」。

// ---------- 真实 ci.yml 集成断言 ----------

test("FACE-CI-003：ci.yml 的 title 全集 == 显式白名单（改错/改漏任一 title 立即红）", () => {
  const src = fs.readFileSync(CI_YML, "utf8");
  const entries = scanAnnotations(src);
  const seen = sortTitles(new Set(entries.map((e) => e.title)));
  assert.deepEqual(
    seen,
    sortTitles(TITLE_WHITELIST),
    `ci.yml 的注解 title 集合与白名单不一致（新增/改名 title 必须同步登记 TITLE_WHITELIST 并复核消费端）\n` +
      `  实际：${JSON.stringify(seen)}\n  白名单：${JSON.stringify(sortTitles(TITLE_WHITELIST))}\n` +
      `  位置：\n${entries.map((e) => `    L${e.line} ${e.level} ${e.title}`).join("\n")}`
  );
});

test("FACE-CI-003：title 与严重度的绑定关系（env-not-ready=warning / env-transient=notice）", () => {
  const src = fs.readFileSync(CI_YML, "utf8");
  for (const e of scanAnnotations(src)) {
    const expected = LEVEL_BY_TITLE[e.title];
    assert.ok(expected, `未登记严重度的 title：${e.title}（L${e.line}）`);
    assert.equal(
      e.level,
      expected,
      `L${e.line} 的 title=${e.title} 与严重度 ${e.level} 不匹配（应为 ${expected}）——` +
        `Annotations 面板的标题分桶会与严重度自相矛盾`
    );
  }
});

test("FACE-CI-003：消费端接线完整（skipped / skipped_reason / outcome 三者均被读取）", () => {
  const src = stripCommentLines(fs.readFileSync(CI_YML, "utf8"));
  for (const { key, label, test: matches } of CONSUMER_WIRING) {
    assert.ok(
      matches(src),
      `ci.yml 缺少「${label}」（key=${key}）—— title 分桶的下游消费链被拆掉` +
        `（分桶信息只剩显示价值，跳过状态无人统计）。判据：剥掉整行注释后按精确表达式匹配，` +
        `既排除「只在注释里提到」，也排除「skipped 被 skipped_reason 前缀命中」。`
    );
  }
});

test("FACE-CI-003：消费端判据不再被「注释提及」与「前缀命中」骗过（旧子串写法的两处假绿）", () => {
  const byKey = (key) => CONSUMER_WIRING.find((w) => w.key === key);
  // ① 只在注释里提到 id ≠ 真的声明了 id（旧 src.includes 写法会假绿）
  assert.equal(
    byKey("id").test("          # 说明：本 step 声明 id: check_native_lib_compile"),
    false,
    "注释行里的 `id: …` 不应算作消费端接线（旧子串判据在此假绿）"
  );
  assert.equal(
    byKey("id").test("        id: check_native_lib_compile"),
    true,
    "真正的 id 声明行应命中"
  );
  // ② skipped 不应被 skipped_reason 前缀命中（旧 src.includes 写法会假绿）
  assert.equal(
    byKey("skipped").test("          NL_S: ${{ steps.check_native_lib_compile.outputs.skipped_reason }}"),
    false,
    "`…outputs.skipped` 不应命中 `…outputs.skipped_reason`（旧子串判据在此假绿）"
  );
  assert.equal(
    byKey("skipped").test("          NL_S: ${{ steps.check_native_lib_compile.outputs.skipped }}"),
    true,
    "精确表达式应命中"
  );
  assert.equal(
    byKey("skipped_reason").test("          NL_R: ${{ steps.check_native_lib_compile.outputs.skipped_reason }}"),
    true,
    "skipped_reason 的精确表达式应命中"
  );
});

test("FACE-CI-003：抽出的注解条数 == 白名单基数（防正则失效造成「空 == 空」通过）", () => {
  const src = fs.readFileSync(CI_YML, "utf8");
  const entries = scanAnnotations(src);
  assert.equal(
    entries.length,
    TITLE_WHITELIST.length,
    `ci.yml 实际抽出 ${entries.length} 条带 title 的注解，白名单基数为 ${TITLE_WHITELIST.length} —— ` +
      `条数不符（生产端增删注解，或扫描正则与实际写法脱节）\n` +
      entries.map((e) => `    L${e.line} ${e.level} ${e.title}`).join("\n")
  );
  assert.equal(
    new Set(entries.map((e) => e.title)).size,
    entries.length,
    "同一 title 重复出现：title 是分桶标识，重复会让「按标题分桶」失去意义"
  );
  assert.ok(
    entries.some((e) => e.kind === "templated"),
    "未抽到任何模板注解 —— shell 变量路径的解析逻辑已成死代码（或该注解被改写）"
  );
});
