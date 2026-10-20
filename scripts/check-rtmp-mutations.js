const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const root = path.resolve(__dirname, '..');
const files = {
  pusher: ['src/java/com/example/rk3288_opencv/FfmpegRtmpPusher.java', 'RK_RTMP_MUTANT_SOURCE'],
  camera: ['src/java/com/example/rk3288_opencv/RtmpCameraInput.java', 'RK_RTMP_CAMERA_MUTANT_SOURCE'],
  ui: ['src/java/com/example/rk3288_opencv/MainActivity.java', 'RK_RTMP_UI_MUTANT_SOURCE'],
};
const mutants = [
  ['错误 FFmpegKit API', 'pusher', '"executeWithArgumentsAsync"', '"executeAsync"', '异步返回不得提前报告完成'],
  ['提前报告完成', 'pusher', 'Object session = kit.getMethod', 'finish(run, true, "STARTED");\n            Object session = kit.getMethod', '异步返回不得提前报告完成'],
  ['失败伪装成功', 'pusher', 'finish(run, code == 0,', 'finish(run, true,', '真实失败必须报告实际返回码'],
  ['放行旧会话回调', 'pusher', 'active != run || run.finished', 'run.finished', '旧完成不得污染新会话'],
  ['放行重复完成', 'pusher', 'if (active != run || run.finished) return;', '', '完成只能通知一次'],
  ['遗漏取消调用', 'pusher', 'if (active.session != null) cancel(active.session);', '', '替换会话必须取消旧会话'],
  ['恢复敏感底层日志', 'pusher', '"-loglevel", "quiet"', '"-loglevel", "debug"', '底层日志必须静默'],
  ['泄漏 RTMP 地址', 'ui', '? "正在启动推流"', '? "正在启动推流: " + url', '界面不得回显地址凭据'],
  ['放行陈旧 UI 完成', 'ui', 'generation != rtmpUiGeneration || isFinishing()', 'isFinishing()', '陈旧完成不得污染重启后的 UI'],
  ['抢占预览读帧入口', 'ui', 'if (!nativeGetRtmpFrame(bitmap))', 'if (!nativeGetFrame(bitmap))', 'MainActivity 必须把实际采集帧交给推流输入器'],
  ['丢弃相机数据', 'camera', 'if (hasFrame) output.write(rgba);', '', 'SocketTimeoutException'],
  ['遗漏监听关闭', 'camera', 'try { listener.close(); } catch (IOException ignored) { }', '', '尚未连接时也必须取消 accept 并释放'],
  ['遗漏连接关闭', 'camera', 'try { connection.close(); } catch (IOException ignored) { }', '', 'SocketTimeoutException'],
  ['吞掉采集失败', 'camera', 'onFailure.run();', '', '采集失败只能报告一次'],
  ['遗漏采集源释放', 'camera', 'close();\n            source.close();', 'close();', '停止必须释放采集源'],
];
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rk-rtmp-mutants-'));
const environment = { ...process.env };
for (const [, key] of Object.values(files)) delete environment[key];
function verify(env) {
  return spawnSync(process.execPath, ['--test', 'tests/scripts/android-rtmp-behavior.test.js'],
    { cwd: root, env, encoding: 'utf8', timeout: 30000 });
}
try {
  const baseline = verify(environment);
  assert.equal(baseline.status, 0, '基线行为测试未通过，停止变异验证');
  for (let index = 0; index < mutants.length; index++) {
    const [name, area, before, after, expected] = mutants[index];
    const [file, variable] = files[area];
    const original = fs.readFileSync(path.join(root, file), 'utf8');
    assert.equal(original.split(before).length, 2, `变异锚点必须唯一：${name}`);
    const directory = path.join(temporary, String(index));
    fs.mkdirSync(directory);
    const target = path.join(directory, path.basename(file));
    fs.writeFileSync(target, original.replace(before, after));
    const result = verify({ ...environment, [variable]: target });
    const output = result.stdout + result.stderr;
    assert.equal(result.status, 1, `变异未被拦截：${name}`);
    assert.ok(!output.includes('Java 编译失败'), `变异必须通过编译后被行为断言拦截：${name}`);
    assert.ok(output.includes(expected), `变异未命中指定行为断言：${name}`);
    console.log(`RTMP_MUTANT_KILLED ${index + 1}/${mutants.length} ${name}`);
  }
  console.log(`RTMP_MUTATION_PASS killed=${mutants.length} survived=0`);
} finally {
  assert.ok(path.resolve(temporary).startsWith(path.resolve(os.tmpdir()) + path.sep));
  fs.rmSync(temporary, { recursive: true, force: true });
}
