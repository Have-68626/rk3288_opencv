"""发布件由本轮输入产生，旧目录和被改写清单不能冒充新产物。"""
from pathlib import Path
import copy
import hashlib
import json
import os
import shutil
import struct
import sys
import zipfile
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import building.coordinator as coordinator
from building.artifacts import inventory, check_cache, check_android_cache, assemble_product, verify_manifest, pe_imports
from building.profiles import make_plan
from test_build_profiles import fixture_inputs


def pe_file(path, dependency='KERNEL32.dll'):
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<H', data, 148, 240)
    struct.pack_into('<H', data, 152, 0x20b)
    struct.pack_into('<I', data, 260, 16)
    struct.pack_into('<II', data, 272, 0x1000, 40)
    struct.pack_into('<IIII', data, 400, 1536, 0x1000, 1536, 512)
    struct.pack_into('<I', data, 524, 0x1080)
    raw = dependency.encode('ascii') + b'\0'
    data[640:640 + len(raw)] = raw
    path.write_bytes(data)
    return path


def product_fixture(tmp_path):
    root = tmp_path / 'repo'
    root.mkdir()
    for name in ['app/src/main/assets/lbpcascade_frontalface.xml', 'config/windows_camera_face_recognition.ini']:
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('synthetic-public-asset', encoding='utf-8')
    web = tmp_path / 'web-output'
    (web / 'assets').mkdir(parents=True)
    (web / 'index.html').write_text('<html><script src="./assets/app-new.js"></script></html>', encoding='utf-8')
    (web / 'assets/app-new.js').write_text('console.log(1)', encoding='utf-8')
    executable = pe_file(tmp_path / 'service.exe')
    plan = dict(root=str(root), identity='b' * 64)
    return plan, executable, web, tmp_path / 'product'


def android_plan(tmp_path, profile, run_id, marker):
    build = tmp_path / 'builds' / profile
    apk = build / 'app/build/outputs/apk/debug/app-debug.apk'
    apk.parent.mkdir(parents=True)
    payloads = {}
    with zipfile.ZipFile(apk, 'w') as archive:
        for abi in ('armeabi-v7a', 'arm64-v8a'):
            member = f'lib/{abi}/libnative-lib.so'
            payloads[member] = f'{marker}-{abi}'.encode('ascii')
            archive.writestr(member, payloads[member])
    run_dir = tmp_path / 'cache/reports/build-coordination' / run_id
    plan = dict(root=str(tmp_path), run_dir=str(run_dir), build_dir=str(build),
                profile=profile, canonical=profile, identity='a' * 64,
                paths={'android_ncnn_dir': str(tmp_path / 'ncnn')},
                abis=['armeabi-v7a', 'arm64-v8a'])
    return plan, payloads


def test_inventory_rejects_links_and_does_not_follow_them(tmp_path):
    target = tmp_path / 'outside'
    target.mkdir()
    (target / 'private.bin').write_bytes(b'synthetic')
    root = tmp_path / 'root'
    root.mkdir()
    try:
        (root / 'link').symlink_to(target, target_is_directory=True)
    except OSError:
        pytest.skip('当前宿主无创建链接权限')
    with pytest.raises(ValueError, match='LINK'):
        inventory(root)


def test_cache_contract_accepts_only_same_source_generator_and_flags(tmp_path):
    plan = make_plan(tmp_path, 'core', fixture_inputs(tmp_path), tmp_path / 'run')
    build = Path(plan['build_dir'])
    build.mkdir(parents=True)
    values = dict(plan['contract'], CMAKE_HOME_DIRECTORY=str(tmp_path), FETCHCONTENT_SOURCE_DIR_GOOGLETEST=plan['paths']['googletest_source'])
    text = '\n'.join(f'{key}:STRING={value}' for key, value in values.items())
    (build / 'CMakeCache.txt').write_text(text, encoding='utf-8')
    check_cache(plan)
    for key in ['CMAKE_HOME_DIRECTORY', 'CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_MSVC_RUNTIME_LIBRARY', 'RK_SKIP_OPENCV']:
        changed = dict(values, **{key: 'wrong'})
        (build / 'CMakeCache.txt').write_text('\n'.join(f'{k}:STRING={v}' for k, v in changed.items()), encoding='utf-8')
        with pytest.raises(ValueError, match='CACHE_CONTRACT'):
            check_cache(plan)


def test_dependency_file_drift_rejected(tmp_path):
    inputs = fixture_inputs(tmp_path)
    plan = make_plan(tmp_path, 'core', inputs, tmp_path / 'run')
    Path(inputs['cmake']).write_bytes(b'drift')
    with pytest.raises(ValueError, match='INPUT_CHANGED'):
        check_cache(plan, require_cache=False)


def test_effective_cache_dependencies_must_match_planned_paths(tmp_path):
    inputs = fixture_inputs(tmp_path)
    plan = make_plan(tmp_path, 'windows', inputs, tmp_path / 'run')
    build = Path(plan['build_dir'])
    build.mkdir(parents=True)
    values = dict(plan['contract'], CMAKE_HOME_DIRECTORY=str(tmp_path),
                  FETCHCONTENT_SOURCE_DIR_GOOGLETEST=inputs['googletest_source'],
                  OPENCV_ROOT=inputs['opencv_root'], OPENCV_CONTRIB_ROOT=inputs['opencv_contrib_root'],
                  ncnn_DIR=str(Path(inputs['windows_ncnn_dir']) / 'lib/cmake/ncnn'))
    cache = build / 'CMakeCache.txt'
    cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
    check_cache(plan)
    values['ncnn_DIR'] = str(tmp_path / 'wrong-ncnn')
    cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
    with pytest.raises(ValueError, match='CACHE_CONTRACT'):
        check_cache(plan)


def test_android_configure_requires_actual_both_abi_contracts(tmp_path):
    inputs = fixture_inputs(tmp_path)
    plan = make_plan(tmp_path, 'android-real', inputs, tmp_path / 'run')
    cmake = Path(inputs['android_sdk']) / 'cmake/3.22.1/bin/cmake.exe'
    Path(inputs['android_ndk'], 'source.properties').write_text(
        'Pkg.Desc = Android NDK\nPkg.Revision = 27.0.12077973\n', encoding='utf-8')
    with pytest.raises(ValueError, match='ANDROID_CACHE'):
        check_android_cache(plan)
    for abi in plan['abis']:
        cache = Path(plan['build_dir']) / 'app/.cxx/Debug/config' / abi / 'CMakeCache.txt'
        cache.parent.mkdir(parents=True)
        values = dict(CMAKE_HOME_DIRECTORY=str(tmp_path), ANDROID_ABI=abi, ANDROID_STL='c++_shared',
                      CMAKE_ANDROID_NDK=inputs['android_ndk'], RK_SKIP_OPENCV='OFF', RK_ENABLE_NCNN='ON',
                      CMAKE_COMMAND=str(cmake),
                      OPENCV_ROOT=inputs['opencv_root'], OPENCV_CONTRIB_ROOT=inputs['opencv_contrib_root'],
                      RK_ENABLE_LIBYUV='OFF',
                      ncnn_DIR=str(Path(inputs['android_ncnn_dir']) / abi / 'lib/cmake/ncnn'))
        cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
    toolchain = check_android_cache(plan)
    assert toolchain == {'abis': {abi: {'ndk_path': inputs['android_ndk'], 'ndk_version': '27.0.12077973',
                                        'cmake_executable': str(cmake)}
                                   for abi in plan['abis']}}
    values['CMAKE_COMMAND'] = str(tmp_path / 'wrong-cmake.exe')
    cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
    with pytest.raises(ValueError, match='ANDROID_CACHE'):
        check_android_cache(plan)
    values['CMAKE_COMMAND'] = str(cmake)
    for key, wrong in [('OPENCV_ROOT', str(tmp_path / 'wrong-opencv')),
                       ('OPENCV_CONTRIB_ROOT', str(tmp_path / 'wrong-contrib')),
                       ('RK_ENABLE_LIBYUV', 'ON')]:
        values[key] = wrong
        cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
        with pytest.raises(ValueError, match='ANDROID_CACHE'):
            check_android_cache(plan)
        values[key] = inputs[key.lower()] if key.startswith('OPENCV_') else 'OFF'
    cache.write_text('\n'.join(f'{k}:STRING={v}' for k, v in values.items()), encoding='utf-8')
    cache.write_text(cache.read_text(encoding='utf-8').replace('c++_shared', 'c++_static'), encoding='utf-8')
    with pytest.raises(ValueError, match='ANDROID_CACHE'):
        check_android_cache(plan)


@pytest.mark.parametrize('changed_abi', ('armeabi-v7a', 'arm64-v8a'))
def test_android_ncnn_archive_drift_rejects_existing_plan(tmp_path, changed_abi):
    inputs = fixture_inputs(tmp_path)
    for abi in ('armeabi-v7a', 'arm64-v8a'):
        archive = Path(inputs['android_ncnn_dir']) / abi / 'lib/libncnn.a'
        archive.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(('ncnn-' + abi).encode())
    plan = make_plan(tmp_path, 'android-real', inputs, tmp_path / 'run')
    archive = Path(inputs['android_ncnn_dir']) / changed_abi / 'lib/libncnn.a'
    archive.write_bytes(b'replaced-ncnn-archive')
    with pytest.raises(ValueError, match='INPUT_CHANGED'):
        check_cache(plan, require_cache=False)


def test_pe_imports_and_non_system_dependency_rejected(tmp_path):
    plan, exe, web, dest = product_fixture(tmp_path)
    assert pe_imports(exe) == ['KERNEL32.dll']
    pe_file(exe, 'unaccounted-synthetic.dll')
    with pytest.raises(ValueError, match='RUNTIME_DEPENDENCY'):
        assemble_product(plan, exe, web, dest)
    assert not dest.exists()


@pytest.mark.parametrize('data', [b'', b'MZ', b'not-a-program'])
def test_invalid_pe_rejected(tmp_path, data):
    exe = tmp_path / 'bad.exe'
    exe.write_bytes(data)
    with pytest.raises(ValueError, match='INVALID_PE'):
        pe_imports(exe)


def test_product_manifest_is_bound_and_extra_files_rejected(tmp_path):
    plan, exe, web, dest = product_fixture(tmp_path)
    manifest = assemble_product(plan, exe, web, dest)
    verify_manifest(manifest)
    assert set(manifest['files']) == {'win_local_service.exe', 'webroot/index.html', 'webroot/assets/app-new.js',
                                      'assets/lbpcascade_frontalface.xml', 'config/windows_camera_face_recognition.ini'}
    (dest / 'webroot/assets/old.js').write_text('stale', encoding='utf-8')
    with pytest.raises(ValueError, match='ARTIFACT_SET'):
        verify_manifest(manifest)


def test_modified_product_cannot_be_resigned_by_verify(tmp_path):
    plan, exe, web, dest = product_fixture(tmp_path)
    manifest = assemble_product(plan, exe, web, dest)
    before = copy.deepcopy(manifest)
    (dest / 'webroot/index.html').write_text('modified', encoding='utf-8')
    with pytest.raises(ValueError, match='ARTIFACT_CHANGED'):
        verify_manifest(manifest)
    assert manifest == before


@pytest.mark.parametrize('html', ['', '<html><script src="missing.js"></script></html>',
                                  '<html><script src="../../outside.js"></script></html>'])
def test_missing_or_escaping_web_resources_rejected(tmp_path, html):
    plan, exe, web, dest = product_fixture(tmp_path)
    (web / 'index.html').write_text(html, encoding='utf-8')
    with pytest.raises(ValueError, match='WEB_'):
        assemble_product(plan, exe, web, dest)


def test_existing_output_is_not_modified(tmp_path):
    plan, exe, web, dest = product_fixture(tmp_path)
    dest.mkdir()
    (dest / 'keep.txt').write_text('keep', encoding='utf-8')
    with pytest.raises(ValueError, match='OUTPUT_EXISTS'):
        assemble_product(plan, exe, web, dest)
    assert (dest / 'keep.txt').read_text(encoding='utf-8') == 'keep'


def test_copy_time_source_change_cannot_pass(monkeypatch, tmp_path):
    import building.artifacts as module
    plan, exe, web, dest = product_fixture(tmp_path)
    original = shutil.copyfile
    def change(source, target):
        value = original(source, target)
        if Path(source).name == 'app-new.js':
            Path(source).write_text('changed-during-copy', encoding='utf-8')
        return value
    monkeypatch.setattr(module.shutil, 'copyfile', change)
    with pytest.raises(ValueError, match='SOURCE_CHANGED'):
        assemble_product(plan, exe, web, dest)


def test_second_product_gets_new_web_even_with_same_executable(tmp_path):
    plan, exe, web, dest = product_fixture(tmp_path)
    first = assemble_product(plan, exe, web, dest)
    (web / 'index.html').write_text('<html><script src="assets/app-next.js"></script></html>', encoding='utf-8')
    (web / 'assets/app-new.js').unlink()
    (web / 'assets/app-next.js').write_text('new', encoding='utf-8')
    second = assemble_product(plan, exe, web, tmp_path / 'second')
    assert first['files']['win_local_service.exe']['sha256'] == second['files']['win_local_service.exe']['sha256']
    assert 'webroot/assets/app-next.js' in second['files'] and 'webroot/assets/app-new.js' not in second['files']
    verify_manifest(first)


@pytest.mark.skipif(os.name != 'nt' or not shutil.which('cmake'), reason='需要Windows CMake/VS2022')
def test_real_post_build_leaves_old_hash_but_new_assembly_has_exact_set(tmp_path):
    from testing.runner import Step, execute_step
    plan, _, web, dest = product_fixture(tmp_path)
    source, build = tmp_path / 'cpp', tmp_path / 'build'
    source.mkdir()
    (source / 'main.cpp').write_text('int main(){return 0;}\n', encoding='utf-8')
    (source / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.22)\nproject(PostBuildFixture LANGUAGES CXX)\n'
        'set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)\nadd_executable(service main.cpp)\n'
        'add_custom_command(TARGET service POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory '
        '"' + web.as_posix() + '" "$<TARGET_FILE_DIR:service>/webroot")\n', encoding='utf-8')
    def command(name, args):
        result = execute_step(Step(name, [shutil.which('cmake'), *args], tmp_path, timeout=240), tmp_path / name)
        assert result['status'] == 'passed'
    command('configure', ['-S', str(source), '-B', str(build), '-G', 'Visual Studio 17 2022', '-A', 'x64'])
    command('first', ['--build', str(build), '--config', 'Release'])
    exe = build / 'Release/service.exe'
    first = assemble_product(plan, exe, web, dest)
    original_exe = exe.read_bytes()
    original_mtime = exe.stat().st_mtime_ns
    (web / 'index.html').write_text('<html><script src="assets/next.js"></script></html>', encoding='utf-8')
    (web / 'assets/app-new.js').unlink()
    (web / 'assets/next.js').write_text('new-page', encoding='utf-8')
    command('second', ['--build', str(build), '--config', 'Release'])
    assert exe.read_bytes() == original_exe and exe.stat().st_mtime_ns == original_mtime
    assert (build / 'Release/webroot/index.html').read_bytes() == (web / 'index.html').read_bytes()
    assert (build / 'Release/webroot/assets/app-new.js').is_file()
    assert (build / 'Release/webroot/assets/next.js').is_file()
    second = assemble_product(plan, exe, web, tmp_path / 'new-product')
    assert (tmp_path / 'new-product/webroot/index.html').read_bytes() == (web / 'index.html').read_bytes()
    assert not (tmp_path / 'new-product/webroot/assets/app-new.js').exists()
    assert first['files']['win_local_service.exe'] == second['files']['win_local_service.exe']


@pytest.mark.parametrize('profile', ('android-mock', 'android-real'))
def test_android_snapshot_binds_run_profile_and_each_native_library(tmp_path, monkeypatch, profile):
    plan, payloads = android_plan(tmp_path, profile, 'run-' + profile, profile)
    if profile == 'android-real':
        monkeypatch.setattr(coordinator, 'verify_android_build', lambda *args: {
            abi: dict(package_sha256=hashlib.sha256(payloads[f'lib/{abi}/libnative-lib.so']).hexdigest(),
                      text_sha256='b' * 64, build_sha256='e' * 64, loadable_sha256='f' * 64,
                      cmake_sha256='c' * 64, ninja_sha256='d' * 64,
                      scope='构建装配绑定，非设备有效性')
            for abi in ('armeabi-v7a', 'arm64-v8a')})

    manifest = coordinator.record_outputs(plan)

    assert manifest['kind'] == 'android-apk'
    assert manifest['profile'] == profile
    assert manifest['run_id'] == 'run-' + profile
    assert manifest['native_libraries'] == {
        member: dict(sha256=hashlib.sha256(payload).hexdigest(), size=len(payload))
        for member, payload in payloads.items()
    }
    assert verify_manifest(manifest)

    stale_libraries = copy.deepcopy(manifest)
    stale_libraries['native_libraries']['lib/armeabi-v7a/libnative-lib.so']['sha256'] = '0' * 64
    with pytest.raises(ValueError, match='ANDROID_LIBRARY'):
        verify_manifest(stale_libraries)
    if profile == 'android-real':
        stale_binding = copy.deepcopy(manifest)
        stale_binding['android_build_binding']['armeabi-v7a']['package_sha256'] = '0' * 64
        with pytest.raises(ValueError, match='ANDROID_BUILD_BINDING'):
            verify_manifest(stale_binding)


def test_android_channel_snapshots_do_not_overwrite_each_other(tmp_path, monkeypatch):
    plans, payloads = {}, {}
    for profile in ('android-mock', 'android-real'):
        plans[profile], payloads[profile] = android_plan(tmp_path, profile, 'run-' + profile, profile)
    monkeypatch.setattr(coordinator, 'verify_android_build', lambda *args: {
        abi: dict(package_sha256=hashlib.sha256(payloads['android-real'][f'lib/{abi}/libnative-lib.so']).hexdigest(),
                  text_sha256='b' * 64, build_sha256='e' * 64, loadable_sha256='f' * 64,
                  cmake_sha256='c' * 64, ninja_sha256='d' * 64,
                  scope='构建装配绑定，非设备有效性')
        for abi in ('armeabi-v7a', 'arm64-v8a')})

    manifests = {profile: coordinator.record_outputs(plan) for profile, plan in plans.items()}

    assert manifests['android-mock']['directory'] != manifests['android-real']['directory']
    for profile, manifest in manifests.items():
        assert verify_manifest(manifest)
        for member, payload in payloads[profile].items():
            assert manifest['native_libraries'][member]['sha256'] == hashlib.sha256(payload).hexdigest()
