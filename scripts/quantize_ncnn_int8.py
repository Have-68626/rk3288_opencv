#!/usr/bin/env python3
"""INT8 量化脚本：将 FP32 ncnn 模型转换为 INT8 量化模型。"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def find_ncnn2int8():
    """查找 ncnn2int8 工具路径。"""
    from shutil import which
    path = which("ncnn2int8")
    if path:
        return path
    candidates = [
        "/usr/bin/ncnn2int8",
        "/usr/local/bin/ncnn2int8",
        "C:/ncnn/tools/quantize/ncnn2int8.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def collect_calib_images(calib_dir: str, num_samples: int, size: int) -> list[str]:
    """从校准目录收集图片路径。"""
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


def run_quantize(ncnn2int8_path: str, fp32_param: str, fp32_bin: str,
                 calib_images: list[str], output_param: str, output_bin: str,
                 size: int) -> dict:
    """调用 ncnn2int8 执行量化，返回报告。"""
    table_path = output_param + ".table"
    with open(table_path, "w") as f:
        for img in calib_images:
            f.write(f"{img} {size} {size}\n")

    cmd = [
        ncnn2int8_path,
        fp32_param, fp32_bin,
        output_param, output_bin,
        table_path,
    ]
    print(f"执行: {' '.join(cmd[:4])} ...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"量化失败:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)

    fp32_size = os.path.getsize(fp32_bin) if os.path.exists(fp32_bin) else 0
    int8_size = os.path.getsize(output_bin) if os.path.exists(output_bin) else 0
    report = {
        "fp32_bin_size": fp32_size,
        "int8_bin_size": int8_size,
        "compression_ratio": round(fp32_size / int8_size, 2) if int8_size > 0 else 0,
        "num_calib_images": len(calib_images),
    }
    return report


def main():
    parser = argparse.ArgumentParser(description="ncnn FP32 → INT8 量化")
    parser.add_argument("--model", required=True, choices=["yolo_face", "arcface", "mobilefacenet"])
    parser.add_argument("--fp32-dir", required=True, help="FP32 模型目录（含 .param + .bin）")
    parser.add_argument("--calib-dir", required=True, help="校准图片目录")
    parser.add_argument("--output-dir", required=True, help="INT8 输出目录")
    parser.add_argument("--size", type=int, default=640, help="输入尺寸")
    parser.add_argument("--num-samples", type=int, default=100, help="校准图片采样数")
    args = parser.parse_args()

    ncnn2int8 = find_ncnn2int8()
    if not ncnn2int8:
        print("错误：未找到 ncnn2int8 工具，请确保已安装 ncnn", file=sys.stderr)
        sys.exit(1)
    print(f"使用 ncnn2int8: {ncnn2int8}")

    fp32_dir = Path(args.fp32_dir)
    param_files = list(fp32_dir.glob("*.param"))
    bin_files = list(fp32_dir.glob("*.bin"))
    if not param_files or not bin_files:
        print(f"错误：{args.fp32_dir} 中未找到 .param/.bin 文件", file=sys.stderr)
        sys.exit(1)

    fp32_param = str(param_files[0])
    fp32_bin = str(bin_files[0])
    print(f"FP32 模型: {fp32_param}, {fp32_bin}")

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    calib_images = collect_calib_images(args.calib_dir, args.num_samples, args.size)
    output_param = str(output_dir / f"{args.model}_int8.param")
    output_bin = str(output_dir / f"{args.model}_int8.bin")

    report = run_quantize(ncnn2int8, fp32_param, fp32_bin,
                          calib_images, output_param, output_bin, args.size)

    report_path = output_dir / "quantize_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2)
    print(f"量化完成，报告: {report_path}")
    print(f"压缩比: {report['compression_ratio']}x "
          f"({report['fp32_bin_size']} → {report['int8_bin_size']} bytes)")


if __name__ == "__main__":
    main()
