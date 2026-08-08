const assert = require("node:assert/strict");
const { spawnSync } = require("node:child_process");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const {
  extractRegistryEntries,
  stripMangledOverloadSuffix,
  compareByClass,
} = require("../../scripts/jni-sync-audit.js");

const REPO_ROOT = path.resolve(__dirname, "../..");
const AUDIT_JS = path.join(REPO_ROOT, "scripts/jni-sync-audit.js");

function makeTempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix));
}

function write(root, relativePath, text) {
  const target = path.join(root, relativePath);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, text, "utf8");
}

test("stripMangledOverloadSuffix 剥离 C++ mangled 重载编号", () => {
  assert.equal(
    stripMangledOverloadSuffix("Java_com_example_rk3288_1opencv_MainActivity_nativePush__2"),
    "Java_com_example_rk3288_1opencv_MainActivity_nativePush"
  );
  assert.equal(
    stripMangledOverloadSuffix("Java_com_example_rk3288_1opencv_MainActivity_nativePush__3"),
    "Java_com_example_rk3288_1opencv_MainActivity_nativePush"
  );
  // 单下划线编号段也剥离（容错）
  assert.equal(
    stripMangledOverloadSuffix("Java_com_example_rk3288_1opencv_MainActivity_nativePush_2"),
    "Java_com_example_rk3288_1opencv_MainActivity_nativePush"
  );
  // 以数字结尾的正常方法名（无下划线分隔）不受影响
  assert.equal(
    stripMangledOverloadSuffix("Java_com_example_rk3288_1opencv_MainActivity_face2"),
    "Java_com_example_rk3288_1opencv_MainActivity_face2"
  );
  // 无编号符号原样返回
  assert.equal(
    stripMangledOverloadSuffix("Java_com_example_rk3288_1opencv_MainActivity_stringFromJNI"),
    "Java_com_example_rk3288_1opencv_MainActivity_stringFromJNI"
  );
});

test("extractRegistryEntries 从 mangled 静态符号还原方法名", () => {
  const src = `
    void registerFixtureMethods(JniMethodRegistry& r) {
      r.add({
        { "nativePush", "(Ljava/nio/ByteBuffer;)Z", reinterpret_cast<void*>(&nativePush) },
        { "Java_com_example_rk3288_1opencv_MainActivity_legacyInit", "placeholder", reinterpret_cast<void*>(&legacyInit) },
        { "Java_com_example_rk3288_1opencv_MainActivity_nativePush__2", "placeholder", reinterpret_cast<void*>(&nativePushOverload) },
      });
    }
  `;
  const entries = extractRegistryEntries(src);
  assert.equal(entries.length, 3);

  const normal = entries.find((e) => !e.staticSymbol && e.name === "nativePush");
  assert.equal(normal.sig, "(Ljava/nio/ByteBuffer;)Z");

  const legacy = entries.find((e) => e.staticSymbol === "Java_com_example_rk3288_1opencv_MainActivity_legacyInit");
  assert.equal(legacy.name, "legacyInit");
  assert.equal(legacy.sig, "(静态链接，无显式签名)");

  // mangled 重载名还原为同一方法名，编号段不被误取为方法名
  const overload = entries.find((e) => e.staticSymbol === "Java_com_example_rk3288_1opencv_MainActivity_nativePush__2");
  assert.equal(overload.name, "nativePush");
  assert.equal(overload.sig, "(静态链接，无显式签名)");
  assert.equal(overload.staticSymbol, "Java_com_example_rk3288_1opencv_MainActivity_nativePush__2");
});

test("静态 Java_ 符号按名核对，不误报 SIG_MISMATCH", () => {
  const cppByClass = new Map([
    ["com/example/rk3288_opencv/MainActivity", new Map([
      ["legacyInit\\u0000(静态链接，无显式签名)", {
        name: "legacyInit",
        sig: "(静态链接，无显式签名)",
        staticSymbol: "Java_com_example_rk3288_1opencv_MainActivity_legacyInit",
        file: "src/cpp/jni/engine.cpp",
      }],
    ])],
  ]);
  const javaByClass = new Map([
    ["com/example/rk3288_opencv/MainActivity", new Map([
      ["legacyInit\\u0000(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z", {
        name: "legacyInit",
        desc: "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z",
      }],
    ])],
  ]);
  const r = compareByClass(cppByClass, javaByClass);
  // 静态符号仅按名核对，不因显式签名缺失误报 SIG_MISMATCH
  assert.equal(r.diffs.length, 0);
  assert.equal(r.cppTotal, 1);
  assert.equal(r.javaTotal, 1);
});

test("同名不同签名 → [CPP_DUP]/[JAVA_DUP] 软警告且不阻断", () => {
  const root = makeTempDir("jni-sync-dup-");
  try {
    write(root, "src/cpp/jni/registry.cpp", `
#include <jni.h>
#include "JniMethodRegistry.h"
jint registerAllNativeMethods(JNIEnv* env) {
    JniMethodRegistry r;
    registerDupMethods(r);
    jclass c = env->FindClass("com/example/rk3288_opencv/DupTest");
    jint count = r.registerAll(env, c);
    return count;
}
`);
    write(root, "src/cpp/jni/dup.cpp", `
void registerDupMethods(JniMethodRegistry& r) {
    r.add({
        { "foo", "()V", reinterpret_cast<void*>(&fooVoid) },
        { "foo", "(I)V", reinterpret_cast<void*>(&fooInt) },
    });
}
`);
    write(root, "src/java/com/example/rk3288_opencv/DupTest.java", `
package com.example.rk3288_opencv;
public class DupTest {
    native void foo();
    native void foo(int x);
}
`);
    const r = spawnSync("node", [AUDIT_JS, "--repo-root", root], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /\[WARN\] \[CPP_DUP\]/);
    assert.match(r.stdout, /\[WARN\] \[JAVA_DUP\]/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("registry 未映射的领域条目 → [CPP_UNMAPPED] 告警且不阻断（EXIT=0）", () => {
  const root = makeTempDir("jni-sync-unmapped-");
  try {
    write(root, "src/cpp/jni/registry.cpp", `
#include <jni.h>
#include "JniMethodRegistry.h"
jint registerAllNativeMethods(JNIEnv* env) {
    JniMethodRegistry r;
    registerMappedMethods(r);
    // 注意：registerGhostMethods 未被调用，Ghost 领域在 registry.cpp 中无映射
    jclass c = env->FindClass("com/example/rk3288_opencv/MappedTest");
    jint count = r.registerAll(env, c);
    return count;
}
`);
    write(root, "src/cpp/jni/mapped.cpp", `
void registerMappedMethods(JniMethodRegistry& r) {
    r.add({ { "foo", "()V", reinterpret_cast<void*>(&fooVoid) } });
}
`);
    write(root, "src/cpp/jni/ghost.cpp", `
void registerGhostMethods(JniMethodRegistry& r) {
    r.add({ { "bar", "()V", reinterpret_cast<void*>(&barVoid) } });
}
`);
    write(root, "src/java/com/example/rk3288_opencv/MappedTest.java", `
package com.example.rk3288_opencv;
public class MappedTest {
    native void foo();
}
`);
    const r = spawnSync("node", [AUDIT_JS, "--repo-root", root], { encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr);
    // 未映射条目告警，而非静默丢弃；已映射的 foo 仍正常匹配，不误报
    assert.match(r.stdout, /\[WARN\] \[CPP_UNMAPPED\]/);
    assert.match(r.stdout, /ghost\.cpp.*Ghost.*bar/);
    assert.doesNotMatch(r.stdout, /MappedTest#foo.*CPP_ONLY|SIG_MISMATCH/);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("真实仓库全量比对 19/19 EXIT=0", () => {
  const r = spawnSync("node", [AUDIT_JS], { cwd: REPO_ROOT, encoding: "utf8" });
  assert.equal(r.status, 0, r.stderr);
  assert.match(r.stdout, /JNI_SYNC: ALL_MATCH cpp=19 java=19 diffs=0 warnings=0/);
});
