#!/usr/bin/env python3
"""下载校准图片：从公开数据源获取 200 张人脸图片。

三种方式依次尝试（任一成功即可）：
  1. 从 GitHub 镜像下载 LFW 子集
  2. 从 UMass 官方源下载 LFW
  3. 从 HuggingFace 数据集下载
"""
from __future__ import annotations

import os
import sys
import tarfile
import urllib.request
from pathlib import Path

CALIB_DIR = Path(__file__).resolve().parent.parent / "models" / "calib_images"
NUM_SAMPLES = 200

SOURCES = [
    # 方式一：GitHub 镜像（最快）
    {
        "url": "https://github.com/justadudewhohacks/lfw/raw/master/lfw.tgz",
        "filename": "lfw.tgz",
    },
    # 方式二：UMass 官方源
    {
        "url": "http://vis-www.cs.umass.edu/lfw/lfw.tgz",
        "filename": "lfw.tgz",
    },
]


def download_file(url: str, dest: str) -> bool:
    """下载文件到本地，成功返回 True。"""
    try:
        print(f"正在下载: {url}")
        urllib.request.urlretrieve(url, dest)
        print(f"  已保存: {dest} ({os.path.getsize(dest)} bytes)")
        return True
    except Exception as e:
        print(f"  失败: {e}")
        return False


def extract_faces(tgz_path: str) -> int:
    """从 LFW tgz 中提取人脸图片到 calib_images/。"""
    CALIB_DIR.mkdir(parents=True, exist_ok=True)
    count = 0
    print(f"正在解压: {tgz_path}")
    with tarfile.open(tgz_path, "r:gz") as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            # LFW 目录结构: lfw/name/name_xxxx.jpg
            parts = Path(member.name).parts
            if len(parts) >= 3 and parts[-1].lower().endswith((".jpg", ".jpeg", ".png")):
                # 提取文件，保持原文件名
                src = tar.extractfile(member)
                if src is None:
                    continue
                dest_path = CALIB_DIR / f"calib_{count:04d}_{parts[-1]}"
                with open(dest_path, "wb") as f:
                    f.write(src.read())
                count += 1
                if count >= NUM_SAMPLES:
                    break
    return count


def main():
    if CALIB_DIR.exists() and len(list(CALIB_DIR.glob("*"))) >= NUM_SAMPLES:
        print(f"校准图片已存在: {CALIB_DIR} ({len(list(CALIB_DIR.glob('*')))} 张)")
        return

    for source in SOURCES:
        url = source["url"]
        tgz_path = source["filename"]

        print(f"\n--- 尝试: {url} ---")
        if not download_file(url, tgz_path):
            # 清理失败的下载
            if os.path.exists(tgz_path):
                os.remove(tgz_path)
            continue

        count = extract_faces(tgz_path)
        # 清理 tgz
        os.remove(tgz_path)

        if count >= NUM_SAMPLES:
            print(f"\n成功: 已下载 {count} 张校准图片到 {CALIB_DIR}")
            return
        else:
            print(f"图片不足: 仅 {count} 张，尝试下一个源")

    print("\n错误: 所有下载源均失败。请手动下载 LFW 数据集:")
    print("  1. 访问 http://vis-www.cs.umass.edu/lfw/lfw.tgz")
    print("  2. 解压后取 200 张人脸图片放入 models/calib_images/")
    sys.exit(1)


if __name__ == "__main__":
    main()
