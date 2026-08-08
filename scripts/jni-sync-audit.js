/**
 * @file jni-sync-audit.js
 * @brief JNI 签名一致性核对脚本（Android 子系统审计）
 *
 * 核对 Java 侧 native 声明与 C++ 侧 JNI 注册表是否一致：
 *   1. C++ 侧：扫描 src/cpp/native-lib.cpp + src/cpp/jni/** 的注册表
 *      （JniMethodRegistry::add({ {name, sig, fn}, ... }) 模式；兼容
 *      { "Java_pkg_Class_method" } 静态链接符号仅计数），
 *      并解析 registry.cpp 的领域→Java 类映射；
 *   2. Java 侧：扫描 src/java/com/example/rk3288_opencv/ 的 native 声明，
 *      把 Java 方法签名转换为 JNI 签名描述符；
 *   3. 按类逐条比对数量与签名，输出 ALL_MATCH 或差异清单。
 * 当存在差异时以非零退出码退出（供 CI 门禁）。
 *
 * 仅使用 Node 标准库，无第三方依赖。
 */
"use strict";

const fs = require("fs");
const fsp = require("fs/promises");
const path = require("path");

function toPosix(p) {
  return p.replace(/\\/g, "/");
}

function nowIsoLocal() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  return (
    `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}` +
    `T${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
  );
}

function printHelp() {
  process.stdout.write(
    "用法: node scripts/jni-sync-audit.js [选项]\n" +
    "核对 C++ JNI 注册表与 Java native 声明的签名一致性。\n" +
    "  --repo-root <dir>     仓库根目录（默认脚本上级目录）\n" +
    "  --java-dir <dir>      Java 源码目录（默认 src/java/com/example/rk3288_opencv）\n" +
    "  --cpp-native-lib <f>  JNI 入口文件（默认 src/cpp/native-lib.cpp）\n" +
    "  --cpp-jni-dir <dir>   JNI 领域源目录（默认 src/cpp/jni）\n" +
    "  --registry-file <f>   注册中心文件（默认 src/cpp/jni/registry.cpp）\n" +
    "  --out-dir <dir>       可选：额外输出 JSON 报告到该目录\n" +
    "  --verbose             打印逐类明细\n" +
    "  --help / -h           显示帮助\n" +
    "退出码：0=全部一致，2=存在差异，1=执行出错\n"
  );
}

function parseArgs(argv) {
  const args = {
    repoRoot: "",
    javaDir: "src/java/com/example/rk3288_opencv",
    cppNativeLib: "src/cpp/native-lib.cpp",
    cppJniDir: "src/cpp/jni",
    registryFile: "src/cpp/jni/registry.cpp",
    outDir: "",
    verbose: false,
  };
  for (let i = 2; i < argv.length; i += 1) {
    const a = argv[i];
    if ((a === "--repo-root") && argv[i + 1]) {
      args.repoRoot = argv[i + 1];
      i += 1;
    } else if ((a === "--java-dir") && argv[i + 1]) {
      args.javaDir = argv[i + 1];
      i += 1;
    } else if ((a === "--cpp-native-lib") && argv[i + 1]) {
      args.cppNativeLib = argv[i + 1];
      i += 1;
    } else if ((a === "--cpp-jni-dir") && argv[i + 1]) {
      args.cppJniDir = argv[i + 1];
      i += 1;
    } else if ((a === "--registry-file") && argv[i + 1]) {
      args.registryFile = argv[i + 1];
      i += 1;
    } else if ((a === "--out-dir") && argv[i + 1]) {
      args.outDir = argv[i + 1];
      i += 1;
    } else if (a === "--verbose") {
      args.verbose = true;
    } else if (a === "--help" || a === "-h") {
      printHelp();
      process.exit(0);
    }
  }
  return args;
}

// ---------- 通用：去除注释（保留行号，供行号定位） ----------
function stripComments(src) {
  let s = String(src);
  // 块注释：非换行字符全部替换为空格，保住换行位置
  s = s.replace(/\/\*[\s\S]*?\*\//g, (blk) => blk.replace(/[^\r\n]/g, " "));
  // 行注释：仅清空内容，保留换行
  s = s.replace(/\/\/[^\r\n]*/g, "");
  return s;
}

// ---------- C++ 侧：注册条目提取 ----------
/**
 * 剥离 C++ mangled 重载编号后缀，还原真实方法名。
 * 静态 Java_ 符号若来自 C++ 重载，符号表会带编号段，形如 method__2 / method__3
 * （个别工具为单下划线 method_2）。
 * 规则：
 *  1. 尾随 "__<数字>" 必为编号——Java 标识符经 JNI 转义不会产生连续双下划线，直接剥离；
 *  2. 尾随 "_<数字>" 视为编号剥离（容错：宁可多剥一个数字段，也不把 method__2/method_2
 *     误判成不同方法）；以数字结尾的正常方法名（如 foo2）无下划线分隔，不受影响。
 * @param {string} symbol 原始静态符号（如 Java_pkg_Class_method__2）
 * @returns {string} 剥离编号后的符号名；无编号时原样返回
 */
function stripMangledOverloadSuffix(symbol) {
  let m = symbol.match(/^(.*)__(?:\d+)$/);
  if (m) return m[1];
  m = symbol.match(/^(.*)_(\d+)$/);
  if (m) return m[1];
  return symbol;
}

/**
 * 从 C++ 源中提取 JniMethodRegistry 注册条目。
 * 匹配格式：{ "methodName", "signature", reinterpret_cast<void*>(fn) }
 * 兼容旧式静态链接符号：{ "Java_pkg_Class_method", ... }（无显式签名，仅参与计数/名称核对）。
 * @returns {Array<{name:string, sig:string}>}
 */
function extractRegistryEntries(cppSrc) {
  const src = stripComments(cppSrc);
  const out = [];
  const re = /\{\s*"([A-Za-z_$][\w$]*)"\s*,\s*"([^"]*)"\s*,/g;
  let m;
  while ((m = re.exec(src)) !== null) {
    const name = m[1];
    const sig = m[2].trim();
    // 静态链接符号：方法名取自最后一个 '_' 段；无显式签名（由 JVM 按符号名查找）。
    // 先剥离 C++ mangled 重载编号（method__2 → method），避免编号段被误取为方法名。
    if (name.startsWith("Java_")) {
      const real = stripMangledOverloadSuffix(name);
      const parts = real.split("_");
      out.push({ name: parts[parts.length - 1], sig: "(静态链接，无显式签名)", staticSymbol: name });
    } else {
      out.push({ name, sig });
    }
  }
  return out;
}

/**
 * 解析 registry.cpp，得到 领域(如 Camera) -> Java 类路径 的映射。
 * 流程：JniMethodRegistry 变量 -> register<Domain>Methods(变量) -> 变量.registerAll(env, clazzVar)
 *       -> clazzVar = env->FindClass("类路径")
 * @returns {Map<string, string>} 领域名 -> 类路径（如 com/example/rk3288_opencv/MainActivity）
 */
function parseRegistryClassMap(registrySrc) {
  const src = stripComments(registrySrc);
  const domainToClass = new Map();

  const clazzToPath = new Map(); // clazz 变量 -> 类路径
  let m;
  const clazzRe = /jclass\s+(\w+)\s*=\s*env->FindClass\("([^"]+)"\)\s*;/g;
  while ((m = clazzRe.exec(src)) !== null) clazzToPath.set(m[1], m[2]);

  const varToClazz = new Map(); // registry 变量 -> clazz 变量
  const regRe = /(\w+)\.registerAll\(\s*env\s*,\s*(\w+)\s*\)\s*;/g;
  while ((m = regRe.exec(src)) !== null) varToClazz.set(m[1], m[2]);

  const domainToVar = new Map(); // 领域 -> registry 变量
  const domRe = /register(\w+)Methods\(\s*(\w+)\s*\)\s*;/g;
  while ((m = domRe.exec(src)) !== null) domainToVar.set(m[1], m[2]);

  for (const [domain, varName] of domainToVar) {
    const clazzVar = varToClazz.get(varName);
    const classPath = clazzVar ? clazzToPath.get(clazzVar) : undefined;
    if (classPath) domainToClass.set(domain, classPath);
  }
  return domainToClass;
}

// ---------- Java 侧：native 声明提取与 JNI 描述符转换 ----------
const PRIM_DESC = {
  void: "V",
  boolean: "Z",
  byte: "B",
  char: "C",
  short: "S",
  int: "I",
  long: "J",
  float: "F",
  double: "D",
};

function extractImports(src) {
  const imports = new Map(); // 简单名 -> 全限定名
  const re = /^\s*import\s+(?:static\s+)?([A-Za-z_$][\w$]*(\.[A-Za-z_$][\w$]*)+)\s*;\s*$/gm;
  let m;
  while ((m = re.exec(src)) !== null) {
    const fqn = m[1];
    if (fqn.endsWith(".*")) continue; // 通配导入不逐类解析
    const simple = fqn.slice(fqn.lastIndexOf(".") + 1);
    imports.set(simple, fqn);
  }
  return imports;
}

/** 参数块按顶层逗号切分（避开泛型/数组内的逗号） */
function splitParams(paramBlock) {
  const parts = [];
  const trimmed = paramBlock.trim();
  if (!trimmed) return parts;
  let depth = 0;
  let cur = "";
  for (const ch of trimmed) {
    if (ch === "<" || ch === "(" || ch === "[") depth += 1;
    else if (ch === ">" || ch === ")" || ch === "]") depth = Math.max(0, depth - 1);
    if (ch === "," && depth === 0) {
      parts.push(cur);
      cur = "";
      continue;
    }
    cur += ch;
  }
  if (cur.trim()) parts.push(cur);
  return parts.map(parseParam).filter(Boolean);
}

function parseParam(p) {
  const t = p.trim();
  if (!t) return null;
  const m = t.match(/^([\s\S]+?)\s+([A-Za-z_$][\w$]*)$/);
  if (!m) return { type: t, name: "" };
  return { type: m[1].trim(), name: m[2] };
}

/**
 * 提取 Java 源中的 native 方法声明（支持多行参数）。
 * @returns {Array<{ret:string, name:string, params:Array, line:number}>}
 */
function extractNativeMethods(javaSrc) {
  const clean = stripComments(javaSrc);
  const methods = [];
  const re = /\bnative\s+([A-Za-z_$][\w$]*(?:\s*(?:\[\s*\])|<[\s\S]*?>)*)\s+([A-Za-z_$][\w$]*)\s*\(([\s\S]*?)\)\s*;/g;
  let m;
  while ((m = re.exec(clean)) !== null) {
    const line = clean.slice(0, m.index).split(/\r?\n/).length;
    methods.push({ ret: m[1].trim(), name: m[2], params: splitParams(m[3]), line });
  }
  return methods;
}

/** 类简单名 -> 全限定名（imports > java.lang > 同包） */
function resolveClassFqn(name, imports, pkg) {
  const n = name.trim();
  if (n.includes(".")) {
    // 可能为全限定名或嵌套类引用：用最长 import 前缀匹配
    for (const [simple, fqn] of imports) {
      if (n === simple || n.startsWith(simple + ".")) {
        return fqn + (n.length > simple.length ? n.slice(simple.length) : "");
      }
    }
    return n; // 视为全限定名
  }
  if (imports.has(n)) return imports.get(n);
  if (n === "String") return "java.lang.String";
  return (pkg ? pkg + "." : "") + n;
}

/** 全限定名 -> JNI 对象描述符；嵌套类点转 $（如 Outer.Inner -> Outer$Inner） */
function fqnToDescriptor(fqn) {
  const segs = fqn.split(".");
  // Java 命名约定：类名以大写字母开头，此前为包名。依此切分包名/类链
  let clsStart = segs.findIndex((s) => /^[A-Z]/.test(s));
  if (clsStart < 0) clsStart = segs.length - 1;
  const pkgPart = segs.slice(0, clsStart).join("/");
  const clsPart = segs.slice(clsStart).join("$");
  return "L" + (pkgPart ? pkgPart + "/" : "") + clsPart + ";";
}

/**
 * Java 类型 -> JNI 签名描述符片段。
 * 处理：基本类型、String、类引用（按 imports/包名解析）、数组。
 * @returns {{ok:boolean, desc?:string, reason?:string}}
 */
function resolveTypeToDescriptor(type, imports, pkg) {
  let t = type.trim().replace(/\s+/g, ""); // 规整数组空格（byte [] -> byte[]）
  let arrays = "";
  while (t.endsWith("[]")) {
    arrays += "[";
    t = t.slice(0, -2);
  }
  let desc = null;
  if (Object.prototype.hasOwnProperty.call(PRIM_DESC, t)) {
    desc = PRIM_DESC[t];
  } else if (t === "String") {
    desc = "Ljava/lang/String;";
  } else if (t.includes("<")) {
    return { ok: false, reason: `泛型类型需展开：${type}` };
  } else if (t.includes("...")) {
    return { ok: false, reason: `变长参数需转数组：${type}` };
  } else {
    desc = fqnToDescriptor(resolveClassFqn(t, imports, pkg));
  }
  return { ok: true, desc: arrays + desc };
}

/** native 方法 -> JNI 签名描述符（(参数)返回） */
function buildMethodDescriptor(method, imports, pkg) {
  const parts = [];
  for (const p of method.params) {
    const r = resolveTypeToDescriptor(p.type, imports, pkg);
    if (!r.ok) return { ok: false, reason: `${method.name}: ${r.reason}` };
    parts.push(r.desc);
  }
  const ret = resolveTypeToDescriptor(method.ret, imports, pkg);
  if (!ret.ok) return { ok: false, reason: `${method.name}: ${ret.reason}` };
  return { ok: true, desc: "(" + parts.join("") + ")" + ret.desc };
}

// ---------- 扫描 ----------
async function listCppFiles(repoRoot, args) {
  const files = [];
  const nativeLib = path.resolve(repoRoot, args.cppNativeLib);
  if (fs.existsSync(nativeLib)) {
    files.push({ rel: toPosix(args.cppNativeLib), abs: nativeLib });
  }
  const jniDir = path.resolve(repoRoot, args.cppJniDir);
  if (fs.existsSync(jniDir)) {
    const walk = (dir) => {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        if (entry.isDirectory()) walk(path.join(dir, entry.name));
        else if (entry.isFile() && entry.name.endsWith(".cpp")) {
          const rel = toPosix(path.relative(repoRoot, path.join(dir, entry.name)));
          files.push({ rel, abs: path.join(dir, entry.name) });
        }
      }
    };
    walk(jniDir);
  }
  return files;
}

async function scanCpp(repoRoot, args) {
  const files = await listCppFiles(repoRoot, args);
  const registryAbs = path.resolve(repoRoot, args.registryFile);
  let domainToClass = new Map();
  if (fs.existsSync(registryAbs)) {
    domainToClass = parseRegistryClassMap(await fsp.readFile(registryAbs, "utf8"));
  }
  const entries = [];
  const unmappedByFile = new Map(); // file -> { domain, names: [] }
  for (const f of files) {
    const src = await fsp.readFile(f.abs, "utf8");
    // 领域取自定义函数名：void register<Domain>Methods(JniMethodRegistry& registry) { ... }
    const domMatch = stripComments(src).match(/void\s+register(\w+)Methods\s*\(\s*JniMethodRegistry\s*&\s*\w+\s*\)\s*\{/);
    const domain = domMatch ? domMatch[1] : null;
    const classPath = domain ? domainToClass.get(domain) || null : null;
    for (const e of extractRegistryEntries(src)) {
      entries.push({ name: e.name, sig: e.sig, staticSymbol: e.staticSymbol || null, file: f.rel, domain, classPath });
      // 未映射到 Java 类的注册条目：告警而非静默丢弃（R2-7 移除 nullMap 后恢复安全网）
      if (!classPath) {
        let rec = unmappedByFile.get(f.rel);
        if (!rec) {
          rec = { domain, names: [] };
          unmappedByFile.set(f.rel, rec);
        }
        rec.names.push(e.name);
      }
    }
  }
  const unmappedWarnings = [...unmappedByFile.entries()].map(([file, rec]) => {
    const domainDesc = rec.domain ? `领域 ${rec.domain}` : "未声明 register<Domain>Methods";
    return `[CPP_UNMAPPED] ${file} ${domainDesc} 未映射到 Java 类（registry.cpp 无对应映射），条目 ${rec.names.join("、")} 被跳过核对`;
  });
  // 按类分组：classPath -> Map(name+"\\u0000"+sig -> {name, sig, file, staticSymbol})
  // 复合键（name+sig）避免同名不同签名被静默覆盖；同 name 不同 sig 时记重复警告（潜在重载/冲突）
  const byClass = new Map();
  const dupWarnings = [];
  for (const e of entries) {
    const key = e.classPath; // 未映射到类时为 null
    if (!byClass.has(key)) byClass.set(key, new Map());
    const cm = byClass.get(key);
    const ck = `${e.name}\\u0000${e.sig}`;
    if (cm.has(ck)) continue; // 完全重复（同 name 同 sig）仅保留首个
    const dupKey = [...cm.keys()].find((k) => k.startsWith(e.name + "\\u0000"));
    if (dupKey) {
      dupWarnings.push(
        `[CPP_DUP] class=${key} name=${e.name} 存在多个签名（${dupKey.split("\\u0000")[1]} 与 ${e.sig}），疑似重载/冲突，请人工核对`
      );
    }
    cm.set(ck, { name: e.name, sig: e.sig, file: e.file, staticSymbol: e.staticSymbol });
  }
  return { byClass, entries, warnings: [...dupWarnings, ...unmappedWarnings] };
}

async function scanJava(repoRoot, args) {
  const dir = path.resolve(repoRoot, args.javaDir);
  const byClass = new Map();
  const files = [];
  const dupWarnings = [];
  if (!fs.existsSync(dir)) return { byClass, files, warnings: dupWarnings };
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (!entry.isFile() || !entry.name.endsWith(".java")) continue;
    const abs = path.join(dir, entry.name);
    const rel = toPosix(path.join(args.javaDir, entry.name));
    const src = await fsp.readFile(abs, "utf8");
    const pkg = (src.match(/^\s*package\s+([\w.]+)\s*;\s*$/m) || [])[1] || "";
    const cls = (src.match(/\bclass\s+([A-Za-z_$][\w$]*)/) || [])[1] || null;
    const imports = extractImports(src);
    // 未解析到类名时归入哨兵键，确保 native 声明仍参与比对
    const classPath = cls ? `${pkg.replace(/\./g, "/")}/${cls}` : "(未解析类)";
    const natives = [];
    for (const m of extractNativeMethods(src)) {
      const d = buildMethodDescriptor(m, imports, pkg);
      natives.push({
        name: m.name,
        desc: d.ok ? d.desc : null,
        error: d.ok ? null : d.reason,
        line: m.line,
      });
    }
    if (!byClass.has(classPath)) byClass.set(classPath, new Map());
    const cm = byClass.get(classPath);
    for (const n of natives) {
      // 复合键（name+desc）避免同名不同签名被静默覆盖；同 name 不同签名时记重复警告（潜在重载/冲突）
      const ck = `${n.name}\\u0000${n.desc ?? ""}`;
      if (cm.has(ck)) continue; // 完全重复（同 name 同 desc）仅保留首个
      const dupKey = [...cm.keys()].find((k) => k.startsWith(n.name + "\\u0000"));
      if (dupKey) {
        dupWarnings.push(
          `[JAVA_DUP] class=${classPath} name=${n.name} 存在多个签名（${dupKey.split("\\u0000")[1]} 与 ${n.desc ?? ""}），疑似重载/冲突，请人工核对`
        );
      }
      cm.set(ck, n);
    }
    files.push({ rel, classPath, natives });
  }
  return { byClass, files, warnings: dupWarnings };
}

// ---------- 比对 ----------
/** 将复合键(name+sig) Map 归并为 name -> entries[]（每个 entry 保留自身 name/sig） */
function groupByName(compositeMap) {
  const byName = new Map();
  for (const v of compositeMap.values()) {
    if (!byName.has(v.name)) byName.set(v.name, []);
    byName.get(v.name).push(v);
  }
  return byName;
}

function compareByClass(cppByClass, javaByClass) {
  const diffs = [];
  let cppTotal = 0;
  let javaTotal = 0;
  const classPaths = new Set([...cppByClass.keys(), ...javaByClass.keys()].filter((k) => k !== null));

  for (const cp of classPaths) {
    const cppByName = groupByName(cppByClass.get(cp) || new Map());
    const javaByName = groupByName(javaByClass.get(cp) || new Map());
    cppTotal += [...cppByName.values()].reduce((s, a) => s + a.length, 0);
    javaTotal += [...javaByName.values()].reduce((s, a) => s + a.length, 0);
    for (const [name, jList] of javaByName) {
      const cList = cppByName.get(name);
      if (!cList) {
        diffs.push({
          class: cp, name, kind: "JAVA_ONLY",
          message: `Java 声明了 ${name}，但 C++ 注册表缺失（需在对应 register<Domain>Methods 中补注册）`,
        });
        continue;
      }
      for (const j of jList) {
        if (j.error) {
          diffs.push({ class: cp, name, kind: "JAVA_UNRESOLVED", message: `Java 签名无法转换：${j.error}` });
          continue;
        }
        // 静态链接符号仅按名核对，不参与 SIG_MISMATCH 判定（静态注册无显式签名，无法比对描述符）
        const nonStatic = cList.filter((c) => !c.staticSymbol);
        const sigMatch = nonStatic.some((c) => c.sig === j.desc);
        if (nonStatic.length > 0 && !sigMatch) {
          diffs.push({
            class: cp, name, kind: "SIG_MISMATCH",
            message: `签名不一致：Java=${j.desc} C++=${nonStatic.map((c) => c.sig).join(" / ")}（${nonStatic.map((c) => c.file).join(" / ")}）`,
          });
        }
      }
    }
    for (const [name, cList] of cppByName) {
      if (!javaByName.has(name)) {
        diffs.push({
          class: cp, name, kind: "CPP_ONLY",
          message: `C++ 注册了 ${name}，但 ${cp} 中无对应 native 声明（${cList.map((c) => c.file).join(" / ")}）`,
        });
      }
    }
  }

  // 注：classPath 为 null 的 C++ 注册（registry 未把该 domain 映射到 Java 类）不参与本处按类比对——
  // 当前仓库所有 jni/*.cpp 均声明 register<Domain>Methods 且经 registry.cpp 映射到类，
  // native-lib.cpp 不含注册条目；若出现未映射条目，已在 scanCpp 中收集为 [CPP_UNMAPPED] 告警而非静默丢弃。

  return { diffs, cppTotal, javaTotal };
}

// ---------- 入口 ----------
async function main() {
  const args = parseArgs(process.argv);
  const repoRoot = path.resolve(args.repoRoot || path.join(__dirname, ".."));

  const cpp = await scanCpp(repoRoot, args);
  const java = await scanJava(repoRoot, args);
  const { diffs, cppTotal, javaTotal } = compareByClass(cpp.byClass, java.byClass);
  const warnings = [...(cpp.warnings || []), ...(java.warnings || [])];
  const allMatch = diffs.length === 0;

  const classSummary = [];
  const classPaths = new Set([...cpp.byClass.keys(), ...java.byClass.keys()].filter((k) => k !== null));
  for (const cp of classPaths) {
    const c = (cpp.byClass.get(cp) || new Map()).size;
    const j = (java.byClass.get(cp) || new Map()).size;
    const d = diffs.filter((x) => x.class === cp).length;
    // 仅保留含 native 方法或存在差异的类，避免无关类噪音
    if (c > 0 || j > 0 || d > 0) {
      classSummary.push({ class: cp, cpp: c, java: j, diffCount: d });
    }
  }
  classSummary.sort((a, b) => a.class.localeCompare(b.class));

  const report = {
    runId: `${nowIsoLocal()}-${Math.random().toString(16).slice(2, 10)}`,
    ok: allMatch,
    summary: { cpp: cppTotal, java: javaTotal, diffCount: diffs.length, warningCount: warnings.length },
    classes: classSummary,
    diffs,
    warnings,
  };

  // 人可读输出
  process.stdout.write(`JNI_SYNC: ${allMatch ? "ALL_MATCH" : "MISMATCH"} cpp=${cppTotal} java=${javaTotal} diffs=${diffs.length} warnings=${warnings.length}\n`);
  if (args.verbose) {
    for (const cs of classSummary) {
      process.stdout.write(`  class ${cs.class}: cpp=${cs.cpp} java=${cs.java} diffs=${cs.diffCount}\n`);
    }
    for (const e of cpp.entries) {
      const staticTag = e.staticSymbol ? ` (静态符号 ${e.staticSymbol})` : "";
      process.stdout.write(`  cpp ${e.name} ${e.sig} @ ${e.file}${staticTag}\n`);
    }
  }
  for (const w of warnings) {
    process.stdout.write(`  [WARN] ${w}\n`);
  }
  for (const d of diffs) {
    process.stdout.write(`  [${d.kind}] ${d.class}#${d.name}: ${d.message}\n`);
  }
  process.stdout.write(JSON.stringify(report, null, 2) + "\n");

  // 可选 JSON 报告落盘（供 CI 归档）
  if (args.outDir) {
    const outDirAbs = path.resolve(repoRoot, args.outDir);
    await fsp.mkdir(outDirAbs, { recursive: true });
    await fsp.writeFile(path.join(outDirAbs, "jni_sync_audit.json"), JSON.stringify(report, null, 2) + "\n", "utf8");
    process.stdout.write(`report written: ${toPosix(path.relative(repoRoot, path.join(outDirAbs, "jni_sync_audit.json")))}\n`);
  }

  process.exit(allMatch ? 0 : 2);
}

if (require.main === module) {
  main().catch((e) => {
    process.stderr.write(String(e && e.stack ? e.stack : e) + "\n");
    process.exit(1);
  });
}

module.exports = {
  stripComments,
  stripMangledOverloadSuffix,
  extractRegistryEntries,
  parseRegistryClassMap,
  extractNativeMethods,
  splitParams,
  resolveTypeToDescriptor,
  buildMethodDescriptor,
  compareByClass,
};
