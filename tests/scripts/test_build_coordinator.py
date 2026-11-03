"""构建失败、取消和旧产物不会被装配阶段或verify改写为通过。"""
from contextlib import nullcontext
from pathlib import Path
import importlib.util
import json
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import building.coordinator as module
from building.profiles import make_plan
from testing.runner import Step
from test_build_profiles import fixture_inputs

ROOT = Path(__file__).resolve().parents[2]


def cli():
    spec = importlib.util.spec_from_file_location('build_cli', ROOT / 'scripts/run-builds.py')
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def synthetic_gradle(tmp_path, monkeypatch):
    """android 计划只校验发行包与完成标记；用合成发行包，避免依赖本机 Gradle 缓存。"""
    program = tmp_path / 'tools' / 'gradle.bat'
    program.parent.mkdir(parents=True, exist_ok=True)
    program.write_bytes(b'synthetic-gradle')
    home = tmp_path / 'gradle-home'
    home.mkdir(exist_ok=True)
    monkeypatch.setattr('building.profiles.cached_gradle', lambda root: (home, program, None))


def setup_run(tmp_path, monkeypatch):
    plan = make_plan(tmp_path, 'core', fixture_inputs(tmp_path), tmp_path / 'run')
    monkeypatch.setattr(module, 'source_binding', lambda root: dict(head='a' * 40, workspace_sha256='b' * 64))
    monkeypatch.setattr(module, 'RepoLock', lambda root: nullcontext())
    monkeypatch.setattr(module, 'collect_versions', lambda *args: None)
    monkeypatch.setattr(module, 'check_cache', lambda *args, **kwargs: None)
    steps = [('configure', Step('configure', [sys.executable, '-V'], tmp_path)),
             ('build', Step('compile', [sys.executable, '-V'], tmp_path))]
    monkeypatch.setattr(module, 'command_steps', lambda *args: steps)
    (tmp_path / 'bin').mkdir()
    (tmp_path / 'bin/core_unit_tests.exe').write_bytes(b'synthetic-program')
    from building.artifacts import inventory
    monkeypatch.setattr(module, 'record_outputs', lambda plan: dict(schema_version=1, directory=str(tmp_path / 'bin'),
                        identity=plan['identity'], files=inventory(tmp_path / 'bin')))
    return plan


def test_plan_cli_is_readonly_and_returns_errors(tmp_path, monkeypatch, capsys):
    entry = cli()
    monkeypatch.setattr(entry, 'ROOT', tmp_path)
    inputs = tmp_path / 'inputs.json'
    inputs.write_text('{"schema_version":1,"paths":{}}', encoding='utf-8')
    assert entry.main(['plan', '--profile', 'core', '--inputs', str(inputs)]) == 2
    assert 'MISSING_CMAKE' in capsys.readouterr().out
    assert not (tmp_path / 'cache').exists()


def test_list_does_not_require_inputs(capsys):
    assert cli().main(['list']) == 0
    assert 'windows-product' in capsys.readouterr().out


def test_ready_plan_contains_actual_commands_without_running(tmp_path, monkeypatch, capsys):
    entry = cli()
    monkeypatch.setattr(entry, 'ROOT', tmp_path)
    inputs = tmp_path / 'inputs.json'
    inputs.write_text(json.dumps(dict(schema_version=1, paths=fixture_inputs(tmp_path))), encoding='utf-8')
    assert entry.main(['plan', '--profile', 'core', '--inputs', str(inputs)]) == 0
    plan = json.loads(capsys.readouterr().out)
    assert plan['steps'][0]['name'] == 'core-configure'
    assert 'command' in plan['steps'][0] and 'environment_keys' in plan['steps'][0]
    assert 'env' not in plan['steps'][0]
    assert not (tmp_path / 'cache').exists()


@pytest.mark.parametrize('status,code', [('failed', 7), ('timed_out', -1), ('cancelled', 130)])
def test_failed_configure_never_builds_or_archives(tmp_path, monkeypatch, status, code):
    plan = setup_run(tmp_path, monkeypatch)
    calls = []
    def execute(step, output):
        calls.append(step.name)
        return dict(name=step.name, status=status, raw_exit_code=code, reason='SYNTHETIC_FAILURE')
    monkeypatch.setattr(module, 'execute_step', execute)
    result = module.run('build', plan)
    assert result['status'] != 'passed'
    assert result['steps'][1]['status'] == 'not_run'
    assert calls == ['configure'] and 'artifacts' not in result
    assert result['exit_code'] == (130 if status == 'cancelled' else 1)


def test_success_saves_bound_original_manifest(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    monkeypatch.setattr(module, 'execute_step', lambda step, output: dict(name=step.name, status='passed', raw_exit_code=0))
    result = module.run('build', plan)
    assert result['status'] == 'passed' and result['action'] == 'build'
    saved = json.loads((tmp_path / 'run/summary.json').read_text(encoding='utf-8'))
    assert saved['artifacts']['files']['core_unit_tests.exe']['sha256']
    assert saved['source_start'] == saved['source_end']


def test_build_markdown_marks_unobserved_resources_as_unavailable(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    resources = dict(observed=dict(peak_active_processes=None, peak_job_memory_bytes=None))
    monkeypatch.setattr(module, 'execute_step', lambda step, output: dict(
        name=step.name, status='passed', raw_exit_code=0, resources=resources))
    result = module.run('build', plan)
    assert result['status'] == 'passed'
    report = (tmp_path / 'run/summary.md').read_text(encoding='utf-8')
    assert '不可用' in report and 'None' not in report


def test_verify_rejects_tampering_before_any_checks(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    monkeypatch.setattr(module, 'execute_step', lambda step, output: dict(name=step.name, status='passed', raw_exit_code=0))
    assert module.run('build', plan)['status'] == 'passed'
    (tmp_path / 'bin/core_unit_tests.exe').write_bytes(b'changed-program')
    plan['run_dir'] = str(tmp_path / 'verify')
    monkeypatch.setattr(module, 'execute_step', lambda *args: pytest.fail('不应运行检查'))
    result = module.run('verify', plan, tmp_path / 'run/summary.json')
    assert result['status'] != 'passed' and 'ARTIFACT_CHANGED' in result['reason']
    old = json.loads((tmp_path / 'run/summary.json').read_text(encoding='utf-8'))
    assert old['artifacts']['files']['core_unit_tests.exe']['sha256'] != module.digest(tmp_path / 'bin/core_unit_tests.exe')


def test_lock_failure_remains_blocked_without_running(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    def unavailable(root):
        raise RuntimeError('RESOURCE_BUSY')
    monkeypatch.setattr(module, 'RepoLock', unavailable)
    monkeypatch.setattr(module, 'execute_step', lambda *args: pytest.fail('不应运行'))
    result = module.run('build', plan)
    assert result['status'] == 'blocked' and result['exit_code'] == 2


def test_source_change_prevents_pass(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    values = iter([dict(head='a', workspace_sha256='a'), dict(head='a', workspace_sha256='b')])
    monkeypatch.setattr(module, 'source_binding', lambda root: next(values))
    monkeypatch.setattr(module, 'execute_step', lambda step, output: dict(name=step.name, status='passed', raw_exit_code=0))
    result = module.run('build', plan)
    assert result['status'] == 'unverified' and result['exit_code'] != 0


def test_windows_product_order_is_web_before_native_build(tmp_path):
    plan = make_plan(ROOT, 'windows-product', fixture_inputs(tmp_path), tmp_path / 'run')
    names = [step.name for phase, step in module.command_steps('build', plan)]
    assert names.index('web-bundle') < names.index('windows-build')
    assert 'run-test-suites.py' not in ' '.join(' '.join(map(str, step.command)) for phase, step in module.command_steps('verify', plan))


def test_build_tools_follow_shared_resource_budget(tmp_path):
    plan = make_plan(ROOT, 'core', fixture_inputs(tmp_path), tmp_path / 'run')
    plan['resource_budget'] = dict(cpu_slots=3, memory_budget_mb=4096, max_parallel_steps=1)
    steps = module.command_steps('build', plan)
    build = next(step for phase, step in steps if phase == 'build')
    assert build.command[build.command.index('--parallel') + 1] == '3'
    assert build.cpu_slots == 3
    assert build.memory_budget_mb == 4096


def test_overbudget_build_step_is_blocked_before_child(tmp_path, monkeypatch):
    plan = setup_run(tmp_path, monkeypatch)
    plan['resource_budget'] = dict(cpu_slots=1, memory_budget_mb=None, max_parallel_steps=1)
    steps = [('configure', Step('too-wide', [sys.executable, '-V'], tmp_path, cpu_slots=2))]
    monkeypatch.setattr(module, 'command_steps', lambda *args: steps)
    monkeypatch.setattr(module, 'execute_step', lambda *args: pytest.fail('overbudget child started'))
    result = module.run('build', plan)
    assert result['status'] == 'blocked' and result['reason'] == 'CPU_BUDGET_EXCEEDED'
    assert result['steps'][0]['status'] == 'blocked'


def test_build_cli_accepts_shared_budget_options(capsys):
    entry = cli()
    assert entry.main(['list', '--cpu-budget', '4', '--memory-budget-mb', '8192']) == 0
    assert 'windows-product' in capsys.readouterr().out


def test_android_configure_runs_real_abi_tasks_and_isolates_outputs(tmp_path):
    plan = make_plan(ROOT, 'android-real', fixture_inputs(tmp_path), tmp_path / 'run')
    plan['build_time'] = '20261022-1630'
    steps = module.command_steps('configure', plan)
    command = ' '.join(map(str, steps[0][1].command))
    assert ':app:configureCMakeDebug[armeabi-v7a]' in command and ':app:configureCMakeDebug[arm64-v8a]' in command
    assert '--dry-run' not in command
    assert '-Prk.buildTime=20261022-1630' in command
    for action in ('build', 'verify'):
        assert '-Prk.buildTime=20261022-1630' in ' '.join(
            str(item) for _, step in module.command_steps(action, plan) for item in step.command
        )
    script = module.gradle_init(plan)
    assert 'ndkPath' in script and 'buildStagingDirectory' in script and plan['build_dir'].replace('\\', '/') in script


def test_android_plan_accepts_and_reports_a_fixed_build_time(tmp_path, monkeypatch, capsys):
    synthetic_gradle(tmp_path, monkeypatch)
    entry = cli()
    monkeypatch.setattr(entry, 'ROOT', ROOT)
    inputs = tmp_path / 'inputs.json'
    inputs.write_text(json.dumps(dict(schema_version=1, paths=fixture_inputs(tmp_path))), encoding='utf-8')
    assert entry.main(['plan', '--profile', 'android-mock', '--inputs', str(inputs),
                       '--build-time', '20261022-1630']) == 0
    plan = json.loads(capsys.readouterr().out)
    assert plan['build_time'] == '20261022-1630'
    assert any('-Prk.buildTime=20261022-1630' in step['command'] for step in plan['steps'])


def test_android_plan_rejects_invalid_calendar_time(tmp_path, monkeypatch, capsys):
    entry = cli()
    monkeypatch.setattr(entry, 'ROOT', ROOT)
    inputs = tmp_path / 'inputs.json'
    inputs.write_text(json.dumps(dict(schema_version=1, paths=fixture_inputs(tmp_path))), encoding='utf-8')
    with pytest.raises(SystemExit) as error:
        entry.main(['plan', '--profile', 'android-mock', '--inputs', str(inputs),
                    '--build-time', '20260230-1260'])
    assert error.value.code == 2
    assert 'BUILD_TIME_INVALID' in capsys.readouterr().err


def test_android_verify_reuses_the_original_build_time(tmp_path, monkeypatch):
    synthetic_gradle(tmp_path, monkeypatch)
    plan = make_plan(ROOT, 'android-mock', fixture_inputs(tmp_path), tmp_path / 'verify')
    old_time = '20261020-0801'
    binding = dict(head='a' * 40, workspace_sha256='b' * 64)
    monkeypatch.setattr(module, 'source_binding', lambda root: binding)
    monkeypatch.setattr(module, 'RepoLock', lambda root: nullcontext())
    monkeypatch.setattr(module, 'collect_versions', lambda *args: None)
    monkeypatch.setattr(module, 'record_android_toolchain', lambda *args: {'abis': {}})
    monkeypatch.setattr(module, 'check_cache', lambda *args, **kwargs: None)
    monkeypatch.setattr(module, 'verify_manifest', lambda manifest: None)
    commands = []
    def execute(step, output):
        commands.append(step.command)
        return dict(name=step.name, status='passed', raw_exit_code=0)
    monkeypatch.setattr(module, 'execute_step', execute)
    source = tmp_path / 'prior-summary.json'
    source.write_text(json.dumps(dict(status='passed', action='build', profile='android-mock',
        build_time=old_time, source_end=binding, plan=dict(identity=plan['identity']),
        artifacts=dict(directory=str(tmp_path / 'artifacts'), identity=plan['identity']))), encoding='utf-8')
    result = module.run('verify', plan, source)
    assert result['status'] == 'passed'
    assert result['build_time'] == old_time
    assert f'-Prk.buildTime={old_time}' in ' '.join(map(str, commands[0]))


def test_android_verify_never_reuses_stale_test_results(tmp_path):
    """B06：verify 必须强制重跑单元测试——只改时间或产物未变都不能跳过测试拿绿灯。"""
    plan = make_plan(ROOT, 'android-mock', fixture_inputs(tmp_path), tmp_path / 'verify')
    plan['build_time'] = '20261022-1630'
    steps = [step for _, step in module.command_steps('verify', plan)
             if any(':app:testDebugUnitTest' in str(item) for item in step.command)]
    assert len(steps) == 1
    command = [str(item) for item in steps[0].command]
    assert '--rerun' in command
    assert '-Prk.buildTime=20261022-1630' in command


def test_android_toolchain_report_uses_effective_cmake_path_and_version(tmp_path, monkeypatch):
    inputs = fixture_inputs(tmp_path)
    plan = make_plan(tmp_path, 'android-mock', inputs, tmp_path / 'run')
    cmake = Path(inputs['android_sdk']) / 'cmake/3.22.1/bin/cmake.exe'
    versions = {}
    calls = []
    def collect(suites, output, records):
        calls.append(suites[0]['tools']['android_cmake'])
        records['android_cmake'] = dict(status='passed', version='cmake version 3.22.1-g37088a8-dirty\n',
                                        evidence=dict(path=str(output / 'android_cmake/log.txt'), sha256='c' * 64))
    monkeypatch.setattr(module, 'check_cache', lambda *args, **kwargs: None)
    monkeypatch.setattr(module, 'check_android_cache', lambda plan: {'abis': {
        abi: dict(ndk_path=inputs['android_ndk'], ndk_version='27.0.12077973', cmake_executable=str(cmake))
        for abi in plan['abis']}})
    monkeypatch.setattr(module, 'collect_versions', collect)
    toolchain = module.record_android_toolchain(plan, tmp_path / 'version-logs', versions)
    assert calls == [str(cmake)]
    assert all(item['cmake_version'] == '3.22.1' for item in toolchain['abis'].values())
    assert versions['android_cmake']['evidence']['sha256'] == 'c' * 64


def test_json_record_does_not_redact_sha256_digit_sequences():
    text = module.record_json(dict(identity='a' * 10 + '13812345678' + 'b' * 43, message='token=synthetic-value'))
    assert '13812345678' in json.loads(text)['identity']
    assert 'synthetic-value' not in text


def test_cli_uses_safe_error_codes(tmp_path, monkeypatch, capsys):
    entry = cli()
    monkeypatch.setattr(entry, 'load_inputs', lambda path: (_ for _ in ()).throw(OSError('token=synthetic-private')))
    assert entry.main(['plan', '--profile', 'core', '--inputs', str(tmp_path / 'x')]) == 2
    assert 'synthetic-private' not in capsys.readouterr().err


def test_core_output_uses_actual_vs_release_layout(tmp_path):
    plan = make_plan(tmp_path, 'core', fixture_inputs(tmp_path), tmp_path / 'run')
    binary = Path(plan['build_dir']) / 'Release/core_unit_tests.exe'
    binary.parent.mkdir(parents=True)
    binary.write_bytes(b'synthetic-core-program')
    manifest = module.record_outputs(plan)
    assert manifest['files']['core_unit_tests.exe']['sha256'] == module.digest(binary)
