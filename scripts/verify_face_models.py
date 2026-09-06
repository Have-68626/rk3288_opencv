#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
本地模型校验脚本（任务10）。

用途：仅在本机校验 Android 人脸模型的存在性 / SHA-256 / Git LFS pointer 状态，
模型二进制 **不上传 GitHub**（由 .gitignore 保证，本脚本不触碰 git）。

用法：
    python scripts/verify_face_models.py [--report <path>]

输出：本地报告文件（默认 build/face_models_verify.md）+ 控制台摘要。
退出码：全部 PASS=0，任一 FAIL=1（供 Gradle verifyFaceModels 门控）。
"""
import argparse
import datetime
import hashlib
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]

# 各 profile 引用的源模型（均应为 ncnn .param/.bin 对）
PROFILES = {
    "rk3288_light": {
        "detector.param": "models/scrfd_face_detect_ncnn/scrfd_500m_kps-opt2.param",
        "detector.bin":   "models/scrfd_face_detect_ncnn/scrfd_500m_kps-opt2.bin",
        "embedder.param": "models/arcface_int8_ncnn/arcface_int8.param",
        "embedder.bin":   "models/arcface_int8_ncnn/arcface_int8.bin",
    },
    "arm64_accuracy": {
        "detector.param": "models/scrfd_face_detect_ncnn/scrfd_2.5g_kps-opt2.param",
        "detector.bin":   "models/scrfd_face_detect_ncnn/scrfd_2.5g_kps-opt2.bin",
        "embedder.param": "models/arcface_ncnn/arcface.param",
        "embedder.bin":   "models/arcface_ncnn/arcface.bin",
    },
}

LFS_MARKER = b"version https://git-lfs.github.com/spec/v1"


def sha256_hex(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_lfs_pointer(path):
    with open(path, "rb") as f:
        head = f.read(512)
    return LFS_MARKER in head


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", default=str(REPO / "build" / "face_models_verify.md"))
    args = ap.parse_args()

    report = pathlib.Path(args.report)
    report.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "# 本地模型校验报告（任务10）",
        "",
        "- 生成时间：%s" % datetime.datetime.now().isoformat(timespec="seconds"),
        "- 仅本机校验（存在性 / SHA-256 / LFS pointer）；模型二进制不上传 GitHub。",
        "",
        "| Profile | 文件 | 存在 | LFS? | SHA-256 |",
        "|---|---|---|---|---|",
    ]
    ok_all = True
    for profile, files in PROFILES.items():
        for key, rel in files.items():
            p = REPO / rel
            exists = p.is_file()
            lfs = is_lfs_pointer(p) if exists else False
            sha = sha256_hex(p) if exists else "-"
            status = "FAIL"
            if exists and not lfs:
                status = "PASS"
                ok_all = ok_all and True
            else:
                ok_all = False
            lines.append("| %s | %s | %s | %s | %s |" % (profile, key, exists, lfs, sha))
            print("[%s] %-8s %-16s %s" % (status, profile, key, sha))

    summary = "PASS" if ok_all else "FAIL"
    lines.extend([
        "",
        "结论：**%s**" % summary,
        "",
        "> 说明：任一文件缺失 / 为 Git LFS pointer / SHA-256 未能计算即判定 FAIL。",
        "> 许可证：各模型来源与再分发许可须在 models/android/*/LICENSE 记录并经人工确认后方可打包分发。",
    ])
    report.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("报告: %s" % report)
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())