#!/usr/bin/env python3
"""INT8 量化脚本：将 FP32 ncnn 模型转换为 INT8 量化模型。

两步流程:
  1) ncnn2table  — 用校准图片生成校准表 (calibration table)
  2) ncnn2int8   — 用校准表将 FP32 权重量化为 INT8

用法:
  python scripts/quantize_ncnn_int8.py --model scrfd \\
      --fp32-dir models/scrfd_face_detect_ncnn \\
      --calib-dir models/calib_images \\
      --output-dir models/int8

支持 --table-only / --quant-only 分离执行。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


# ---------------------------------------------------------------------------
# 模型预设参数（均值/方差/输入尺寸/颜色通道顺序）
# 不同模型训练时的预处理不同，需分别配置
# ---------------------------------------------------------------------------
MODEL_PRESETS = {
    # ⚠️ 校准预处理必须与**生产推理**的预处理一致，否则量化校准分布与实际部署分布错位，
    #    精度对不上。本轮按生产契约对齐（§I1.2），**不要**再改回经验值：
    #    · SCRFD 生产：RGB、mean=127.5×3、norm=1/128、640×640（居中 letterbox，pad=0）。
    #    · ArcFace 生产：RGB、mean=127.5×3、norm=1/127.5、112×112。
    #    · MobileFaceNet：已认可 w600k_mbf 父图，RGB、mean=127.5×3、norm=1/127.5、112×112。
    "scrfd": {
        "mean": [127.5, 127.5, 127.5],
        "norm": [1.0 / 128.0] * 3,
        "shape": [640, 640, 3],
        "pixel": "RGB",
    },
    "arcface": {
        "mean": [127.5, 127.5, 127.5],
        "norm": [1.0 / 127.5] * 3,
        "shape": [112, 112, 3],
        "pixel": "RGB",
    },
    "mobilefacenet": {
        "mean": [127.5, 127.5, 127.5],
        "norm": [1.0 / 127.5] * 3,
        "shape": [112, 112, 3],
        "pixel": "RGB",
    },
}


def find_tool(name: str) -> str:
    """查找 ncnn 工具路径（ncnn2table / ncnn2int8）。"""
    from shutil import which
    path = which(name)
    if path:
        return path

    ncnn_root = os.environ.get("RK_NCNN_ROOT") or os.environ.get("NCNN_ROOT") or "C:/ncnn"
    candidates: list[str] = [
        f"/usr/bin/{name}",
        f"/usr/local/bin/{name}",
    ]
    if os.name == "nt":
        candidates.append(f"{ncnn_root}/tools/quantize/{name}.exe")
        candidates.append(f"{ncnn_root}/bin/{name}.exe")
        candidates.append(f"{ncnn_root}/{name}.exe")
    for c in candidates:
        if os.path.isfile(c):
            return c
    print(f"错误：未找到 {name}，请确保已安装 ncnn 并将 bin/ 加入 PATH"
          f"（或设置 NCNN_ROOT/RK_NCNN_ROOT 环境变量）", file=sys.stderr)
    sys.exit(1)


def collect_calib_images(calib_dir: str, num_samples: int) -> list[str]:
    """从校准目录收集图片路径（按需采样）。"""
    calib_path = Path(calib_dir)
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    images = [str(p) for p in sorted(calib_path.rglob("*")) if p.suffix.lower() in exts]
    if not images:
        print(f"错误：校准目录 {calib_dir} 中未找到图片文件", file=sys.stderr)
        sys.exit(1)
    if len(images) > num_samples:
        step = len(images) // num_samples
        images = images[::step][:num_samples]
    print(f"收集到 {len(images)} 张校准图片（目标 {num_samples} 张）")
    return images


def write_imagelist(images: list[str], path: str):
    """写入图片列表文件（ncnn2table 输入）。"""
    with open(path, "w") as f:
        for img in images:
            f.write(f"{img}\n")
    print(f"图片列表已写入: {path}")


def prepare_scrfd_images(images: list[str], output_dir: Path, tool_path: str) -> str:
    """ncnn2table 双正 shape 会拉伸，先以生产 OpenCV 几何生成无损校准图。"""
    source_list = str(output_dir / "calib_source_imagelist.txt")
    output_list = str(output_dir / "calib_imagelist.txt")
    write_imagelist([str(Path(p).resolve()) for p in images], source_list)
    result = subprocess.run([tool_path, source_list, output_list, "640", "640"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        print("错误：SCRFD letterbox 校准图准备失败", file=sys.stderr)
        sys.exit(1)
    return output_list


def file_sha256(filename: str) -> str:
    digest = hashlib.sha256()
    with open(filename, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def calibration_contract(model: str, preset: dict, param: str, binary: str, table: str) -> dict:
    """校准表必须绑定父模型与几何；分步量化不能凭模型名补写不存在的证据。"""
    return {"model": model, "preprocessing": preset,
            "geometry": "letterbox_center_pad0" if model == "scrfd" else "resize",
            "fp32_param_sha256": file_sha256(param),
            "fp32_bin_sha256": file_sha256(binary), "table_sha256": file_sha256(table)}


def run_ncnn2table(tool_path: str, fp32_param: str, fp32_bin: str,
                   imagelist_path: str, table_path: str,
                   preset: dict, num_threads: int) -> None:
    """Step 1: 调用 ncnn2table 生成校准表。"""
    cfg = preset
    shape_str = f"{cfg['shape'][0]},{cfg['shape'][1]},{cfg['shape'][2]}"
    mean_str = ",".join(str(v) for v in cfg["mean"])
    norm_str = ",".join(str(v) for v in cfg["norm"])

    cmd = [
        tool_path,
        fp32_param, fp32_bin,
        imagelist_path, table_path,
        f"mean=[{mean_str}]",
        f"norm=[{norm_str}]",
        f"shape=[{shape_str}]",
        f"pixel={cfg['pixel']}",
        f"thread={num_threads}",
        "method=kl",
    ]
    print(f"[ncnn2table] 生成校准表...")
    print(f"  模型: {fp32_param}")
    print(f"  校准图片: {imagelist_path} ({len(open(imagelist_path).readlines())} 张)")
    print(f"  shape={shape_str}, mean=[{mean_str}], pixel={cfg['pixel']}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[ncnn2table] 失败:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    if result.stdout:
        # 打印前 5 行概要即可
        for line in result.stdout.strip().splitlines()[:5]:
            print(f"  {line}")
    print(f"  校准表已生成: {table_path}")


def run_ncnn2int8(tool_path: str, fp32_param: str, fp32_bin: str,
                  table_path: str, output_param: str, output_bin: str) -> dict:
    """Step 2: 调用 ncnn2int8 执行 INT8 量化。"""
    cmd = [
        tool_path,
        fp32_param, fp32_bin,
        output_param, output_bin,
        table_path,
    ]
    print(f"[ncnn2int8] 量化模型...")
    print(f"  FP32: {fp32_param}")
    print(f"  INT8: {output_param}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[ncnn2int8] 量化失败:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    if result.stdout:
        for line in result.stdout.strip().splitlines()[:5]:
            print(f"  {line}")

    fp32_size = os.path.getsize(fp32_bin) if os.path.exists(fp32_bin) else 0
    int8_size = os.path.getsize(output_bin) if os.path.exists(output_bin) else 0
    report = {
        "fp32_bin_size": fp32_size,
        "int8_bin_size": int8_size,
        "compression_ratio": round(fp32_size / int8_size, 2) if int8_size > 0 else 0,
    }
    # 精度程序核对四个文件摘要，把 INT8 产物绑定到本次量化的同一父模型。
    report_dir = Path(output_param).resolve().parent
    for key, filename in (("fp32_param", fp32_param), ("fp32_bin", fp32_bin),
                          ("int8_param", output_param), ("int8_bin", output_bin)):
        artifact = Path(filename).resolve()
        report[key] = os.path.relpath(artifact, report_dir).replace(os.sep, "/")
        report[key + "_sha256"] = file_sha256(str(artifact))
    return report


def find_fp32_model(fp32_dir: str) -> tuple[str, str]:
    """在 fp32_dir 中找到唯一同名配对的 .param + .bin。

    要求 param/bin 同 stem 配对唯一（Q-CQ-B08-009）：
      - 无任何同 stem 配对（param 与 bin 错配）→ 报错退出非零；
      - 多个同 stem 配对（多模型混放，共同 stem 不唯一）→ 报错退出非零，
        避免静默选中错误的 param/bin 组合。
    """
    d = Path(fp32_dir)
    params = sorted(d.glob("*.param"))
    bins = sorted(d.glob("*.bin"))
    if not params or not bins:
        print(f"错误：{fp32_dir} 中未找到 .param/.bin 文件", file=sys.stderr)
        sys.exit(1)

    # 仅保留 param 与 bin 同 stem 的候选对，排除跨模型错配
    bin_stems = {p.stem for p in bins}
    paired = [p for p in params if p.stem in bin_stems]
    if not paired:
        print(f"错误：{fp32_dir} 中无 param/bin 同 stem 配对"
              f"（param: {[p.name for p in params]}; "
              f"bin: {[b.name for b in bins]}）",
              file=sys.stderr)
        sys.exit(1)
    if len(paired) > 1:
        print(f"错误：{fp32_dir} 中 param/bin 同 stem 配对不唯一"
              f"（{', '.join(p.stem for p in paired)}），"
              f"请只放入单一模型的 param+bin",
              file=sys.stderr)
        sys.exit(1)
    return str(paired[0]), str(paired[0].with_suffix(".bin"))


def main():
    parser = argparse.ArgumentParser(description="ncnn FP32 → INT8 量化（两步流程）")
    parser.add_argument("--model", required=True,
                        choices=list(MODEL_PRESETS.keys()),
                        help="模型名")
    parser.add_argument("--fp32-dir", required=True,
                        help="FP32 模型目录（含 .param + .bin）")
    parser.add_argument("--calib-dir",
                        help="校准图片目录（--table-only 或完整流程时需要）")
    parser.add_argument("--output-dir", required=True,
                        help="INT8 输出目录")
    parser.add_argument("--num-samples", type=int, default=100,
                        help="校准图片采样数（默认 100）")
    parser.add_argument("--num-threads", type=int, default=4,
                        help="ncnn2table 线程数（默认 4）")
    parser.add_argument("--table-only", action="store_true",
                        help="仅生成校准表，不量化")
    parser.add_argument("--quant-only", action="store_true",
                        help="仅用已有校准表量化，不重新生成")
    parser.add_argument("--table-path",
                        help="--quant-only 时指定已有校准表路径")
    parser.add_argument("--calib-preprocessor",
                        help="SCRFD letterbox 工具 ncnn_calibration_images 路径（也可放 PATH）")
    args = parser.parse_args()

    preset = MODEL_PRESETS[args.model]
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # 校验 preset 与 fp32_dir 一致
    model_key = args.model.lower().replace("_", "")
    dir_key = os.path.basename(os.path.normpath(args.fp32_dir)).lower().replace("_", "").replace("-", "")
    if model_key not in dir_key:
        print(f"错误：--model '{args.model}' 与 --fp32-dir '{args.fp32_dir}' 不匹配", file=sys.stderr)
        print(f"      期望 fp32_dir 包含 '{args.model}'", file=sys.stderr)
        sys.exit(1)

    fp32_param, fp32_bin = find_fp32_model(args.fp32_dir)

    # ---- Step 1：生成校准表 ----
    if not args.quant_only:
        if not args.calib_dir:
            print("错误：--calib-dir 为必填（完整流程或 --table-only）", file=sys.stderr)
            sys.exit(1)
        ncnn2table = find_tool("ncnn2table")
        images = collect_calib_images(args.calib_dir, args.num_samples)
        if args.model == "scrfd":
            preprocessor = args.calib_preprocessor or find_tool("ncnn_calibration_images")
            imagelist_path = prepare_scrfd_images(images, output_dir, preprocessor)
        else:
            imagelist_path = str(output_dir / "calib_imagelist.txt")
            write_imagelist(images, imagelist_path)
        table_path = str(output_dir / f"{args.model}.table")
        run_ncnn2table(ncnn2table, fp32_param, fp32_bin,
                       imagelist_path, table_path, preset, args.num_threads)
        table_contract = calibration_contract(args.model, preset, fp32_param, fp32_bin, table_path)
        Path(table_path + ".json").write_text(json.dumps(table_contract, indent=2), encoding="utf-8")
    else:
        # --quant-only 模式：使用已有校准表
        table_path = args.table_path or str(output_dir / f"{args.model}.table")
        if not os.path.isfile(table_path):
            print(f"错误：校准表不存在: {table_path}", file=sys.stderr)
            sys.exit(1)
        print(f"使用已有校准表: {table_path}")
        # 没有几何/来源证据的旧表须重新校准，不能补写未经验证的生产口径。
        contract_path = Path(table_path + ".json")
        table_contract = json.loads(contract_path.read_text(encoding="utf-8")) if contract_path.is_file() else None
        expected_contract = calibration_contract(args.model, preset, fp32_param, fp32_bin, table_path)
        if table_contract != expected_contract:
            print("错误：校准表缺少或不匹配父模型/预处理证据，请先重新执行 --table-only", file=sys.stderr)
            sys.exit(1)

    if args.table_only:
        print(f"\n--- 校准表已就绪: {table_path} ---")
        print("可以后续执行: python quantize_ncnn_int8.py --model {args.model} "
              f"--fp32-dir {args.fp32_dir} --output-dir {args.output_dir} "
              " --quant-only")
        return

    # ---- Step 2：INT8 量化 ----
    ncnn2int8 = find_tool("ncnn2int8")
    output_param = str(output_dir / f"{args.model}_int8.param")
    output_bin = str(output_dir / f"{args.model}_int8.bin")
    report = run_ncnn2int8(ncnn2int8, fp32_param, fp32_bin,
                           table_path, output_param, output_bin)

    # ---- 写入报告 ----
    report_path = output_dir / "quantize_report.json"
    report["model"] = args.model
    report["preprocessing"] = preset
    report["geometry"] = table_contract["geometry"]
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    print(f"\n量化完成，报告: {report_path}")
    print(f"  FP32: {report['fp32_bin_size']} bytes")
    print(f"  INT8: {report['int8_bin_size']} bytes")
    print(f"  压缩比: {report['compression_ratio']}x")


if __name__ == "__main__":
    main()
