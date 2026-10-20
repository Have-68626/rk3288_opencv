"""不依赖本机区域设置，验证中文诊断和校准路径的编码边界。"""
import builtins
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[2]


def legacy_cli(script, *args):
    environment = os.environ.copy()
    environment.update(PYTHONUTF8="0", PYTHONIOENCODING="cp1252:strict")
    return subprocess.run([sys.executable, "-X", "utf8=0", str(script), *map(str, args)],
                          env=environment, capture_output=True, timeout=20)


def test_model_cli_writes_report_with_legacy_stdout(tmp_path):
    script = tmp_path / "repo/scripts/verify_face_models.py"
    script.parent.mkdir(parents=True)
    shutil.copyfile(ROOT / "scripts/verify_face_models.py", script)
    result = legacy_cli(script)
    assert result.returncode == 1
    assert "模型清单缺失或无法解析" in result.stdout.decode("utf-8")
    assert b"Traceback" not in result.stderr
    report = script.parents[1] / "cache/reports/face-models/face_models_verify.md"
    assert "结论：**FAIL**" in report.read_text(encoding="utf-8")


def test_quantize_help_with_legacy_stdout():
    result = legacy_cli(ROOT / "scripts/quantize_ncnn_int8.py", "--help")
    assert result.returncode == 0, result.stderr
    assert "量化" in result.stdout.decode("utf-8")


def test_quantize_failure_with_legacy_stderr(tmp_path):
    model = tmp_path / "arcface"
    model.mkdir()
    (model / "model.param").touch()
    (model / "model.bin").touch()
    result = legacy_cli(ROOT / "scripts/quantize_ncnn_int8.py", "--model", "arcface",
                        "--fp32-dir", model, "--output-dir", tmp_path / "out")
    assert result.returncode == 1
    assert "--calib-dir 为必填" in result.stderr.decode("utf-8")
    assert b"Traceback" not in result.stderr


@pytest.mark.parametrize("operation", ["write", "read"])
def test_calibration_list_chinese_paths_with_legacy_file_encoding(tmp_path, monkeypatch, operation):
    spec = importlib.util.spec_from_file_location("quant_encoding", ROOT / "scripts/quantize_ncnn_int8.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    listing = tmp_path / "images.txt"
    image = str(tmp_path / "省内校准图片.bmp")
    if operation == "read":
        listing.write_text(image + "\n", encoding="utf-8")
    original_open = builtins.open

    def legacy_open(file, mode="r", buffering=-1, encoding=None, errors=None, newline=None,
                    closefd=True, opener=None):
        if "b" not in mode and encoding in (None, "locale"):
            encoding = "cp1252"
        return original_open(file, mode, buffering, encoding, errors, newline, closefd, opener)

    monkeypatch.setattr(builtins, "open", legacy_open)
    if operation == "write":
        tool.write_imagelist([image], str(listing))
        assert listing.read_text(encoding="utf-8") == image + "\n"
    else:
        calls = []
        def invoke(command, **kwargs):
            calls.append(command)
            return SimpleNamespace(returncode=0, stdout="", stderr="")
        monkeypatch.setattr(tool.subprocess, "run", invoke)
        tool.run_ncnn2table("ncnn2table", "m.param", "m.bin", str(listing),
                           str(tmp_path / "m.table"), tool.MODEL_PRESETS["arcface"], 1)
        assert calls and calls[0][3] == str(listing)
