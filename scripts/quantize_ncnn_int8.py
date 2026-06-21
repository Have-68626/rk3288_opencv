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
    "scrfd": {
        "mean": [104, 117, 123],
        "norm": [0.017, 0.017, 0.017],
        "shape": [640, 640, 3],
        "pixel": "BGR",
    },
    "arcface": {
        "mean": [127.5, 127.5, 127.5],
        "norm": [0.0078125, 0.0078125, 0.0078125],
        "shape": [112, 112, 3],
        "pixel": "RGB",
    },
    "mobilefacenet": {
        "mean": [127.5, 127.5, 127.5],
        "norm": [0.0078125, 0.0078125, 0.0078125],
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
    candidates = [
        f"/usr/bin/{name}",
        f"/usr/local/bin/{name}",
        f"C:/ncnn/tools/quantize/{name}.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    print(f"错误：未找到 {name}，请确保已安装 ncnn 并将 bin/ 加入 PATH", file=sys.stderr)
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
    return report


def find_fp32_model(fp32_dir: str) -> tuple[str, str]:
    """在 fp32_dir 中找到第一对 .param + .bin。"""
    d = Path(fp32_dir)
    params = sorted(d.glob("*.param"))
    bins = sorted(d.glob("*.bin"))
    if not params or not bins:
        print(f"错误：{fp32_dir} 中未找到 .param/.bin 文件", file=sys.stderr)
        sys.exit(1)
    return str(params[0]), str(bins[0])


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
    args = parser.parse_args()

    preset = MODEL_PRESETS[args.model]
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    fp32_param, fp32_bin = find_fp32_model(args.fp32_dir)

    # ---- Step 1：生成校准表 ----
    if not args.quant_only:
        if not args.calib_dir:
            print("错误：--calib-dir 为必填（完整流程或 --table-only）", file=sys.stderr)
            sys.exit(1)
        ncnn2table = find_tool("ncnn2table")
        images = collect_calib_images(args.calib_dir, args.num_samples)
        imagelist_path = str(output_dir / "calib_imagelist.txt")
        write_imagelist(images, imagelist_path)
        table_path = str(output_dir / f"{args.model}.table")
        run_ncnn2table(ncnn2table, fp32_param, fp32_bin,
                       imagelist_path, table_path, preset, args.num_threads)
    else:
        # --quant-only 模式：使用已有校准表
        table_path = args.table_path or str(output_dir / f"{args.model}.table")
        if not os.path.isfile(table_path):
            print(f"错误：校准表不存在: {table_path}", file=sys.stderr)
            sys.exit(1)
        print(f"使用已有校准表: {table_path}")

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
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"\n量化完成，报告: {report_path}")
    print(f"  FP32: {report['fp32_bin_size']} bytes")
    print(f"  INT8: {report['int8_bin_size']} bytes")
    print(f"  压缩比: {report['compression_ratio']}x")


if __name__ == "__main__":
    main()
