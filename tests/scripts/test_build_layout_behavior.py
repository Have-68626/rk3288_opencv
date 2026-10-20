"""以原生命令替身运行批处理，验证工作目录、输出边界和失败传播。"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = Path(os.environ.get("RK_LAYOUT_SCRIPTS_ROOT", ROOT / "scripts"))
pytestmark = pytest.mark.skipif(os.name != "nt", reason="批处理入口仅在Windows验证")


@pytest.fixture(scope="module")
def command_exe(tmp_path_factory):
    directory = tmp_path_factory.mktemp("native-command")
    source = directory / "Command.cs"
    executable = directory / "command.exe"
    source.write_text(r'''using System;
using System.IO;
class Command {
    public static int Main(string[] args) {
        string name = Path.GetFileNameWithoutExtension(Environment.GetCommandLineArgs()[0]);
        File.AppendAllText(Environment.GetEnvironmentVariable("RK_LAYOUT_RECORD"),
            name + "\\t" + Environment.CurrentDirectory + "\\t" + String.Join("\\t", args) + "\\n");
        string phase = name == "cmake" ? (args.Length > 0 && args[0] == "--build" ? "BUILD" : "CONFIGURE") : "VERIFY";
        if (phase == "VERIFY") Console.WriteLine("VERIFY_SUMMARY fixture=1");
        int code;
        return Int32.TryParse(Environment.GetEnvironmentVariable("RK_LAYOUT_" + phase + "_EXIT"), out code) ? code : 0;
    }
}
'''.replace("\\\\t", "\\t").replace("\\\\n", "\\n"), encoding="utf-8")
    environment = os.environ.copy()
    environment.update(RK_FAKE_SOURCE=str(source), RK_FAKE_EXE=str(executable))
    result = subprocess.run(["powershell", "-NoProfile", "-Command",
        "$ErrorActionPreference='Stop'; Add-Type -Path $env:RK_FAKE_SOURCE -OutputAssembly $env:RK_FAKE_EXE -OutputType ConsoleApplication"],
        env=environment, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
    assert result.returncode == 0, result.stderr
    return executable


def run_batch(tmp_path, command_exe, name, arguments=(), **exits):
    repository = tmp_path / "fixture repo"
    (repository / "scripts").mkdir(parents=True)
    script = repository / "scripts" / name
    # 本组验证路径语义，先将夹具副本固定为cmd的CRLF行尾。
    script.write_bytes((SCRIPTS / name).read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
    for relative in ["tests/data/lena.bmp", "tests/data/manifest.csv", "tests/data/lbpcascade_frontalface.xml",
                     "tests/fixtures/opencv/face-parameters.csv", "CMakeLists.txt"]:
        path = repository / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("fixture", encoding="ascii")
    sdk = tmp_path / "fixture sdk"
    cmake = sdk / "cmake/3.22.1/bin/cmake.exe"
    cmake.parent.mkdir(parents=True)
    shutil.copyfile(command_exe, cmake)
    (sdk / "cmake/3.22.1/bin/ninja.exe").write_bytes(b"")
    ndk = sdk / "ndk/fixture"
    ndk.mkdir(parents=True)
    opencv = tmp_path / "opencv source"
    opencv.mkdir()
    binary = repository / "cache/builds/windows-opencv-vs2022-x64/bin/Release/opencv_verify_cli.exe"
    binary.parent.mkdir(parents=True)
    shutil.copyfile(command_exe, binary)
    caller = tmp_path / "caller with space"
    caller.mkdir()
    (caller / "images").mkdir()
    (caller / "images/manifest.csv").write_text("fixture", encoding="ascii")
    record = tmp_path / "record.txt"
    environment = os.environ.copy()
    environment.update(ANDROID_SDK_ROOT=str(sdk), ANDROID_HOME=str(sdk), ANDROID_NDK_HOME=str(ndk),
        OPENCV_ROOT=str(opencv), OPENCV_CONTRIB_ROOT=str(opencv), RK_LAYOUT_RECORD=str(record),
        RK_TEST_SCRIPT=str(script))
    environment.update({"RK_LAYOUT_" + key.upper() + "_EXIT": str(value) for key, value in exits.items()})
    command = subprocess.list2cmdline([os.environ.get("COMSPEC", "cmd.exe")]) + ' /d /s /c ""' + str(script) + '"'
    command += ''.join(' "' + arg + '"' for arg in arguments) + '"'
    result = subprocess.run(command, cwd=caller, env=environment, capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=30)
    calls = [line.split("\t") for line in record.read_text(encoding="utf-8-sig").splitlines()] if record.exists() else []
    return result, repository, caller, calls


@pytest.mark.parametrize("name,build", [
    ("build_android.bat", "android-opencv-ninja-armeabi-v7a"),
    ("verify_opencv_host.bat", "windows-opencv-vs2022-x64"),
    ("verify_faces_test_set01.bat", "windows-opencv-vs2022-x64"),
])
def test_batch_build_resolves_source_and_binary_from_other_cwd(tmp_path, command_exe, name, build):
    result, root, caller, calls = run_batch(tmp_path, command_exe, name)
    configure = next((call[2:] for call in calls if call[0] == "cmake" and "--build" not in call), [])
    assert "-S" in configure and "-B" in configure, "配置必须显式指定source和binary根"
    assert Path(configure[configure.index("-S") + 1]) == root
    assert Path(configure[configure.index("-B") + 1]) == root / "cache/builds" / build
    assert result.returncode == 0, result.stdout + result.stderr
    assert not any(p.name.startswith("build_") for p in caller.iterdir())


def test_host_validation_keeps_outputs_out_of_repository_root(tmp_path, command_exe):
    result, root, _, calls = run_batch(tmp_path, command_exe, "verify_opencv_host.bat")
    assert result.returncode == 0, result.stdout + result.stderr
    verify = next(call[2:] for call in calls if call[0] == "opencv_verify_cli")
    assert Path(verify[verify.index("--out") + 1]).parent == root / "cache/reports/opencv-host"
    assert not (root / "verify.log").exists()


def test_host_failure_is_not_hidden_by_summary_marker(tmp_path, command_exe):
    result, _, _, _ = run_batch(tmp_path, command_exe, "verify_opencv_host.bat", verify=7)
    assert result.returncode == 7, "带SUMMARY的失败仍须传播真实退出码"
    assert "[PASS]" not in result.stdout


@pytest.mark.parametrize("name", ["build_android.bat", "verify_opencv_host.bat", "verify_faces_test_set01.bat"])
def test_configure_failure_prevents_build_and_verification(tmp_path, command_exe, name):
    result, _, _, calls = run_batch(tmp_path, command_exe, name, configure=13)
    assert result.returncode != 0
    assert not any("--build" in call for call in calls)
    assert not any(call[0] == "opencv_verify_cli" for call in calls)


def test_model_report_default_uses_local_report_directory(tmp_path):
    root = tmp_path / "model repo"
    script = root / "scripts/verify_face_models.py"
    script.parent.mkdir(parents=True)
    shutil.copyfile(SCRIPTS / "verify_face_models.py", script)
    result = subprocess.run([sys.executable, str(script)], cwd=tmp_path, capture_output=True, text=True,
        encoding="utf-8", errors="replace", timeout=20)
    assert result.returncode == 1
    assert (root / "cache/reports/face-models/face_models_verify.md").is_file()
    assert not (root / "build").exists()


@pytest.mark.parametrize("entry", ["build_android.bat", "verify_opencv_host.bat", "verify_faces_test_set01.bat", "android-preset"])
def test_verification_module_selection_satisfies_link_contract(entry):
    import re
    cmake = (ROOT / "cmake/opencv.cmake").read_text(encoding="utf-8")
    match = re.search(r"set\(RK_OPENCV_FULL_LIBS(.*?)\)", cmake, re.S)
    assert match, "无法定位真实OpenCV链接契约"
    required = {name.removeprefix("opencv_") for name in match.group(1).split()}
    if entry == "android-preset":
        presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        value = next(p for p in presets["configurePresets"] if p["name"] == "android")["cacheVariables"].get("BUILD_LIST")
    else:
        source = (SCRIPTS / entry).read_text(encoding="utf-8")
        flag = re.search(r"-DBUILD_LIST=([^\s\"]+)", source)
        value = flag.group(1) if flag else None
    assert value is None or required <= set(value.split(",")), "验证入口裁掉了目标实际链接的模块"


@pytest.mark.parametrize("mode", ["default", "grid"])
def test_face_manifest_remains_one_argument_from_spaced_caller(tmp_path, command_exe, mode):
    result, root, caller, calls = run_batch(tmp_path, command_exe, "verify_faces_test_set01.bat",
        arguments=("images", "", "", mode))
    assert result.returncode == 0, result.stdout + result.stderr
    verify = next(call[2:] for call in calls if call[0] == "opencv_verify_cli")
    assert verify[verify.index("--input") + 1] == str(caller / "images")
    assert verify[verify.index("--manifest") + 1] == str(caller / "images/manifest.csv")
    if mode == "grid":
        assert Path(verify[verify.index("--param-list") + 1]) == root / "tests/fixtures/opencv/face-parameters.csv"
