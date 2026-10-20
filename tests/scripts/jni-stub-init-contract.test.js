const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

test('降级桩 nativeInit 的 C++ 参数类型必须匹配实际 Java 声明', () => {
  const root = path.resolve(__dirname, '../..');
  const java = fs.readFileSync(path.join(root, 'src/java/com/example/rk3288_opencv/MainActivity.java'), 'utf8');
  const cpp = fs.readFileSync(path.join(root, 'src/cpp/native-lib-stub.cpp'), 'utf8');
  const declaration = java.match(/native\s+boolean\s+nativeInit\s*\(([^)]*)\)/);
  const definition = cpp.match(/jboolean\s+JNICALL\s+Java_com_example_rk3288_1opencv_MainActivity_nativeInit\s*\(([^)]*)\)/);
  assert.ok(declaration, 'Java nativeInit 声明缺失');
  assert.ok(definition, 'C++ nativeInit 桩缺失');
  const types = { String: 'jstring', int: 'jint', boolean: 'jboolean' };
  const expected = declaration[1].split(',').map(parameter => {
    const javaType = parameter.trim().split(/\s+/)[0];
    assert.ok(types[javaType], `未支持的 Java 参数类型：${javaType}`);
    return types[javaType];
  });
  const actual = definition[1].replace(/\/\*[\s\S]*?\*\//g, '').split(',').slice(2)
    .map(parameter => parameter.trim().split(/\s+/)[0]);
  assert.deepEqual(actual, expected);
});
