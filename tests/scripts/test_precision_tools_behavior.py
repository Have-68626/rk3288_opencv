"""使用真实工具可执行文件和自造固定件；不读取任何私有模型或媒体。"""
import json
import os
import subprocess
import struct
import sys
from pathlib import Path

import pytest


def tool(name):
    override = os.environ.get("RK_PRECISION_TOOL_" + name.upper())
    if override:
        return override
    directory = os.environ.get("RK_PRECISION_TOOLS_DIR")
    if not directory:
        pytest.skip("需设置 RK_PRECISION_TOOLS_DIR 指向本机工具构建输出")
    return str(Path(directory) / (name + (".exe" if os.name == "nt" else "")))


def run(name, *args, cwd=None):
    environment = os.environ.copy()
    if cwd is not None:
        environment.update(TEMP=str(cwd), TMP=str(cwd))
    return subprocess.run([tool(name), *map(str, args)], cwd=cwd,
                          env=environment, capture_output=True, text=True, encoding="utf-8", errors="replace")


def test_missing_precision_assets_are_unverified(tmp_path):
    result = run("ncnn_precision_test", cwd=tmp_path)
    assert result.returncode == 77, result.stdout + result.stderr
    assert "TEST_UNVERIFIED" in result.stdout
    assert "TEST_PASS name=ncnn_precision_detection_iou" not in result.stdout


def test_opencv_missing_model_message(tmp_path):
    result = run("inference_bench_cli", "--backend", "opencv", "--out-dir", tmp_path)
    assert "缺少 --opencv-model" in result.stderr


def test_ncnn_failed_iterations_do_not_claim_effective(tmp_path):
    param = tmp_path / "synthetic.param"
    binary = tmp_path / "synthetic.bin"
    param.write_text("7767517\n2 2\nInput input 0 1 data\nReLU relu 1 1 data output\n", encoding="ascii")
    binary.write_bytes(b"")
    result = run("inference_bench_cli", "--backend", "ncnn", "--ncnn-param", param,
                 "--ncnn-bin", binary, "--ncnn-input", "data", "--ncnn-output", "absent",
                 "--iters", 2, "--warmup", 0, "--out-dir", tmp_path)
    reports = list(tmp_path.glob("*.json"))
    assert len(reports) == 1, result.stdout + result.stderr
    report = json.loads(reports[0].read_text(encoding="utf-8"))
    record = report["results"][0]
    assert record["ok_iters"] == 0
    assert record["err_iters"] == 2
    assert record["backendReason"] == "runtime_init_failed"
    assert record["backendEffective"] == "none"


def write_bmp(path, width=4, height=2):
    stride = (width * 3 + 3) & ~3
    pixels = (bytes([10, 20, 30]) * width + bytes(stride - width * 3)) * height
    header = struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54)
    info = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixels), 0, 0, 0, 0)
    path.write_bytes(header + info + pixels)


def test_scrfd_calibration_geometry_preserves_aspect_and_centers_pad(tmp_path):
    image = tmp_path / "synthetic.bmp"
    write_bmp(image)
    source = tmp_path / "source.txt"
    source.write_text(str(image) + "\n", encoding="utf-8")
    output = tmp_path / "prepared.txt"
    result = run("ncnn_calibration_images", source, output, 8, 8)
    assert result.returncode == 0, result.stdout + result.stderr
    bmp = Path(output.read_text(encoding="utf-8").strip()).read_bytes()
    width, height = struct.unpack_from("<ii", bmp, 18)
    assert (width, height) == (8, 8)
    offset = struct.unpack_from("<I", bmp, 10)[0]
    stride = (width * 3 + 3) & ~3
    rows = [bmp[offset + r * stride:offset + r * stride + width * 3] for r in range(height)]
    assert rows[0] == rows[1] == rows[6] == rows[7] == bytes(24)
    assert rows[2] == rows[5] == bytes([10, 20, 30]) * 8


def test_precision_metric_and_arcface_preprocess_contracts(tmp_path):
    result = run("ncnn_precision_test", "--self-test", cwd=tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "TEST_PASS name=ncnn_precision_contracts" in result.stdout


def prepare_pair(tmp_path, model="scrfd"):
    import hashlib
    image_dir = tmp_path / "images"
    image_dir.mkdir()
    write_bmp(image_dir / "synthetic.bmp")
    model_dir = tmp_path / "models" / (model + "_int8_ncnn")
    model_dir.mkdir(parents=True)
    report = {"model": model, "geometry": "letterbox_center_pad0"}
    for key in ["fp32_param", "fp32_bin", "int8_param", "int8_bin"]:
        path = model_dir / key
        if key.endswith("param"):
            flag = " 8=1" if key.startswith("int8") else ""
            path.write_text("7767517\n2 2\nInput input 0 1 input.1\n"
                            "Convolution conv 1 1 input.1 out0 0=1 1=1 6=3" + flag + "\n", encoding="ascii")
        else:
            binary = (struct.pack("<I", 0x000D4B38) + bytes(4) + struct.pack("<2f", 1, 1)
                      if key.startswith("int8") else struct.pack("<I3f", 0, 0, 0, 0))
            path.write_bytes(binary)
        report[key] = path.name
        report[key + "_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    destination = model_dir / "quantize_report.json"
    destination.write_text(json.dumps(report), encoding="utf-8")
    return report, destination


def test_precision_rejects_changed_parent_before_inference(tmp_path):
    report, destination = prepare_pair(tmp_path)
    (destination.parent / report["fp32_param"]).write_text("changed", encoding="ascii")
    result = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert result.returncode == 1, result.stdout + result.stderr
    assert "摘要不匹配" in result.stderr


def test_scrfd_missing_output_is_failure_not_skip(tmp_path):
    prepare_pair(tmp_path)
    result = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert result.returncode == 1, result.stdout + result.stderr
    assert "ncnn_extract_failed" in result.stderr
    assert "TEST_UNVERIFIED" not in result.stdout


def test_precision_rejects_cross_architecture_report(tmp_path):
    report, destination = prepare_pair(tmp_path)
    report["model"] = "yolo_face"
    destination.write_text(json.dumps(report), encoding="utf-8")
    result = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert result.returncode == 1, result.stdout + result.stderr
    assert "架构不匹配" in result.stderr


def test_precision_rejects_quantized_model_as_fp32_reference(tmp_path):
    import hashlib
    report, destination = prepare_pair(tmp_path)
    parent = destination.parent / report["fp32_param"]
    parent.write_text(parent.read_text(encoding="ascii").rstrip() + " 8=1\n", encoding="ascii")
    report["fp32_param_sha256"] = hashlib.sha256(parent.read_bytes()).hexdigest()
    destination.write_text(json.dumps(report), encoding="utf-8")
    result = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert result.returncode == 1, result.stdout + result.stderr
    assert "FP32 父模型实际含 INT8 层" in result.stderr


def test_windows_benchmark_excludes_warmup_frames_and_reports_quantiles(tmp_path):
    cascade = os.environ.get("RK_PUBLIC_CASCADE_PATH")
    if not cascade:
        pytest.skip("需设置 RK_PUBLIC_CASCADE_PATH 指向 OpenCV 公共级联模型")
    images = tmp_path / "images"
    images.mkdir()
    write_bmp(images / "synthetic.bmp", 80, 80)
    result = run("win_face_bench_cli", "--input", images, "--cascade", cascade,
                 "--db", tmp_path / "absent.yml", "--warmup", 3, "--iters", 2)
    assert result.returncode == 0, result.stdout + result.stderr
    summary = next(line for line in result.stdout.splitlines() if line.startswith("BENCH_SUMMARY"))
    values = dict(item.split("=", 1) for item in summary.split()[1:])
    assert values["warmup_frames"] == "3"
    assert values["frames"] == "2"
    assert 0 <= float(values["p50_ms"]) <= float(values["p95_ms"])


def test_database_benchmark_reports_measured_sample_count(tmp_path):
    result = run("win_face_database_perf", "--warmup", 1, "--iters", 2, cwd=tmp_path)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "BENCH_SCOPE warmup_iters=1 measured_iters=2 includes_cold_start=false" in result.stdout
    assert "TEST_RESULT: PASS" in result.stdout
    for operation in ["SAVE", "LOAD"]:
        values = dict(line.split(": ", 1) for line in result.stdout.splitlines() if ": " in line)
        assert 0 <= float(values[operation + "_P50_MS"]) <= float(values[operation + "_P95_MS"])


def write_scrfd_fixture(directory):
    directory.mkdir(parents=True)
    lines = ["7767517", "11 19", "Input input 0 1 input.1",
             "Split split 1 9 input.1 " + " ".join("branch" + str(i) for i in range(9))]
    weights = bytearray()
    for group, channels in enumerate([2, 8, 20]):
        for level, stride in enumerate([8, 16, 32]):
            index = group * 3 + level
            lines.append(f"Convolution head{index} 1 1 branch{index} out{index} "
                         f"0={channels} 1=1 3={stride} 5=1 6={channels * 3}")
            weights.extend(struct.pack("<I", 0))
            weights.extend(struct.pack("<" + "f" * (channels * 3), *([0.001] * (channels * 3))))
            bias = [0.9, 0] if group == 0 else [1.0] * channels if group == 1 else [0.0] * channels
            weights.extend(struct.pack("<" + "f" * channels, *bias))
    (directory / "scrfd.param").write_text("\n".join(lines) + "\n", encoding="ascii")
    (directory / "scrfd.bin").write_bytes(weights)


def test_synthetic_scrfd_quantize_to_actual_detection_metrics(tmp_path):
    """九头合成图实际经校准/INT8转换/生产解码，不能用张量存在冒充精度。"""
    fp32 = tmp_path / "models" / "scrfd_ncnn"
    write_scrfd_fixture(fp32)
    images = tmp_path / "images"
    images.mkdir()
    write_bmp(images / "synthetic.bmp", 80, 40)
    environment = os.environ.copy()
    environment["PATH"] = str(Path(tool("ncnn2table")).parent) + os.pathsep + environment["PATH"]
    script = Path(__file__).resolve().parents[2] / "scripts" / "quantize_ncnn_int8.py"
    result = subprocess.run([sys.executable, str(script), "--model", "scrfd", "--fp32-dir", str(fp32),
                             "--calib-dir", str(images), "--output-dir", str(tmp_path / "models" / "scrfd_int8_ncnn"),
                             "--calib-preprocessor", tool("ncnn_calibration_images"), "--num-samples", "1"],
                            env=environment, capture_output=True, text=True, encoding="utf-8", errors="replace")
    assert result.returncode == 0, result.stdout + result.stderr
    precision = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert precision.returncode == 0, precision.stdout + precision.stderr
    metric = next(line for line in precision.stdout.splitlines() if line.startswith("PRECISION_METRIC"))
    values = dict(item.split("=", 1) for item in metric.split()[1:])
    assert int(values["matched"]) > 0
    assert float(values["matched_mean_iou"]) >= 0.80
    assert "relative_recall" in values and "relative_precision" in values
    # 保持全部张量形状，仅把 INT8 score 偏置清零；必须由检测指标判失败。
    import hashlib
    report_path = tmp_path / "models" / "scrfd_int8_ncnn" / "quantize_report.json"
    report = json.loads(report_path.read_text(encoding="utf-8"))
    int8_bin = report_path.parent / report["int8_bin"]
    binary = int8_bin.read_bytes()
    assert binary.count(struct.pack("<f", 0.9)) >= 3
    binary = binary.replace(struct.pack("<f", 0.9), struct.pack("<f", 0.0))
    int8_bin.write_bytes(binary)
    report["int8_bin_sha256"] = hashlib.sha256(binary).hexdigest()
    report_path.write_text(json.dumps(report), encoding="utf-8")
    degraded = run("ncnn_precision_test", "--root", tmp_path, "--images", "images", "--mode", "scrfd")
    assert degraded.returncode == 1, degraded.stdout + degraded.stderr
    assert "matched=0" in degraded.stdout
