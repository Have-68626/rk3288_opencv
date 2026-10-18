// 仅在合成目录恢复自造小文件；不读取真实隔离区或删除资产。
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const script = process.env.RK_CLEANUP_SCRIPT || path.resolve(__dirname, '../../scripts/clean-repo-junk.js');

for (const present of [false, true]) {
  test(`恢复结果传播：隔离源存在=${present}`, () => {
    const base = fs.mkdtempSync(path.join(os.tmpdir(), 'cleanup-result-'));
    try {
      const root = path.join(base, 'repo');
      fs.mkdirSync(path.join(root, '.trae/specs/clean-repo-junk'), { recursive: true });
      fs.writeFileSync(path.join(root, '.trae/specs/clean-repo-junk/spec.md'), '# fixture');
      const source = path.join(root, 'quarantine.txt');
      const target = path.join(root, 'restored.txt');
      if (present) fs.writeFileSync(source, 'fixture');
      const manifest = path.join(base, 'manifest.json');
      fs.writeFileSync(manifest, JSON.stringify({ items: [{ originalPath: target, quarantinePath: source,
        action: 'quarantine', status: 'moved', bytes: 7 }] }));
      const output = path.join(base, 'reports');
      const result = spawnSync(process.execPath, [script, 'restore', '--root', root, '--manifest', manifest,
        '--output-dir', output, '--no-dry-run', '--yes'], { encoding: 'utf8', timeout: 15000 });
      const file = fs.readdirSync(output).find(x => /^restore-report-.*\.json$/.test(x));
      const report = JSON.parse(fs.readFileSync(path.join(output, file), 'utf8'));
      assert.equal(result.status, present ? 0 : 1, result.stderr);
      assert.equal(report.totals.failedItems, present ? 0 : 1);
      assert.equal(report.totals.restoredItems, present ? 1 : 0);
      assert.equal(fs.existsSync(target), present);
    } finally {
      fs.rmSync(base, { recursive: true, force: true });
    }
  });
}

test('隔离移动失败必须传播为非零退出码', () => {
  const base = fs.mkdtempSync(path.join(os.tmpdir(), 'cleanup-failed-'));
  try {
    const root = path.join(base, 'repo');
    fs.mkdirSync(path.join(root, '.trae/specs/clean-repo-junk'), { recursive: true });
    fs.writeFileSync(path.join(root, '.trae/specs/clean-repo-junk/spec.md'), '# fixture');
    const source = path.join(root, 'fixture.log');
    fs.writeFileSync(source, 'keep-fixture');
    const config = path.join(base, 'config.json');
    fs.writeFileSync(config, JSON.stringify({ targets: [{ kind: 'path', pattern: 'fixture.log', action: 'quarantine' }] }));
    const preload = path.join(base, 'failed-rename.cjs');
    fs.writeFileSync(preload, `const f = require('node:fs/promises'); const move = f.rename;
f.rename = async (source, target) => { if (source === ${JSON.stringify(source)}) {
  const e = new Error('fixture move failed'); e.code = 'EIO'; throw e;
} return move(source, target); };`);
    const output = path.join(base, 'reports');
    const result = spawnSync(process.execPath, ['--require', preload, script, 'clean', '--root', root,
      '--config', config, '--output-dir', output, '--quarantine-dir', path.join(base, 'quarantine'),
      '--no-dry-run', '--yes'], { encoding: 'utf8', timeout: 15000 });
    assert.equal(result.status, 1, result.stderr);
    const file = fs.readdirSync(output).find(x => /^clean-manifest-.*\.json$/.test(x));
    const report = JSON.parse(fs.readFileSync(path.join(output, file), 'utf8'));
    assert.equal(report.totals.failedItems, 1);
    assert.equal(fs.readFileSync(source, 'utf8'), 'keep-fixture');
  } finally { fs.rmSync(base, { recursive: true, force: true }); }
});
