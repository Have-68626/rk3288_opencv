"""B05 受控增量测量：只扰动 mtime，判据是重编集合与产物正确性。"""
import hashlib
import shutil
import subprocess
import sys
import uuid
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from building import incremental
from building.coordinator import command_steps
from testing.runner import Step, execute_step

ROOT = Path(__file__).resolve().parents[2]


def _plan(tmp_path, profile='core'):
    canonical = 'windows' if profile == 'windows-product' else profile
    return dict(schema_version=1, profile=profile, canonical=canonical, root=str(ROOT),
                paths=dict(cmake=shutil.which('cmake') or 'cmake', ctest='ctest',
                           googletest_source=str(tmp_path)),
                fingerprints={}, contract=dict(CMAKE_GENERATOR='Ninja'), identity='0' * 64,
                identity_data={}, build_dir=str(tmp_path / 'build'), run_dir=str(tmp_path / 'run'),
                web_output=str(tmp_path / 'web'), gradle_user_home=None, targets=['core_unit_tests'],
                abis=[], build_time=None, build_time_explicit=False, errors=[])


def test_object_digests_counts_only_compile_outputs(tmp_path):
    (tmp_path / 'a.obj').write_bytes(b'a')
    (tmp_path / 'unit.o').write_bytes(b'o')
    (tmp_path / 'sub').mkdir()
    (tmp_path / 'sub/b.obj').write_bytes(b'b')
    (tmp_path / 'notes.txt').write_text('ignored', encoding='utf-8')
    assert set(incremental.object_digests(tmp_path)) == {'a.obj', 'unit.o', 'sub/b.obj'}


def test_diff_objects_reports_changed_missing_and_added():
    before = {'a.obj': '1', 'b.obj': '2'}
    after = {'a.obj': '1', 'b.obj': '3', 'c.obj': '4'}
    diff = incremental.diff_objects(before, after)
    assert diff['changed'] == ['b.obj', 'c.obj']
    assert diff['changed_count'] == 2
    assert diff['total'] == 3


def test_perturb_moves_only_mtime_and_restore_is_exact(tmp_path):
    path = tmp_path / 'unit.cpp'
    path.write_text('int main() { return 0; }\n', encoding='utf-8')
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    before = path.stat().st_mtime
    original = incremental.perturb(path)
    assert original == before
    assert path.stat().st_mtime > before
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest
    incremental.restore(path, original)
    assert path.stat().st_mtime == original


def test_pick_targets_fails_loudly_without_project_sources(tmp_path):
    with pytest.raises(ValueError, match='INCREMENTAL_SOURCES_UNAVAILABLE'):
        incremental.pick_targets(tmp_path, tmp_path)


def test_incremental_action_reuses_only_the_build_stage(tmp_path):
    steps = command_steps('incremental', _plan(tmp_path))
    assert [phase for phase, _ in steps] == ['build']


def test_incremental_rejects_gradle_profiles(tmp_path):
    with pytest.raises(ValueError, match='INCREMENTAL_PROFILE_UNSUPPORTED'):
        command_steps('incremental', _plan(tmp_path, 'android-mock'))


def _toolchain(cmake):
    """优先用与生产 core 配置一致的 VS2022；没有则回退到 Ninja + GCC/Clang。"""
    text = subprocess.run([cmake, '--help'], capture_output=True, text=True, errors='replace').stdout
    if sys.platform == 'win32' and 'Visual Studio 17 2022' in text:
        return ['-G', 'Visual Studio 17 2022', '-A', 'x64']
    if shutil.which('ninja') and (shutil.which('g++') or shutil.which('clang++')):
        return ['-G', 'Ninja']
    return None


def test_incremental_counts_are_explainable(tmp_path):
    # MSBuild 在「中间/输出目录位于系统临时目录」时会关闭可靠增量（MSB8029），
    # 那样头文件改动根本不会触发重编，测量结果会变成假阴性。夹具因此建在仓库内
    # 已忽略的 cache 下，结束时清理自己创建的目录。
    workspace = ROOT / 'cache' / 'tests' / ('incremental-' + uuid.uuid4().hex[:8])
    fixture = workspace / 'fixture'
    (fixture / 'src').mkdir(parents=True)
    (fixture / 'src/shared.h').write_text('#pragma once\nint shared();\n', encoding='utf-8')
    (fixture / 'src/one.cpp').write_text(
        '#include "shared.h"\nint shared() { return 1; }\nint one() { return shared(); }\n', encoding='utf-8')
    (fixture / 'src/two.cpp').write_text('#include "shared.h"\nint two() { return shared(); }\n', encoding='utf-8')
    (fixture / 'src/main.cpp').write_text(
        'int one(); int two();\nint main() { return one() + two() - 2; }\n', encoding='utf-8')
    (fixture / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.22)\nproject(Fixture LANGUAGES CXX)\n'
        'add_executable(fixture src/one.cpp src/two.cpp src/main.cpp)\n', encoding='utf-8')
    build, cmake = fixture / 'build', shutil.which('cmake')
    generator = _toolchain(cmake) if cmake else None
    if generator is None:
        pytest.skip('需要可用C++工具链（VS2022 或 Ninja+GCC）')
    command = [cmake, '--build', str(build), '--config', 'Release']

    def rebuild(tag):
        assert execute_step(Step('build', command, fixture, timeout=180), tmp_path / tag)['status'] == 'passed'

    try:
        assert execute_step(Step('configure', [cmake, '-S', str(fixture), '-B', str(build), *generator],
                                 fixture, timeout=180), tmp_path / 'configure')['status'] == 'passed'
        rebuild('build-1')

        # 同输入重复：必须零重编，否则说明「增量」其实每次都在重建。
        before = incremental.object_digests(build)
        rebuild('build-2')
        assert incremental.diff_objects(before, incremental.object_digests(build))['changed_count'] == 0

        picked = incremental.pick_targets(build, fixture)
        assert picked['header_includers'] >= 2

        # 单源改动：只应重编该文件对应的对象。
        before = incremental.object_digests(build)
        mtime = incremental.perturb(picked['source'])
        try:
            rebuild('build-3')
        finally:
            incremental.restore(picked['source'], mtime)
        assert incremental.diff_objects(before, incremental.object_digests(build))['changed_count'] == 1

        # 头文件改动：所有包含它的源文件都要重编。
        before = incremental.object_digests(build)
        mtime = incremental.perturb(picked['header'])
        try:
            rebuild('build-4')
        finally:
            incremental.restore(picked['header'], mtime)
        changed = incremental.diff_objects(before, incremental.object_digests(build))['changed_count']
        assert changed >= 2, '头文件改动未触发重编：检查构建目录是否落在系统临时目录（MSB8029）'

        name = 'fixture.exe' if sys.platform == 'win32' else 'fixture'
        executable = next((path for path in (build / 'Release' / name, build / name) if path.is_file()), None)
        assert executable is not None
        assert subprocess.run([str(executable)], check=False).returncode == 0
    finally:
        shutil.rmtree(workspace, ignore_errors=True)
