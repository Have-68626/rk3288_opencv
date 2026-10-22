"""固定构建契约不接受任意命令，也不依赖本机默认路径。"""
from pathlib import Path
import json
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from building.profiles import PROFILES, load_inputs, make_plan, validate_build_time


def fixture_inputs(tmp_path):
    paths = {}
    files = {
        'cmake': 'cmake.exe', 'ctest': 'ctest.exe', 'node': 'node.exe',
        'pnpm': 'pnpm.cmd', 'ninja': 'ninja.exe',
    }
    for key, name in files.items():
        path = tmp_path / 'tools' / name
        path.parent.mkdir(exist_ok=True)
        path.write_bytes(b'synthetic-tool')
        paths[key] = str(path)
    directories = {
        'googletest_source': ['CMakeLists.txt'],
        'opencv_root': ['CMakeLists.txt', 'modules/core/include/opencv2/core/version.hpp'],
        'opencv_contrib_root': ['modules/face/CMakeLists.txt'],
        'windows_ncnn_dir': ['lib/cmake/ncnn/ncnnConfig.cmake', 'lib/ncnn.lib'],
        'android_ncnn_dir': [path for abi in ['armeabi-v7a', 'arm64-v8a']
                             for path in (abi + '/lib/cmake/ncnn/ncnnConfig.cmake', abi + '/lib/libncnn.a')],
        'android_ndk': ['source.properties', 'build/cmake/android.toolchain.cmake'],
        'android_sdk': ['platforms/android-34/android.jar', 'cmake/3.22.1/bin/cmake.exe'],
        'java_home': ['release', 'bin/java.exe'],
        'web_dir': ['package.json', 'pnpm-lock.yaml', 'node_modules/.modules.yaml',
                    'node_modules/typescript/bin/tsc', 'node_modules/vite/bin/vite.js'],
    }
    for key, names in directories.items():
        directory = tmp_path / key
        for name in names:
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic-config', encoding='utf-8')
        paths[key] = str(directory)
    Path(paths['java_home'], 'release').write_text('JAVA_VERSION="21.0.10"', encoding='utf-8')
    Path(paths['web_dir'], 'node_modules/.modules.yaml').write_text('packageManager: pnpm@10.33.0\n', encoding='utf-8')
    return paths


def test_fixed_profiles():
    assert set(PROFILES) == {'core', 'windows', 'android-mock', 'android-real', 'android-cli', 'web', 'windows-product'}


def test_duplicate_json_fields_rejected(tmp_path):
    path = tmp_path / 'inputs.json'
    path.write_text('{"schema_version":1,"schema_version":1,"paths":{}}', encoding='utf-8')
    with pytest.raises(ValueError, match='DUPLICATE'):
        load_inputs(path)


@pytest.mark.parametrize('extra', [{'command': 'arbitrary'}, {'token': 'synthetic-private-value'}])
def test_unknown_fields_rejected(tmp_path, extra):
    path = tmp_path / 'inputs.json'
    path.write_text(json.dumps(dict(schema_version=1, paths={}, **extra)), encoding='utf-8')
    with pytest.raises(ValueError, match='INPUT_FIELDS'):
        load_inputs(path)


def test_relative_input_paths_use_manifest_directory(tmp_path):
    tool = tmp_path / 'cmake.exe'
    tool.write_bytes(b'synthetic')
    path = tmp_path / 'inputs.json'
    path.write_text(json.dumps(dict(schema_version=1, paths={'cmake': 'cmake.exe'})), encoding='utf-8')
    assert load_inputs(path)['cmake'] == str(tool.resolve())


def test_unknown_profile_rejected(tmp_path):
    with pytest.raises(ValueError, match='UNKNOWN_PROFILE'):
        make_plan(tmp_path, 'arbitrary', {}, tmp_path / 'out')


def test_missing_tools_are_explicit_and_plan_is_readonly(tmp_path):
    plan = make_plan(tmp_path, 'core', {}, tmp_path / 'out')
    assert 'MISSING_CMAKE' in plan['errors']
    assert 'MISSING_GOOGLETEST_SOURCE' in plan['errors']
    assert not (tmp_path / 'cache').exists() and not (tmp_path / 'out').exists()


def test_configuration_identity_does_not_change_for_cpp_edit(tmp_path):
    inputs = fixture_inputs(tmp_path)
    first = make_plan(tmp_path, 'core', inputs, tmp_path / 'run1')
    (tmp_path / 'main.cpp').write_text('int main(){}', encoding='utf-8')
    second = make_plan(tmp_path, 'core', inputs, tmp_path / 'run2')
    assert first['identity'] == second['identity'] and first['build_dir'] == second['build_dir']
    assert not first['errors']


def test_dependency_configuration_change_changes_identity(tmp_path):
    inputs = fixture_inputs(tmp_path)
    first = make_plan(tmp_path, 'core', inputs, tmp_path / 'run1')
    Path(inputs['googletest_source'], 'CMakeLists.txt').write_text('changed', encoding='utf-8')
    second = make_plan(tmp_path, 'core', inputs, tmp_path / 'run2')
    assert first['identity'] != second['identity']


def test_ncnn_import_metadata_change_changes_identity(tmp_path):
    inputs = fixture_inputs(tmp_path)
    metadata = Path(inputs['windows_ncnn_dir']) / 'lib/cmake/ncnn/ncnn-release.cmake'
    metadata.write_text('first-config', encoding='utf-8')
    first = make_plan(tmp_path, 'windows', inputs, tmp_path / 'first')
    metadata.write_text('changed-import-flags', encoding='utf-8')
    second = make_plan(tmp_path, 'windows', inputs, tmp_path / 'second')
    assert first['identity'] != second['identity']


@pytest.mark.parametrize('changed_abi', ('armeabi-v7a', 'arm64-v8a'))
def test_android_ncnn_static_archives_are_part_of_identity(tmp_path, changed_abi):
    inputs = fixture_inputs(tmp_path)
    for abi in ('armeabi-v7a', 'arm64-v8a'):
        archive = Path(inputs['android_ncnn_dir']) / abi / 'lib/libncnn.a'
        archive.parent.mkdir(parents=True, exist_ok=True)
        archive.write_bytes(('ncnn-' + abi).encode())
    first = make_plan(tmp_path, 'android-real', inputs, tmp_path / 'first')
    for abi in ('armeabi-v7a', 'arm64-v8a'):
        assert abi + '/lib/libncnn.a' in first['fingerprints']['android_ncnn_dir']
    archive = Path(inputs['android_ncnn_dir']) / changed_abi / 'lib/libncnn.a'
    archive.write_bytes(b'replaced-ncnn-archive')
    second = make_plan(tmp_path, 'android-real', inputs, tmp_path / 'second')
    assert first['identity'] != second['identity']


def test_missing_wrapper_distribution_is_blocked_before_any_download(tmp_path, monkeypatch):
    inputs = fixture_inputs(tmp_path)
    wrapper = tmp_path / 'gradle/wrapper/gradle-wrapper.properties'
    wrapper.parent.mkdir(parents=True)
    wrapper.write_text('distributionUrl=https\\://services.gradle.org/distributions/gradle-9.0-bin.zip\n', encoding='utf-8')
    monkeypatch.setenv('GRADLE_USER_HOME', str(tmp_path / 'missing-cache'))
    assert 'GRADLE_DISTRIBUTION_MISSING' in make_plan(tmp_path, 'android-mock', inputs, tmp_path / 'run')['errors']


def test_windows_contract_and_product_share_native_tree(tmp_path):
    inputs = fixture_inputs(tmp_path)
    normal = make_plan(tmp_path, 'windows', inputs, tmp_path / 'run1')
    product = make_plan(tmp_path, 'windows-product', inputs, tmp_path / 'run2')
    assert normal['build_dir'] == product['build_dir']
    assert normal['contract']['BUILD_SHARED_LIBS'] == 'OFF'
    assert normal['contract']['RK_ENABLE_NCNN'] == 'ON'
    assert normal['contract']['RK_COPY_DEV_MODELS'] == 'OFF'
    assert 'win_local_service' in normal['targets']
    assert 'CORE_PATCH_REQUIRED' in product['errors']


def test_android_apk_and_cli_have_distinct_stl(tmp_path):
    inputs = fixture_inputs(tmp_path)
    apk = make_plan(tmp_path, 'android-real', inputs, tmp_path / 'run1')
    cli = make_plan(tmp_path, 'android-cli', inputs, tmp_path / 'run2')
    assert apk['contract']['ANDROID_STL'] == 'c++_shared'
    assert cli['contract']['ANDROID_STL'] == 'c++_static'
    assert apk['abis'] == ['armeabi-v7a', 'arm64-v8a']
    assert cli['abis'] == ['armeabi-v7a']


def test_android_build_time_is_validated_without_forking_the_build_tree(tmp_path):
    inputs = fixture_inputs(tmp_path)
    first = make_plan(tmp_path, 'android-mock', inputs, tmp_path / 'first', build_time='20261022-1630')
    second = make_plan(tmp_path, 'android-mock', inputs, tmp_path / 'second', build_time='20261022-1631')
    assert first['build_time'] == '20261022-1630'
    assert first['build_time_explicit'] is True
    assert first['identity'] == second['identity']
    assert first['build_dir'] == second['build_dir']


@pytest.mark.parametrize('value', ['20261022-1630', '20240229-0000'])
def test_build_time_accepts_valid_local_minute_values(value):
    assert validate_build_time(value) == value


@pytest.mark.parametrize('value', ['20260229-0000', '20261022-2460', '2026102-1630', '20261022T1630'])
def test_build_time_rejects_invalid_calendar_or_shape(value):
    with pytest.raises(ValueError, match='BUILD_TIME_INVALID'):
        validate_build_time(value)


def test_wrong_jdk_and_web_dependency_versions_block(tmp_path):
    inputs = fixture_inputs(tmp_path)
    Path(inputs['java_home'], 'release').write_text('JAVA_VERSION="17.0.1"', encoding='utf-8')
    assert 'JAVA_VERSION_MISMATCH' in make_plan(tmp_path, 'android-mock', inputs, tmp_path / 'a')['errors']
    Path(inputs['web_dir'], 'node_modules/.modules.yaml').write_text('packageManager: pnpm@9.15.5', encoding='utf-8')
    assert 'WEB_DEPENDENCY_VERSION_MISMATCH' in make_plan(tmp_path, 'web', inputs, tmp_path / 'b')['errors']


def test_linked_dependency_rejected(tmp_path):
    target = tmp_path / 'real.exe'
    target.write_bytes(b'synthetic')
    link = tmp_path / 'cmake.exe'
    try:
        link.symlink_to(target)
    except OSError:
        pytest.skip('当前宿主无创建链接权限')
    path = tmp_path / 'inputs.json'
    path.write_text(json.dumps(dict(schema_version=1, paths={'cmake': str(link)})), encoding='utf-8')
    with pytest.raises(ValueError, match='LINK'):
        load_inputs(path)


def test_pnpm_managed_package_link_is_allowed_only_inside_its_store(tmp_path):
    inputs = fixture_inputs(tmp_path)
    web = Path(inputs['web_dir'])
    package = web / 'node_modules/typescript'
    old = package / 'bin/tsc'
    store = web / 'node_modules/.pnpm/typescript@5/node_modules/typescript'
    (store / 'bin').mkdir(parents=True)
    old.rename(store / 'bin/tsc')
    (package / 'bin').rmdir()
    package.rmdir()
    try:
        package.symlink_to(store, target_is_directory=True)
    except OSError:
        pytest.skip('当前宿主无创建链接权限')
    plan = make_plan(tmp_path, 'web', inputs, tmp_path / 'out')
    assert 'node_modules/typescript/bin/tsc' in plan['fingerprints']['web_dir']
