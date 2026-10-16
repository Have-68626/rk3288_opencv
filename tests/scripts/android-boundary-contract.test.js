const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = process.env.RK_REVIEW_ANDROID_ROOT || path.resolve(__dirname, '../..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');

test('网络媒体入口在主清单声明 INTERNET 普通权限', () => {
  const manifest = read('app/src/main/AndroidManifest.xml');
  assert.match(manifest, /<uses-permission\b[^>]*android:name="android\.permission\.INTERNET"/);
});

test('GMS 包探测只声明其所需的单包可见性', () => {
  const manifest = read('app/src/main/AndroidManifest.xml');
  assert.match(manifest, /<queries>\s*<package\s+android:name="com\.google\.android\.gms"\s*\/?>\s*<\/queries>/);
  assert.doesNotMatch(manifest, /android\.permission\.QUERY_ALL_PACKAGES/);
});

test('FileProvider 只暴露专用拍照缓存目录', () => {
  const xml = read('app/src/main/res/xml/file_paths.xml');
  const roots = [...xml.matchAll(/<(external-path|cache-path|files-path|root-path|external-files-path|external-cache-path)\b([^>]*)\/>/g)];
  assert.equal(roots.length, 1);
  assert.equal(roots[0][1], 'cache-path');
  assert.match(roots[0][2], /\bpath="captured_images\/"/);
});
