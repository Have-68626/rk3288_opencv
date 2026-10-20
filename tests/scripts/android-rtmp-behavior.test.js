const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('RTMP 生产适配器、相机帧输入器与实际 UI 方法完成边界行为验证', () => {
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'rk-rtmp-'));
  const extension = process.platform === 'win32' ? '.exe' : '';
  const compilerDirectory = process.env.JAVA_HOME ? path.join(process.env.JAVA_HOME, 'bin')
    : (process.env.PATH || '').split(path.delimiter).find(directory =>
      !directory.toLowerCase().includes('javapath') && fs.existsSync(path.join(directory, `javac${extension}`)));
  const binary = command => compilerDirectory ? path.join(compilerDirectory, command + extension) : command;
  try {
    for (const name of ['NonNull', 'Nullable']) {
      fs.writeFileSync(path.join(temporary, `${name}.java`),
        `package androidx.annotation; public @interface ${name} {}`);
    }
    const fixture = path.join(root, 'tests/fixtures/rtmp');
    const sources = fs.readdirSync(fixture).filter(name => name.endsWith('.java'))
      .map(name => path.join(fixture, name));
    const camera = process.env.RK_RTMP_CAMERA_MUTANT_SOURCE
      || path.join(root, 'src/java/com/example/rk3288_opencv/RtmpCameraInput.java');
    if (fs.existsSync(camera)) sources.push(camera);
    const activity = fs.readFileSync(process.env.RK_RTMP_UI_MUTANT_SOURCE
      || path.join(root, 'src/java/com/example/rk3288_opencv/MainActivity.java'), 'utf8');
    const start = activity.indexOf('    private void startRtmpPush()');
    const end = activity.indexOf('    private void toggleFullscreen()', start);
    assert.ok(start >= 0 && end > start, '生产 RTMP UI 方法缺失');
    const boundary = fs.readFileSync(path.join(fixture, 'RtmpUiBoundary.java.template'), 'utf8');
    const uiSource = path.join(temporary, 'MainActivity.java');
    fs.writeFileSync(uiSource, boundary.replace('__RTMP_METHODS__', activity.slice(start, end)));
    sources.push(uiSource);
    const production = process.env.RK_RTMP_MUTANT_SOURCE
      || path.join(root, 'src/java/com/example/rk3288_opencv/FfmpegRtmpPusher.java');
    const compile = spawnSync(binary('javac'), ['-encoding', 'UTF-8', '-d', temporary,
      ...sources, path.join(temporary, 'NonNull.java'), path.join(temporary, 'Nullable.java'), production],
    { encoding: 'utf8', timeout: 30000 });
    assert.equal(compile.status, 0, `Java 编译失败：${compile.stderr || compile.error}`);
    const execution = spawnSync(binary('java'), ['-cp', temporary,
      'com.example.rk3288_opencv.RtmpBehaviorHarness'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(execution.status, 0, execution.stderr || String(execution.error));
    assert.equal(execution.stdout.trim(), 'RTMP_BEHAVIOR_PASS');
    const cameraExecution = spawnSync(binary('java'), ['-cp', temporary,
      'com.example.rk3288_opencv.RtmpCameraBehaviorHarness'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(cameraExecution.status, 0, cameraExecution.stderr || String(cameraExecution.error));
    assert.equal(cameraExecution.stdout.trim(), 'RTMP_CAMERA_BEHAVIOR_PASS');
    const uiExecution = spawnSync(binary('java'), ['-cp', temporary,
      'com.example.rk3288_opencv.MainActivity'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(uiExecution.status, 0, uiExecution.stderr || String(uiExecution.error));
    assert.equal(uiExecution.stdout.trim(), 'RTMP_UI_BEHAVIOR_PASS');
    const missingKit = path.join(temporary, 'without-kit');
    fs.cpSync(path.join(temporary, 'com/example'), path.join(missingKit, 'com/example'), { recursive: true });
    const absentExecution = spawnSync(binary('java'), ['-cp', missingKit,
      'com.example.rk3288_opencv.RtmpMissingKitHarness'], { encoding: 'utf8', timeout: 10000 });
    assert.equal(absentExecution.status, 0, absentExecution.stderr || String(absentExecution.error));
    assert.equal(absentExecution.stdout.trim(), 'RTMP_MISSING_KIT_PASS');
  } finally {
    assert.ok(path.resolve(temporary).startsWith(path.resolve(os.tmpdir()) + path.sep));
    fs.rmSync(temporary, { recursive: true, force: true });
  }
});
