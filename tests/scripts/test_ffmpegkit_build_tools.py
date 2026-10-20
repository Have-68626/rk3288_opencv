"""合成输入验证构建工具拒绝边界，不联网、不访问设备。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import subprocess
import zipfile

import pytest

from android_tool_test_support import ROOT
from test_apk_native_inspection import ABIS, elf


def load(name):
    path = ROOT / 'scripts/ffmpegkit' / name
    assert path.exists(), '缺少正式FFmpegKit构建入口：' + name
    sys.path.insert(0, str(path.parent))
    spec = importlib.util.spec_from_file_location('ffmpegkit_' + name.replace('-', '_').replace('.', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source(tmp_path):
    root = tmp_path / 'source'
    rows = {
        'android.sh': 'BUILD_VERSION=$(git describe --tags --always 2>>"${BASEDIR}"/build.log)\n',
        'scripts/function.sh': 'echo $(nproc)\nenv\n',
        'scripts/android/cpu-features.sh': '$(android_ndk_cmake) || return 1\n',
        'android/jni/Android.mk': 'MY_ARM_NEON := false\nMY_ARM_NEON := false\n',
    }
    for rel, text in rows.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf8')
    return root


def test_prepare_patches_known_inputs_once(tmp_path):
    tool = load('prepare-native.py')
    root = source(tmp_path)
    result = tool.patch_source(root, 6)
    assert result['environment_dumps_removed'] == 1
    assert 'echo 6' in (root / 'scripts/function.sh').read_text(encoding='utf-8')
    assert 'MY_ARM_NEON := true' in (root / 'android/jni/Android.mk').read_text(encoding='utf-8')
    with pytest.raises(ValueError):
        tool.patch_source(root, 6)


def test_prepare_rejects_patch_drift_before_any_write(tmp_path):
    tool = load('prepare-native.py')
    root = source(tmp_path)
    (root / 'android/jni/Android.mk').write_text('unknown')
    before = (root / 'android.sh').read_bytes()
    with pytest.raises(ValueError):
        tool.patch_source(root, 6)
    assert (root / 'android.sh').read_bytes() == before


@pytest.mark.parametrize('unsafe', ['../escape', '/absolute', 'C:/outside', 'dir/../../escape'])
def test_archive_traversal_is_rejected(tmp_path, unsafe):
    tool = load('_common.py')
    archive = tmp_path / 'input.zip'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('safe/file', 'keep')
        z.writestr(unsafe, 'reject')
    with pytest.raises(ValueError):
        tool.extract_zip(archive, tmp_path / 'out', hashlib.sha256(archive.read_bytes()).hexdigest())
    assert not (tmp_path / 'out').exists()


def test_archive_digest_mismatch_does_not_extract(tmp_path):
    tool = load('_common.py')
    archive = tmp_path / 'input.zip'
    with zipfile.ZipFile(archive, 'w') as z:
        z.writestr('file', 'data')
    with pytest.raises(ValueError):
        tool.extract_zip(archive, tmp_path / 'out', '0' * 64)
    assert not (tmp_path / 'out').exists()


def java_jar(path, major=52):
    with zipfile.ZipFile(path, 'w') as z:
        for name in ['com/arthenica/ffmpegkit/FFmpegKit.class', 'com/arthenica/smartexception/AbstractExceptions.class']:
            z.writestr(name, b'\xca\xfe\xba\xbe' + struct.pack('>HH', 0, major))
    return path


@pytest.mark.parametrize('major,valid', [(52, True), (61, False)])
def test_java_class_contract(tmp_path, major, valid):
    tool = load('build-java.py')
    jar = java_jar(tmp_path / 'classes.jar', major)
    if valid:
        assert tool.inspect_classes(jar)['classes'] == 2
    else:
        with pytest.raises(ValueError):
            tool.inspect_classes(jar)


def native(tmp_path):
    libraries = tmp_path / 'libs'
    for abi, (bits, machine) in ABIS.items():
        directory = libraries / abi
        directory.mkdir(parents=True)
        for name in ['libffmpegkit.so', 'libffmpegkit_abidetect.so', 'libavcodec.so', 'libavformat.so']:
            (directory / name).write_bytes(elf(bits, machine))
    resources = tmp_path / 'res'
    (resources / 'raw').mkdir(parents=True)
    (resources / 'raw/license.txt').write_text('fixture-license')
    rules = tmp_path / 'consumer-rules.pro'
    rules.write_text('-keep class com.arthenica.** { *; }')
    license_path = tmp_path / 'smart-exception-LICENSE.txt'
    license_path.write_text('fixture-license')
    jar = java_jar(tmp_path / 'classes.jar')
    return libraries, jar, resources, rules, [license_path], tmp_path / 'output.aar', {abi: ['libc.so'] for abi in ABIS}


def test_bundle_keeps_two_abis_and_does_not_claim_runtime(tmp_path):
    tool = load('bundle-aar.py')
    args = native(tmp_path)
    report = tool.bundle(*args)
    assert report['runtime_verified'] is False
    assert report['min_sdk'] == 21
    assert len(report['native_libraries']) == 8
    with zipfile.ZipFile(args[5]) as z:
        assert z.testzip() is None
        assert 'classes.jar' in z.namelist()
        assert 'res/raw/license.txt' in z.namelist()


@pytest.mark.parametrize('mutation', ['abi', 'dependency', 'textrel', 'system_stub', 'extra_abi'])
def test_bundle_rejects_native_mutations_before_output(tmp_path, mutation):
    tool = load('bundle-aar.py')
    args = native(tmp_path)
    target = args[0] / 'arm64-v8a/libavcodec.so'
    if mutation == 'abi':
        target.write_bytes(elf(1, 40))
    elif mutation == 'dependency':
        target.write_bytes(elf(2, 183, needed=('libmissing.so',)))
    elif mutation == 'textrel':
        target.write_bytes(elf(2, 183, textrel=True))
    elif mutation == 'system_stub':
        (args[0] / 'arm64-v8a/libc.so').write_bytes(elf(2, 183))
    else:
        (args[0] / 'x86').mkdir()
    with pytest.raises(ValueError):
        tool.bundle(*args)
    assert not args[5].exists()


def test_bundle_refuses_existing_output(tmp_path):
    tool = load('bundle-aar.py')
    args = native(tmp_path)
    args[5].write_bytes(b'preserve')
    with pytest.raises(ValueError):
        tool.bundle(*args)
    assert args[5].read_bytes() == b'preserve'


def test_bundle_cli_rejects_report_output_collision_before_writing(tmp_path, monkeypatch):
    tool = load('bundle-aar.py')
    args = native(tmp_path)
    platform = tmp_path / 'platform.json'
    platform.write_text(json.dumps(args[6]), encoding='utf8')
    monkeypatch.setattr(sys, 'argv', ['bundle-aar.py', '--native-libs', str(args[0]),
        '--classes-jar', str(args[1]), '--resources', str(args[2]), '--consumer-rules', str(args[3]),
        '--licenses', str(args[4][0]), '--output', str(args[5]), '--report', str(args[5]),
        '--platform-libraries', str(platform)])
    with pytest.raises(ValueError):
        tool.main()
    assert not args[5].exists()


def test_collect_rejects_one_abi_missing_build_option(tmp_path):
    tool = load('collect-native.py')
    args = native(tmp_path)
    for abi in ABIS:
        for name in ['libavcodec.so', 'libavformat.so']:
            with (args[0] / abi / name).open('ab') as out:
                out.write(b'--enable-libx264 --enable-openssl --enable-gpl --enable-version3')
    (args[0] / 'arm64-v8a/libavcodec.so').write_bytes(elf(2, 183))
    with pytest.raises(ValueError):
        tool.check_capabilities(args[0])


def test_dependency_commit_drift_is_rejected_before_extraction(tmp_path):
    tool = load('prepare-native.py')
    repo = tmp_path / 'mirror'
    repo.mkdir()
    for command in [['git', 'init', '--quiet', str(repo)],
                    ['git', '-C', str(repo), '-c', 'user.name=fixture', '-c', 'user.email=fixture@invalid',
                     '-c', 'core.hooksPath=' + os.devnull, 'commit', '--quiet', '--allow-empty', '-m', 'fixture']]:
        subprocess.run(command, check=True, capture_output=True)
    mirrors = {name: str(repo) for name in tool.lock()['sources']}
    with pytest.raises(ValueError, match='DEPENDENCY_COMMIT_MISMATCH'):
        tool.prepare(tmp_path / 'absent.zip', tmp_path / 'out', mirrors, 6)
    assert not (tmp_path / 'out').exists()


def test_root_junction_is_rejected(tmp_path):
    if os.name != 'nt':
        pytest.skip('Windows重解析点边界')
    tool = load('_common.py')
    source = tmp_path / 'real'
    source.mkdir()
    (source / 'file').write_text('fixture')
    link = tmp_path / 'link'
    script = tmp_path / 'junction.ps1'
    script.write_text('param($Link,$Target) New-Item -ItemType Junction -Path $Link -Target $Target -ErrorAction Stop | Out-Null')
    subprocess.run(['pwsh', '-NoProfile', '-File', str(script), str(link), str(source)], check=True, capture_output=True)
    assert link.is_junction()
    with pytest.raises(ValueError):
        tool.regular_files(link)


@pytest.mark.parametrize('line', ['Authorization: Bearer fixture-credential',
                                 'password="fixture credential"', 'https://fixture-user:fixture-pass@example.invalid/path'])
def test_redaction_does_not_leave_credential_fragments(line):
    tool = load('_common.py')
    result = tool.redact(line)
    assert 'fixture' not in result and 'credential' not in result
