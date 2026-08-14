# Q-CQ-B08-009 配对校验测试：quantize_ncnn_int8.py 的 param/bin 配对必须唯一同 stem。
#
# RED 判别逻辑（修复前 RED，修复后 GREEN）：
#   - 修复前 find_fp32_model() 取排序后的第一个 .param 与第一个 .bin，
#     不校验两者是否同名：param/bin 错配（model_a.param + model_b.bin）
#     或多模型同 stem 混放（共同 stem 不唯一）时都静默返回，不报错退出。
#   - 修复后：无同 stem 配对（错配）或多对同 stem 配对（多模型）都
#     sys.exit(1)，仅唯一同名配对放行。
#
# 运行：python -m pytest tests/scripts/test_quantize_ncnn_int8_pairing.py
import importlib.util
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "quantize_ncnn_int8.py"

# scripts/ 无 __init__.py，按文件路径加载模块
_spec = importlib.util.spec_from_file_location("quantize_ncnn_int8", SCRIPT)
quant = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(quant)


def _mk(tmp_path, names):
    """在临时目录创建若干空文件，返回目录字符串路径。"""
    for n in names:
        (tmp_path / n).touch()
    return str(tmp_path)


def test_unique_pair_ok(tmp_path):
    """唯一同 stem 配对 → 返回该 param/bin（回归保护）。"""
    d = _mk(tmp_path, ["det_10g.param", "det_10g.bin"])
    param, bin_ = quant.find_fp32_model(d)
    assert param == str(tmp_path / "det_10g.param")
    assert bin_ == str(tmp_path / "det_10g.bin")


def test_mismatch_no_common_stem_exits_nonzero(tmp_path):
    """param/bin 错配（无共同 stem）→ sys.exit(1)，不得静默返回。"""
    d = _mk(tmp_path, ["model_a.param", "model_b.bin"])
    with pytest.raises(SystemExit) as e:
        quant.find_fp32_model(d)
    assert e.value.code == 1


def test_multiple_pairs_exits_nonzero(tmp_path):
    """多模型同 stem 配对（共同 stem 不唯一）→ sys.exit(1)。"""
    d = _mk(tmp_path, [
        "det_10g.param", "det_10g.bin",
        "w600k.param", "w600k.bin",
    ])
    with pytest.raises(SystemExit) as e:
        quant.find_fp32_model(d)
    assert e.value.code == 1


def test_missing_files_exits_nonzero(tmp_path):
    """无 param/bin → 既有行为 sys.exit(1)（回归保护）。"""
    d = _mk(tmp_path, ["unrelated.txt"])
    with pytest.raises(SystemExit) as e:
        quant.find_fp32_model(d)
    assert e.value.code == 1
