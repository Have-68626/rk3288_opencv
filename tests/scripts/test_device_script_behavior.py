"""执行正式PowerShell入口，所有设备访问均在函数边界替换。"""
import csv
import json
import os
from pathlib import Path
import subprocess
import shutil

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = Path(os.environ.get('RK_AUX_SCRIPTS_ROOT', ROOT / 'scripts'))
HARNESS = Path(__file__).parent / 'fixtures/device-script-harness.ps1'


def run(tmp_path, mode, case, devices=1):
    script = SCRIPTS / ('bench_camera_adb.ps1' if mode == 'bench' else 'stability_switch_50_adb.ps1')
    return subprocess.run(['pwsh', '-NoProfile', '-File', str(HARNESS), '-Script', str(script),
        '-Mode', mode, '-Case', case, '-OutputRoot', str(tmp_path / 'output'),
        '-DeviceCount', str(devices)], capture_output=True, text=True, encoding='utf-8', timeout=20)


@pytest.mark.parametrize('mode', ['bench', 'stability'])
@pytest.mark.parametrize('devices', [1, 2])
def test_valid_device_sample_handles_singletons(tmp_path, mode, devices):
    result = run(tmp_path, mode, 'valid', devices)
    assert result.returncode == 0, result.stderr
    if mode == 'bench':
        files = list((tmp_path / 'output').glob('*.csv'))
        rows = list(csv.DictReader(files[0].open(encoding='utf-8-sig')))
        assert rows[0]['capture_ok'] == '2' and rows[0]['ttff_ms'] == '20'
    else:
        files = list((tmp_path / 'output').glob('*/summary.json'))
        assert json.loads(files[0].read_text(encoding='utf-8-sig'))['pass'] is True


@pytest.mark.parametrize('case', ['header-only', 'partial', 'start-failed', 'read-failed'])
def test_bench_rejects_missing_measurements(tmp_path, case):
    result = run(tmp_path, 'bench', case, devices=2)
    assert result.returncode != 0
    assert '已生成基准结果' not in result.stdout


@pytest.mark.parametrize('case', ['empty-log', 'header-only', 'start-failed', 'read-failed', 'missing-focus', 'invalid-back', 'black-screen'])
def test_stability_does_not_pass_missing_or_failed_observations(tmp_path, case):
    result = run(tmp_path, 'stability', case, devices=2)
    assert result.returncode != 0
    for path in (tmp_path / 'output').glob('*/summary.json'):
        assert json.loads(path.read_text(encoding='utf-8-sig'))['pass'] is not True


@pytest.mark.parametrize('mode', ['bench', 'stability'])
def test_no_devices_are_rejected(tmp_path, mode):
    assert run(tmp_path, mode, 'valid', devices=0).returncode != 0


def test_stability_skips_null_focus_before_valid_component(tmp_path):
    result = run(tmp_path, 'stability', 'valid-focus-after-null')
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize('case', ['valid', 'header-only', 'partial', 'start-failed'])
def test_shell_bench_uses_real_metrics(tmp_path, case):
    git = Path(shutil.which('git') or '')
    candidates = [git.parent.parent / 'bin/bash.exe', git.parent / 'bash.exe']
    bash = next((str(p) for p in candidates if p.is_file()), None) if os.name == 'nt' else shutil.which('bash')
    assert bash, '需要Git Bash验证Android宿主脚本'
    sdk = tmp_path / 'sdk'
    (sdk / 'platform-tools').mkdir(parents=True)
    adb = sdk / 'platform-tools/adb'
    adb.write_text('''#!/usr/bin/env bash
if [[ "$1" == devices ]]; then printf 'List of devices attached\\nfixture-device\\tdevice\\n'; exit 0; fi
if [[ " $* " == *" start "* && "$FIXTURE_CASE" == start-failed ]]; then exit 1; fi
if [[ " $* " == *" logcat -d "* ]]; then
  if [[ "$FIXTURE_CASE" == header-only ]]; then echo '--------- beginning of main'
  elif [[ "$FIXTURE_CASE" == partial ]]; then echo 'I/BENCH_CAMERA(1): TTFF_MS=20'
  else echo 'I/BENCH_CAMERA(1): TTFF_MS=20 CAPTURE_OK=2 CAPTURE_FAIL=0 CAMERA_SERVICE_RESTART=0'; fi
fi
exit 0
''', encoding='utf-8')
    adb.chmod(0o755)
    environment = os.environ.copy()
    environment.update(ANDROID_SDK_ROOT=sdk.as_posix(), ANDROID_HOME='', FIXTURE_CASE=case)
    result = subprocess.run([bash, (SCRIPTS / 'bench_camera_adb.sh').as_posix(), 'fixture.application', '.Main',
        '1', '1', 'fixture-device', 'BENCH_CAMERA', (tmp_path / 'output').as_posix()],
        env=environment, capture_output=True, text=True, encoding='utf-8', timeout=20)
    assert result.returncode == (0 if case == 'valid' else 1), result.stderr
    if case == 'valid':
        file = next((tmp_path / 'output').glob('*.csv'))
        rows = list(csv.DictReader(file.open(encoding='utf-8-sig')))
        assert rows[0]['capture_ok'] == '2' and rows[0]['ttff_ms'] == '20'
