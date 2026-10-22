"""隔离副本的基线先通过，八类缺陷必须由真实断言检出。"""
import hashlib
from pathlib import Path
import shutil
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from testing.runner import Step, execute_step
from testing.mutations import is_detected

ROOT = Path(__file__).resolve().parents[2]
MUTATIONS = (
    ('identity', 'building/artifacts.py', 'if not equal:', 'if False:',
     'test_build_artifacts.py::test_cache_contract_accepts_only_same_source_generator_and_flags'),
    ('exit', 'building/coordinator.py', "if result['status'] != 'passed':", 'if False:',
     'test_build_coordinator.py::test_failed_configure_never_builds_or_archives[failed-7]'),
    ('stale-web', 'building/artifacts.py', '    web_files = inventory(web)',
     "    if (destination.parent / 'product/webroot').is_dir():\n        web = destination.parent / 'product/webroot'\n    web_files = inventory(web)",
     'test_build_artifacts.py::test_second_product_gets_new_web_even_with_same_executable'),
    ('extra', 'building/artifacts.py', '    actual = inventory(directory)',
     "    actual = inventory(directory)\n    actual = {key: actual[key] for key in manifest['files'] if key in actual}",
     'test_build_artifacts.py::test_product_manifest_is_bound_and_extra_files_rejected'),
    ('android-loadable', 'testing/reports.py',
     'or compiled["loadable_sha256"] != packaged["loadable_sha256"]', 'or False',
     'test_suite_reports.py::test_android_apk_binding_rejects_wrong_build[wrong_non_text]'),
    ('resource-cpu', 'testing/resources.py',
     'if claim.cpu_slots > self.budget.cpu_slots:', 'if False:',
     'test_resource_contract.py::test_claim_over_cpu_budget_is_rejected_before_acquisition'),
    ('resource-duplicate', 'testing/resources.py',
     'if len(set(keys)) != len(keys):', 'if False:',
     'test_resource_contract.py::test_duplicate_resource_key_in_one_claim_is_rejected'),
    ('resource-memory', 'testing/runner.py',
     'if memory_limit is not None and observed["peak_job_memory_bytes"] > memory_limit:',
     'if False:',
     'test_suite_runner.py::test_memory_budget_terminates_only_owned_job'),
)


@pytest.mark.parametrize('name,filename,old,new,test', MUTATIONS, ids=[m[0] for m in MUTATIONS])
def test_behavior_mutation_is_detected(tmp_path, name, filename, old, new, test):
    copy = tmp_path / 'source'
    sources = [*ROOT.joinpath('scripts/building').glob('*.py'),
               *ROOT.joinpath('scripts/testing').glob('*.py'),
               *[ROOT / 'scripts/android' / item for item in ('verify-apk-native.py', '_tool_support.py')],
               ROOT / 'scripts/audit/_common.py', ROOT / 'scripts/run-builds.py',
               *[ROOT / 'tests/scripts' / item for item in (
                   'test_build_profiles.py', 'test_build_artifacts.py', 'test_build_coordinator.py',
                   'test_suite_reports.py', 'test_suite_runner.py', 'test_resource_contract.py',
                   'test_apk_native_inspection.py', 'android_tool_test_support.py')]]
    before = {source: hashlib.sha256(source.read_bytes()).hexdigest() for source in sources}
    for source in sources:
        target = copy / source.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)

    def run(label):
        output = tmp_path / label
        return execute_step(Step(label, [sys.executable, '-m', 'pytest', 'tests/scripts/' + test,
            '-q', '--tb=line', '--basetemp', str(tmp_path / (label + '-temp')),
            '--junitxml', str(output / 'junit.xml')], copy, timeout=60,
            junit=(str(output / 'junit.xml'),), env={'PYTEST_DISABLE_PLUGIN_AUTOLOAD': '1',
            'PYTHONUTF8': '1', 'TEMP': str(tmp_path / 'temp'), 'TMP': str(tmp_path / 'temp')}), output)

    control = run('control')
    assert control['status'] == 'passed' and control['counts']['skipped'] == 0
    path = copy / 'scripts' / filename
    text = path.read_text(encoding='utf-8')
    assert text.count(old) == 1
    path.write_text(text.replace(old, new), encoding='utf-8')
    assert is_detected(run(name)), '只认可当前JUnit中的行为断言失败'
    assert before == {source: hashlib.sha256(source.read_bytes()).hexdigest() for source in sources}
