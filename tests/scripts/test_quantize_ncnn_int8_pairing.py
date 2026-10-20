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


# ── §I1.2 校准预处理与生产契约一致 ───────────────────────────────────────────
# 判据落在**实际传给 ncnn2table 的 argv**（而非只查 MODEL_PRESETS 字典）：
# 字典与 argv 若有任一处漂移（如字典改了但 run_ncnn2table 没透传），本组用例都会红。
def _capture_ncnn2table_argv(monkeypatch, tmp_path, model):
    """monkeypatch subprocess.run，捕获 run_ncnn2table 实际拼出的命令。"""
    captured = {}

    class _Result:
        returncode = 0
        stdout = ""
        stderr = ""

    def _fake_run(cmd, *args, **kwargs):
        captured["cmd"] = list(cmd)
        return _Result()

    monkeypatch.setattr(quant.subprocess, "run", _fake_run)

    imagelist = tmp_path / "imagelist.txt"
    imagelist.write_text("img1.png\nimg2.png\n", encoding="utf-8")
    quant.run_ncnn2table(
        "ncnn2table", "m.param", "m.bin",
        str(imagelist), str(tmp_path / "m.table"),
        quant.MODEL_PRESETS[model], 4,
    )
    return captured["cmd"]


def _assert_argv_has(cmd, token):
    assert token in cmd, f"argv 缺少 {token}：{cmd}"


def test_scrfd_preset_matches_production_preprocessing(monkeypatch, tmp_path):
    """SCRFD 校准预设 = 生产契约：RGB / mean 127.5×3 / norm 1/128 / 640×640。"""
    cmd = _capture_ncnn2table_argv(monkeypatch, tmp_path, "scrfd")
    _assert_argv_has(cmd, "pixel=RGB")
    _assert_argv_has(cmd, "mean=[127.5,127.5,127.5]")
    _assert_argv_has(cmd, "norm=[0.0078125,0.0078125,0.0078125]")
    _assert_argv_has(cmd, "shape=[640,640,3]")
    # 反例：旧的 BGR/ImageNet 均值不得再出现在传给工具的命令里。
    assert not any("pixel=BGR" in a for a in cmd)
    assert not any("mean=[104" in a for a in cmd)


def test_arcface_preset_matches_production_preprocessing(monkeypatch, tmp_path):
    """ArcFace 校准预设 = 生产契约：RGB / mean 127.5×3 / norm 1/127.5 / 112×112。"""
    cmd = _capture_ncnn2table_argv(monkeypatch, tmp_path, "arcface")
    _assert_argv_has(cmd, "pixel=RGB")
    _assert_argv_has(cmd, "mean=[127.5,127.5,127.5]")
    _assert_argv_has(cmd, f"norm=[{1.0 / 127.5},{1.0 / 127.5},{1.0 / 127.5}]")
    _assert_argv_has(cmd, "shape=[112,112,3]")


def test_mobilefacenet_preset_matches_approved_parent(monkeypatch, tmp_path):
    """MobileFaceNet 指定父图的官方输入为 RGB / 127.5 / 1÷127.5 / 112。"""
    cmd = _capture_ncnn2table_argv(monkeypatch, tmp_path, "mobilefacenet")
    _assert_argv_has(cmd, "pixel=RGB")
    _assert_argv_has(cmd, "mean=[127.5,127.5,127.5]")
    _assert_argv_has(cmd, f"norm=[{1.0 / 127.5},{1.0 / 127.5},{1.0 / 127.5}]")
    _assert_argv_has(cmd, "shape=[112,112,3]")


def test_quantization_report_binds_both_artifact_pairs(monkeypatch, tmp_path):
    """父模型与量化产物的路径和摘要必须可供真实精度程序复核。"""
    import hashlib

    source_param = tmp_path / "scrfd.param"
    source_bin = tmp_path / "scrfd.bin"
    output_param = tmp_path / "scrfd_int8.param"
    output_bin = tmp_path / "scrfd_int8.bin"
    source_param.write_bytes(b"source-param")
    source_bin.write_bytes(b"source-weights")
    output_param.write_bytes(b"quantized-param")
    output_bin.write_bytes(b"quantized-weights")

    class Result:
        returncode = 0
        stdout = ""
        stderr = ""

    monkeypatch.setattr(quant.subprocess, "run", lambda *a, **k: Result())
    report = quant.run_ncnn2int8("ncnn2int8", str(source_param), str(source_bin),
                               "table", str(output_param), str(output_bin))
    for key, artifact in [("fp32_param", source_param), ("fp32_bin", source_bin),
                          ("int8_param", output_param), ("int8_bin", output_bin)]:
        assert report[key] == artifact.name
        assert report[key + "_sha256"] == hashlib.sha256(artifact.read_bytes()).hexdigest()


def test_quant_only_rejects_calibration_table_without_geometry_evidence(monkeypatch, tmp_path, capsys):
    """旧表没有来源/几何元数据，不能直接宣称它按生产 letterbox 校准。"""
    import sys
    directory = tmp_path / "scrfd_ncnn"
    directory.mkdir()
    _mk(directory, ["scrfd.param", "scrfd.bin"])
    table = tmp_path / "scrfd.table"
    table.write_bytes(b"synthetic-table")
    monkeypatch.setattr(sys, "argv", ["quantize", "--model", "scrfd", "--fp32-dir", str(directory),
                                     "--output-dir", str(tmp_path / "output"), "--quant-only", "--table-path", str(table)])
    monkeypatch.setattr(quant, "find_tool", lambda name: pytest.fail("缺证据时不得启动量化工具"))
    with pytest.raises(SystemExit) as error:
        quant.main()
    assert error.value.code == 1
    assert "校准表缺少或不匹配父模型/预处理证据" in capsys.readouterr().err
