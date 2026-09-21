// ζ 组发现（Low）的处置：把「两处模型文件表需人工同步」这一维护隐患变成会红的断言。
//
// 背景：同一份「profile → 4 个 ncnn 模型源文件」的表被写了两遍——
//   · app/build.gradle 的 faceModelFiles（Gradle 的 verifyFaceModels 与 stageFaceModels 共用，单一来源）
//   · scripts/verify_face_models.py 的 PROFILES（独立命令行工具）
// 两处各自的注释都承认「同步义务」，但**没有任何机制**能发现漂移 ⇒ 增删模型时漏改一处会静默发生。
//
// 为什么不按原计划「让 Gradle 调 Python + 接 CI」：
//   · app/build.gradle:181-183 已记录该取向被否（Windows cmd 下 PATH 常缺 python、外部 exec 曾悬挂）；
//   · 该脚本在模型 blob 缺失时 exit 1，而 blob 被 .gitignore 排除 ⇒ CI 里必然缺失 ⇒ 直接接入会让 CI 变红；
//     若为此加「缺文件即跳过」，则该步在 CI 里恒为 no-op —— 正是本仓明确警惕的「静默通过」反模式。
//   ⇒ 故改为**检测漂移**而非消除重复：保留两处实现，但让不一致立刻报警。
//
// 另钉住 Groovy 侧「按键名由文件名派生」这一启发式的**前提**：
// verifyFaceModels 以「含 scrfd → detector，否则 embedder」派生 manifest 键（app/build.gradle:203-204）。
// 若将来检测模型改名后不再含 scrfd，两条目会双双派生成 embedder ⇒ 键冲突/错配，且**不会报错**。
// 故这里显式断言每个 profile 恰有一对含 scrfd 的 param/bin。
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");

const REPO_ROOT = path.resolve(__dirname, "..", "..");
const GRADLE = path.join(REPO_ROOT, "app", "build.gradle");
const PY = path.join(REPO_ROOT, "scripts", "verify_face_models.py");

function readFile(p) {
  assert.ok(fs.existsSync(p), `缺少文件: ${p}`);
  return fs.readFileSync(p, "utf8");
}

// 解析 app/build.gradle 的 faceModelFiles → Map<profile, string[]>
function parseGradleTable() {
  const src = readFile(GRADLE);
  const start = src.indexOf("def faceModelFiles = [");
  assert.ok(start >= 0, "app/build.gradle 中未找到 `def faceModelFiles = [`（改名？本守卫需同步更新）");
  const rest = src.slice(start);
  const end = rest.indexOf("\n]");
  assert.ok(end > 0, "faceModelFiles 块的顶层 `]` 未找到（缩进/排版变化？）");
  const block = rest.slice(0, end);

  const table = new Map();
  const re = /'([A-Za-z0-9_]+)'\s*:\s*\[([\s\S]*?)\]/g;
  let m;
  while ((m = re.exec(block)) !== null) {
    const paths = [...m[2].matchAll(/'(models\/[^']+)'/g)].map((x) => x[1]);
    table.set(m[1], paths);
  }
  assert.ok(table.size > 0, "faceModelFiles 未解析出任何 profile（解析器与文件格式已脱节）");
  // ⚠️ 上一条**单独兜不住**「边界失配导致只抽到部分条目」——`size=1 > 0` 照样通过。
  // 故另加一条**直接**的兜底：文件里 `'models/...'` 字面量总数必须等于解析出的路径条数。
  // 它与 profile 数无关（不钉"必须 2 个 profile"，故将来合法新增 profile 不会误红），
  // 但任何漏抽都会立刻红。
  const parsedPaths = [...table.values()].reduce((n, arr) => n + arr.length, 0);
  const literals = [...src.matchAll(/'models\//g)].length;
  assert.equal(
    parsedPaths,
    literals,
    `faceModelFiles 解析出 ${parsedPaths} 条路径，但 app/build.gradle 里有 ${literals} 个 ` +
      `'models/...' 字面量 —— 解析器漏抽了条目（这种静默失败不会被 table.size>0 兜住）`
  );
  return table;
}

// 解析 scripts/verify_face_models.py 的 PROFILES → Map<profile, string[]>
function parsePythonTable() {
  const src = readFile(PY);
  const start = src.indexOf("PROFILES = {");
  assert.ok(start >= 0, "verify_face_models.py 中未找到 `PROFILES = {`");
  const rest = src.slice(start);
  const end = rest.indexOf("\n}");
  assert.ok(end > 0, "PROFILES 块的顶层 `}` 未找到");
  const block = rest.slice(0, end);

  const table = new Map();
  const re = /"([A-Za-z0-9_]+)"\s*:\s*\{([\s\S]*?)\}/g;
  let m;
  while ((m = re.exec(block)) !== null) {
    const paths = [...m[2].matchAll(/"([^"]+)"/g)]
      .map((x) => x[1])
      .filter((s) => s.startsWith("models/"));
    table.set(m[1], paths);
  }
  assert.ok(table.size > 0, "PROFILES 未解析出任何 profile（解析器与文件格式已脱节）");
  // 同 parseGradleTable：`size>0` 兜不住漏抽，故直接钉「字面量总数 == 解析条数」。
  const parsedPaths = [...table.values()].reduce((n, arr) => n + arr.length, 0);
  const literals = [...src.matchAll(/"models\//g)].length;
  assert.equal(
    parsedPaths,
    literals,
    `PROFILES 解析出 ${parsedPaths} 条路径，但 scripts/verify_face_models.py 里有 ${literals} 个 ` +
      `"models/..." 字面量 —— 解析器漏抽了条目`
  );
  return table;
}

test("ζ：两处模型文件表的 profile 集一致", () => {
  const gradle = parseGradleTable();
  const python = parsePythonTable();
  assert.deepEqual(
    [...gradle.keys()].sort(),
    [...python.keys()].sort(),
    "app/build.gradle 的 faceModelFiles 与 verify_face_models.py 的 PROFILES 的 profile 集不一致"
  );
});

test("ζ：每个 profile 的模型源路径集两处完全一致", () => {
  const gradle = parseGradleTable();
  const python = parsePythonTable();
  for (const name of gradle.keys()) {
    assert.ok(python.has(name), `PROFILES 缺少 profile: ${name}`);
    assert.deepEqual(
      gradle.get(name).slice().sort(),
      python.get(name).slice().sort(),
      `profile ${name} 的模型源路径在 Gradle 与 Python 两表中不一致 —— 增删模型后漏改了一处`
    );
  }
});

test("ζ：每个 profile 恰为 2 个 .param + 2 个 .bin，且恰有一对含 scrfd 的检测模型", () => {
  const stem = (p) => path.basename(p).replace(/\.(param|bin)$/, "");
  for (const [name, paths] of parseGradleTable()) {
    assert.equal(paths.length, 4, `profile ${name} 应有 4 个模型文件，实际 ${paths.length}`);
    assert.equal(
      paths.filter((p) => p.endsWith(".param")).length,
      2,
      `profile ${name} 应有 2 个 .param（stage 与引脚派生依赖 param/bin 成对）`
    );
    assert.equal(
      paths.filter((p) => p.endsWith(".bin")).length,
      2,
      `profile ${name} 应有 2 个 .bin`
    );
    const stems = new Set(paths.map(stem));
    assert.equal(
      stems.size,
      2,
      `profile ${name} 的 param/bin 未成对：文件名主干应恰有 2 个，实际 ${JSON.stringify([...stems])}`
    );
    assert.equal(
      [...stems].filter((s) => s.includes("scrfd")).length,
      1,
      `profile ${name} 中「含 scrfd」的模型主干应恰有 1 个。verifyFaceModels 依赖该启发式派生 manifest` +
        ` 键（含 scrfd → detector，否则 embedder；实现见 app/build.gradle:206-209 的 pinKeyOf，` +
        `其说明在 :203-204）；若检测模型改名后不再含 scrfd，两条目会双双派生成 embedder 且**不会报错**。` +
        ` 实际主干：${JSON.stringify([...stems])}`
    );
  }
});
