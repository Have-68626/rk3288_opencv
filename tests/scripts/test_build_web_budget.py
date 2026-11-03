"""显式输出必须受同一700KB门槛约束，不能误验默认旧webroot。"""
from pathlib import Path
import os
import shutil
import subprocess
import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.skipif(not shutil.which('node'), reason='需要项目已有Node工具链')
@pytest.mark.parametrize('size,expected', [(12, 0), (701 * 1024, 1)])
def test_bundle_budget_checks_explicit_output(tmp_path, size, expected):
    (tmp_path / 'assets').mkdir()
    (tmp_path / 'index.html').write_text('<script type="module" src="./assets/entry.js"></script>', encoding='utf-8')
    (tmp_path / 'assets/entry.js').write_bytes(b'x' * size)
    result = subprocess.run([shutil.which('node'), str(ROOT / 'web/scripts/check-bundle-budget.mjs'),
                             '--webroot', str(tmp_path)], capture_output=True, text=True, encoding='utf-8', timeout=20)
    assert result.returncode == expected
    assert 'entry.js' in result.stdout + result.stderr
