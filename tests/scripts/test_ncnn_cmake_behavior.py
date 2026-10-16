"""在最小 OpenCV 外壳中执行真实模块，检查 ncnn 路径与消费方编译契约。"""
import os
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def package(root, marker, target="ncnn"):
    directory = root / "lib/cmake/ncnn"
    directory.mkdir(parents=True)
    if target == "alias":
        declaration = "add_library(fixture_ncnn INTERFACE IMPORTED)\nadd_library(ncnn::ncnn ALIAS fixture_ncnn)"
        owner = "fixture_ncnn"
    else:
        declaration = f"add_library({target} INTERFACE IMPORTED)"
        owner = target
    (directory / "ncnnConfig.cmake").write_text(
        declaration + f'\nset(RK_TEST_PACKAGE_MARKER "{marker}")\n'
        f'set_property(TARGET {owner} PROPERTY INTERFACE_COMPILE_OPTIONS '
        '"-fno-rtti;-fno-exceptions;-DNCNN_KEEP_OPTION=1")\n', encoding="utf-8")
    return directory


def configure(tmp_path, definitions, *, environment_root=None):
    cmake, ninja = shutil.which("cmake"), shutil.which("ninja")
    if not cmake:
        pytest.skip("需要本机 CMake")
    opencv = tmp_path / "opencv"
    opencv.mkdir()
    (opencv / "CMakeLists.txt").write_text("# 此夹具不编译 OpenCV，只执行项目的依赖装配模块。\n", encoding="utf-8")
    source = tmp_path / "project"
    source.mkdir()
    module = Path(os.environ.get("RK_NCNN_CMAKE_UNDER_TEST", ROOT / "cmake/opencv.cmake"))
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.18.1)\nproject(NcnnContract NONE)\n"
        f'set(OPENCV_ROOT "{opencv.as_posix()}")\n'
        "set(RK_ENABLE_LIBYUV OFF)\nset(RK_NCNN_FETCHCONTENT OFF)\n"
        # 排除宿主包和用户注册表，防止本机已安装的另一 ABI 掩盖路径丢失。
        "set(CMAKE_FIND_USE_CMAKE_SYSTEM_PATH OFF)\n"
        "set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)\n"
        "set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)\n"
        "set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)\n"
        f'include("{module.as_posix()}")\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/state.txt" "${RK_HAVE_NCNN}\n${RK_NCNN_TARGET}\n${RK_TEST_PACKAGE_MARKER}\n")\n'
        'if(RK_HAVE_NCNN)\n'
        '  get_target_property(options ${RK_NCNN_TARGET} INTERFACE_COMPILE_OPTIONS)\n'
        '  file(WRITE "${CMAKE_BINARY_DIR}/options.txt" "${options}")\n'
        'endif()\n', encoding="utf-8")
    environment = os.environ.copy()
    environment.pop("NCNN_DIR", None)
    if environment_root is not None:
        environment["NCNN_DIR"] = str(environment_root)
    build = tmp_path / "build"
    command = [cmake, "-S", str(source), "-B", str(build)]
    if ninja:
        command.extend(["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja])
    command.extend("-D" + key + "=" + str(value) for key, value in definitions.items())
    result = subprocess.run(command, env=environment, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    state = (build / "state.txt").read_text(encoding="utf-8").splitlines()
    options = (build / "options.txt").read_text(encoding="utf-8").split(";") if state[0] == "ON" else []
    return state, options


def test_install_root_enables_ncnn(tmp_path):
    install = tmp_path / "ncnn install"
    package(install, "requested")
    state, _ = configure(tmp_path, {"RK_ENABLE_NCNN": "ON", "NCNN_DIR": install})
    assert state[0] == "ON", "NCNN_DIR 安装根未被消费"
    assert state[2] == "requested"


def test_environment_install_root_enables_ncnn(tmp_path):
    install = tmp_path / "ncnn install"
    package(install, "environment")
    state, _ = configure(tmp_path, {"RK_ENABLE_NCNN": "ON"}, environment_root=install)
    assert state[0] == "ON", "环境变量 NCNN_DIR 未被消费"
    assert state[2] == "environment"


@pytest.mark.parametrize("abi", ["armeabi-v7a", "arm64-v8a"])
def test_android_selects_current_abi_outside_sysroot(tmp_path, abi):
    armv7, arm64 = tmp_path / "armeabi-v7a", tmp_path / "arm64-v8a"
    package(armv7, "armeabi-v7a")
    package(arm64, "arm64-v8a")
    state, _ = configure(tmp_path, {
        "RK_ENABLE_NCNN": "ON", "ANDROID": "ON", "ANDROID_ABI": abi,
        "NCNN_DIR_ARMv7": armv7, "NCNN_DIR_ARM64": arm64,
        "CMAKE_FIND_ROOT_PATH": tmp_path / "sysroot", "CMAKE_FIND_ROOT_PATH_MODE_PACKAGE": "ONLY"})
    assert state[0] == "ON", "当前 ABI 的安装根未被消费"
    assert state[2] == abi, "错误使用了其他 ABI 的包"


def test_explicit_package_directory_remains_supported(tmp_path):
    directory = package(tmp_path / "ncnn", "explicit")
    state, _ = configure(tmp_path, {"RK_ENABLE_NCNN": "ON", "ncnn_DIR": directory})
    assert state[0] == "ON"
    assert state[2] == "explicit"


@pytest.mark.parametrize("target", ["ncnn", "ncnn::ncnn", "alias"])
def test_android_consumer_keeps_rtti_and_exceptions(tmp_path, target):
    directory = package(tmp_path / "ncnn", "flags", target)
    state, options = configure(tmp_path, {"RK_ENABLE_NCNN": "ON", "ANDROID": "ON", "ncnn_DIR": directory})
    assert state[0] == "ON"
    assert "-fno-rtti" not in options, "ncnn 不应禁止项目消费方使用 RTTI"
    assert "-fno-exceptions" not in options, "ncnn 不应禁止项目消费方使用异常"
    assert "-DNCNN_KEEP_OPTION=1" in options, "不能丢弃无关的接口选项"


def test_disabled_backend_does_not_load_installed_package(tmp_path):
    directory = package(tmp_path / "ncnn", "disabled")
    state, _ = configure(tmp_path, {"RK_ENABLE_NCNN": "OFF", "ncnn_DIR": directory})
    assert state[0] == "OFF"
    assert state[1] == ""


def test_non_android_package_options_are_unchanged(tmp_path):
    directory = package(tmp_path / "ncnn", "host")
    state, options = configure(tmp_path, {"RK_ENABLE_NCNN": "ON", "ncnn_DIR": directory})
    assert state[0] == "ON"
    assert options == ["-fno-rtti", "-fno-exceptions", "-DNCNN_KEEP_OPTION=1"]


def test_absent_package_keeps_backend_disabled(tmp_path):
    state, _ = configure(tmp_path, {"RK_ENABLE_NCNN": "ON", "NCNN_DIR": tmp_path / "missing"})
    assert state[0] == "OFF"
    assert state[1] == ""
