const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');

test('相机 RTMP 独立读帧入口在 Java、真实 JNI 和降级桩签名一致', () => {
  const java = read('src/java/com/example/rk3288_opencv/MainActivity.java');
  const cpp = read('src/cpp/jni/preview.cpp');
  const stub = read('src/cpp/native-lib-stub.cpp');
  assert.match(java, /public native boolean nativeGetRtmpFrame\(Bitmap bitmap\);/);
  assert.match(cpp, /\{"nativeGetRtmpFrame", "\(Landroid\/graphics\/Bitmap;\)Z",\s*reinterpret_cast<void\*>\(preview_nativeGetRtmpFrame\)\}/);
  assert.match(stub, /jboolean JNICALL\s*Java_com_example_rk3288_1opencv_MainActivity_nativeGetRtmpFrame\(JNIEnv\*[^,]*, jobject[^,]*, jobject\)\s*\{\s*return JNI_FALSE;/);
  for (const entry of ['nativeGetFrame', 'nativeGetRtmpFrame']) {
    const method = cpp.match(new RegExp(`jboolean preview_${entry}\\([^)]*\\) \\{([^}]+)\\}`));
    assert.ok(method);
    assert.match(method[1], /static std::mutex seqMu/);
    assert.match(method[1], /static uint64_t lastSeq/);
    assert.match(method[1], /copyRenderFrameToBitmap\(env, bitmap, seqMu, lastSeq\)/);
  }
  assert.match(java, /protected void onDestroy\(\) \{\s*super\.onDestroy\(\);\s*\+\+rtmpUiGeneration;\s*stopRtmpCameraInput\(\);\s*rtmpPusher\.stop\(\);/);
});
