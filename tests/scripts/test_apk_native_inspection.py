"""自造ELF/ZIP验证ABI与依赖拒绝路径，不需要私有模型。"""
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import zipfile

import pytest

from android_tool_test_support import ROOT, load_tool

ABIS = {"armeabi-v7a": (1, 40), "arm64-v8a": (2, 183)}


def elf(elf_class=1, machine=40, needed=("libc.so",), textrel=False, core=False):
    data = bytearray(768)
    data[:7] = b"\x7fELF" + bytes([elf_class, 1, 1])
    struct.pack_into("<H", data, 16, 3)
    struct.pack_into("<H", data, 18, machine)
    names = b"\0.shstrtab\0.text\0.dynstr\0.dynamic\0"
    data[128:128+len(names)] = names
    data[200:216] = b"T" * 16
    strings = b"\0" + b"\0".join(n.encode() for n in needed) + b"\0"
    data[240:240+len(strings)] = strings
    fmt = "<iI" if elf_class == 1 else "<qQ"
    entries, offset = [], 1
    for name in needed:
        entries.append(struct.pack(fmt, 1, offset))
        offset += len(name) + 1
    if textrel: entries.append(struct.pack(fmt, 22, 0))
    entries.append(struct.pack(fmt, 0, 0))
    dynamic = b"".join(entries)
    data[320:320+len(dynamic)] = dynamic
    sections = [(0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
                (1, 3, 0, 0, 128, len(names), 0, 0, 1, 0),
                (11, 1, 0, 0, 200, 16, 0, 0, 1, 0),
                (17, 3, 0, 0, 240, len(strings), 0, 0, 1, 0),
                (25, 6, 0, 0, 320, len(dynamic), 3, 0, 1, struct.calcsize(fmt))]
    section_fmt = "<IIIIIIIIII" if elf_class == 1 else "<IIQQQQIIQQ"
    stride = struct.calcsize(section_fmt)
    for i, section in enumerate(sections): struct.pack_into(section_fmt, data, 400+i*stride, *section)
    struct.pack_into("<I" if elf_class == 1 else "<Q", data, 32 if elf_class == 1 else 40, 400)
    struct.pack_into("<HHH", data, 46 if elf_class == 1 else 58, stride, len(sections), 1)
    if core:
        data += b"\0" * (512 * 1024)
        data += b"nativeGetRtmpFrame\0"
    return bytes(data)


def assets(tmp_path, *, wrong_abi=False, missing_dependency=False, stub=False, textrel=False):
    apk, aar = tmp_path / "candidate.apk", tmp_path / "dependency.aar"
    with zipfile.ZipFile(apk, "w") as app, zipfile.ZipFile(aar, "w") as dependency:
        for abi, (bits, machine) in ABIS.items():
            core = elf(bits, machine + 1 if wrong_abi else machine,
                       ("libmissing.so",) if missing_dependency else ("libc.so",), textrel, not stub)
            app.writestr(f"lib/{abi}/libnative-lib.so", core)
            library = elf(bits, machine)
            app.writestr(f"lib/{abi}/libffmpegkit.so", library)
            dependency.writestr(f"jni/{abi}/libffmpegkit.so", library)
        app.writestr("classes.dex", b"Lcom/arthenica/ffmpegkit/FFmpegKit; Lcom/arthenica/smartexception/AbstractExceptions;")
    return apk, aar, {abi: ["libc.so"] for abi in ABIS}


@pytest.mark.parametrize("bits,machine", [(1, 40), (2, 183)])
def test_elf_inspects_both_android_abis(bits, machine):
    tool = load_tool("verify-apk-native.py")
    info = tool.elf_info(elf(bits, machine))
    assert info["machine"] == machine
    assert info["needed"] == ["libc.so"]
    assert len(info["text_sha256"]) == 64


def test_static_apk_does_not_claim_runtime_or_ncnn_binding(tmp_path):
    tool = load_tool("verify-apk-native.py")
    report = tool.inspect_apk(*assets(tmp_path))
    assert report["static_checks_passed"] is True
    assert report["runtime_verified"] is False
    assert report["ncnn_build_binding_verified"] is False
    assert report["signature_checked"] is False


@pytest.mark.parametrize("option", ["wrong_abi", "missing_dependency", "stub", "textrel"])
def test_invalid_apk_is_rejected(tmp_path, option):
    tool = load_tool("verify-apk-native.py")
    with pytest.raises(ValueError):
        tool.inspect_apk(*assets(tmp_path, **{option: True}))


def test_corrupt_and_duplicate_zip_are_rejected(tmp_path):
    tool = load_tool("verify-apk-native.py")
    apk, aar, platform = assets(tmp_path)
    apk.write_bytes(b"not-a-zip")
    with pytest.raises(ValueError): tool.inspect_apk(apk, aar, platform)
    apk, aar, platform = assets(tmp_path)
    with pytest.warns(UserWarning), zipfile.ZipFile(apk, "a") as app:
        app.writestr("classes.dex", b"duplicate")
    with pytest.raises(ValueError): tool.inspect_apk(apk, aar, platform)


def test_malformed_elf_bounds_are_rejected():
    tool = load_tool("verify-apk-native.py")
    for payload in [b"", b"\x7fELF", elf()[:450]]:
        with pytest.raises(ValueError): tool.elf_info(payload)


def test_native_library_rejects_executable_elf_type():
    tool = load_tool("verify-apk-native.py")
    payload = bytearray(elf())
    struct.pack_into("<H", payload, 16, 2)
    with pytest.raises(ValueError): tool.elf_info(bytes(payload))


def test_wrong_build_binding_cannot_claim_ncnn(tmp_path):
    tool = load_tool("verify-apk-native.py")
    apk, aar, platform = assets(tmp_path)
    bad = tmp_path / "bad.so"
    bad.write_bytes(elf(core=True).replace(b"T" * 16, b"F" * 16))
    config = tmp_path / "CMakeCache.txt"
    ninja = tmp_path / "build.ninja"
    config.write_text("RK_SKIP_OPENCV:BOOL=OFF\nRK_ENABLE_NCNN:BOOL=ON\n")
    ninja.write_text("-Dnative_lib_EXPORTS -DRK_HAVE_NCNN=1\n")
    binding = {abi: {"core_library": str(bad), "cmake_cache": str(config), "build_ninja": str(ninja)} for abi in ABIS}
    with pytest.raises(ValueError): tool.inspect_apk(apk, aar, platform, binding)
