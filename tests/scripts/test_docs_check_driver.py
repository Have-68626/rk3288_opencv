"""用本地PowerShell替身验证两项门禁退出码，不运行真实门禁。"""
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "scripts/run-docs-checks.ps1"


@pytest.mark.parametrize("audit,hygiene,expected", [(0, 0, 0), (2, 0, 2), (0, 7, 7), (2, 7, 2)])
def test_docs_driver_propagates_both_steps_from_other_cwd(tmp_path, audit, hygiene, expected):
    assert DRIVER.exists(), "缺少正式文档门禁包装"
    root = tmp_path / "含 空格的仓库"
    (root / "scripts").mkdir(parents=True)
    shutil.copyfile(DRIVER, root / "scripts/run-docs-checks.ps1")
    node = root / "fake-node.ps1"
    node.write_text(
        "param([Parameter(ValueFromRemainingArguments)]$Rest)\n"
        "Add-Content -LiteralPath (Join-Path $PSScriptRoot 'calls.txt') "
        "-Value ((Get-Location).Path + '|' + $Rest[0])\n"
        f"if ($Rest[0] -like '*docs-sync-audit.js') {{ exit {audit} }}\n"
        f"exit {hygiene}\n", encoding="utf8")
    result = subprocess.run(["pwsh", "-NoProfile", "-File", str(root / "scripts/run-docs-checks.ps1"),
                             "-NodePath", str(node), "-OutputDir", str(tmp_path / "报告")],
                            cwd=tmp_path, capture_output=True, text=True, encoding="utf8", timeout=30)
    assert result.returncode == expected, result.stdout + result.stderr
    calls = (root / "calls.txt").read_text(encoding="utf-8-sig").splitlines()
    assert len(calls) == 2
    assert all(line.split("|")[0] == str(root) for line in calls)
    assert "AUDIT_EXIT=" + str(audit) in result.stdout
    assert "HYGIENE_EXIT=" + str(hygiene) in result.stdout


def test_docs_driver_missing_executable_is_failure(tmp_path):
    assert DRIVER.exists(), "缺少正式文档门禁包装"
    result = subprocess.run(["pwsh", "-NoProfile", "-File", str(DRIVER),
                             "-NodePath", str(tmp_path / "不存在的命令")],
                            capture_output=True, text=True, encoding="utf8", timeout=30)
    assert result.returncode != 0
    assert "AUDIT_EXIT=127" in result.stdout
    assert "HYGIENE_EXIT=127" in result.stdout
    assert str(tmp_path) not in result.stdout + result.stderr


def test_docs_driver_preserves_native_exit_with_caller_error_preference(tmp_path):
    root = tmp_path / "native"
    (root / "scripts").mkdir(parents=True)
    shutil.copyfile(DRIVER, root / "scripts/run-docs-checks.ps1")
    (root / "scripts/docs-sync-audit.js").write_text("raise SystemExit(2)")
    (root / "scripts/clean-repo-junk.js").write_text("raise SystemExit(0)")
    caller = root / "caller.ps1"
    caller.write_text(
        "param([string]$NodePath)\n"
        "$PSNativeCommandUseErrorActionPreference = $true\n"
        "& (Join-Path $PSScriptRoot 'scripts/run-docs-checks.ps1') -NodePath $NodePath\n"
        "exit $LASTEXITCODE\n", encoding="utf8")
    result = subprocess.run(["pwsh", "-NoProfile", "-File", str(caller), "-NodePath", sys.executable],
                            capture_output=True, text=True, encoding="utf8", timeout=30)
    assert result.returncode == 2, result.stdout + result.stderr
    assert "AUDIT_EXIT=2" in result.stdout
    assert "HYGIENE_EXIT=0" in result.stdout
