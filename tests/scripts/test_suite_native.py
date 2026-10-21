"""原生报告必须区分目标、执行次数和未执行的禁用用例。"""
from pathlib import Path
import sys
import shutil
import xml.etree.ElementTree as ET
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))


def evidence(tmp_path, text, gtest=None):
    root = ET.Element('testsuite', tests='1', failures='0', skipped='0')
    case = ET.SubElement(root, 'testcase', name='win_unit_tests')
    ET.SubElement(case, 'system-out').text = text
    ctest = tmp_path/'ctest.xml'
    ET.ElementTree(root).write(ctest, encoding='utf-8')
    google = tmp_path/'gtest.xml'
    if gtest is not None:
        google.write_text(gtest, encoding='utf-8')
    return ctest, google


def test_mixed_target_counts_real_cases_and_excludes_disabled(tmp_path):
    from testing.native import parse_target
    ctest, google = evidence(tmp_path, 'TEST_PASS name=one\nTEST_SUMMARY pass=1 fail=0 total=1\n',
        '<testsuites><testsuite><testcase classname="Http" name="ok" status="run"/>'
        '<testcase classname="Http" name="skip" status="run"><skipped/></testcase>'
        '<testcase classname="Http" name="DISABLED_later" status="notrun"/>'
        '</testsuite></testsuites>')
    result = parse_target('win_unit_tests', ctest, google)
    assert len(result['cases']) == 3
    assert result['disabled'] == 1
    assert sum(c['status']=='skipped' for c in result['cases']) == 1
    assert len({c['identity'] for c in result['cases']}) == 3


@pytest.mark.parametrize('text', [
    'TEST_PASS name=one\nTEST_SUMMARY pass=2 fail=0 total=2\n',
    'TEST_PASS name=one\nTEST_PASS name=one\nTEST_SUMMARY pass=2 fail=0 total=2\n',
    'TEST_PASS name=one\n',
])
def test_bool_summary_missing_duplicate_or_contradictory_is_rejected(tmp_path, text):
    from testing.native import parse_target
    ctest, google = evidence(tmp_path, text, '<testsuites><testsuite><testcase name="ok"/></testsuite></testsuites>')
    with pytest.raises(ValueError):
        parse_target('win_unit_tests', ctest, google)


def test_missing_google_report_is_unverified(tmp_path):
    from testing.native import parse_target
    ctest, google = evidence(tmp_path, 'TEST_PASS name=one\nTEST_SUMMARY pass=1 fail=0 total=1\n')
    with pytest.raises(ValueError, match='MISSING_GTEST'):
        parse_target('win_unit_tests', ctest, google)


def test_truncated_google_xml_is_a_report_error(tmp_path):
    from testing.native import parse_target
    ctest,google=evidence(tmp_path,'TEST_PASS name=one\nTEST_SUMMARY pass=1 fail=0 total=1\n','<testsuites>')
    with pytest.raises(ValueError,match='INVALID_NATIVE_XML'):
        parse_target('win_unit_tests',ctest,google)


@pytest.mark.parametrize('google',[
    '<testsuites tests="99" failures="4"><testsuite><testcase name="one" status="run"/></testsuite></testsuites>',
    '<testsuites><testsuite><testcase name="one" status="run"/><testcase name="hidden_failure" status="notrun"><failure/></testcase></testsuite></testsuites>',
])
def test_google_contradictory_counts_or_notrun_failure_are_rejected(tmp_path,google):
    from testing.native import parse_target
    ctest,report=evidence(tmp_path,'',google)
    with pytest.raises(ValueError):
        parse_target('core_gtest_tests',ctest,report,expected_name=False)


def test_reentry_and_default_google_execution_have_same_identity(tmp_path):
    from testing.native import parse_target
    ctest, google = evidence(tmp_path, '', '<testsuites><testsuite><testcase classname="Engine" name="Run" status="run"/></testsuite></testsuites>')
    # 目标名称不同，身份仍使用真正的可执行文件；不能冒充两条唯一用例。
    first = parse_target('core_gtest_tests', ctest, google, expected_name=False)
    second = parse_target('engine_run_reentry_behavior', ctest, google, expected_name=False)
    assert first['cases'][0]['identity'] == second['cases'][0]['identity']


def test_google_failure_and_absent_assets_are_preserved(tmp_path):
    from testing.native import parse_target
    ctest, google = evidence(tmp_path, 'TEST_SKIP_ABSENT model=x\nTEST_FAIL name=one\nTEST_SUMMARY pass=0 fail=1 total=1\n',
        '<testsuites><testsuite><testcase classname="Http" name="bad"><failure/></testcase></testsuite></testsuites>')
    result = parse_target('win_unit_tests', ctest, google)
    assert all(c['status']=='failed' for c in result['cases'])
    assert result['asset_unverified'] is True


def test_ctest_does_not_truncate_bool_summary_after_long_output(tmp_path):
    from testing.native import run_native, TARGETS
    ctest=shutil.which('ctest')
    if not ctest:
        pytest.skip('缺少CTest工具')
    build=tmp_path/'build'; build.mkdir()
    script=build/'fixture.py'
    script.write_text('''import os,sys
from pathlib import Path
target=sys.argv[1]
print('x'*4096)
if target in ('win_unit_tests','face_infer_unit_tests'):
    print('TEST_PASS name=one\\nTEST_SUMMARY pass=1 fail=0 total=1')
if target != 'face_infer_unit_tests':
    Path(os.environ['GTEST_OUTPUT'][4:]).write_text('<testsuites><testsuite><testcase classname="Synthetic" name="one" status="run"/></testsuite></testsuites>',encoding='utf-8')
''',encoding='utf-8')
    (build/'CTestTestfile.cmake').write_text('\n'.join(
        f'add_test({target} "{Path(sys.executable).as_posix()}" "{script.as_posix()}" {target})'
        for target in TARGETS),encoding='utf-8')
    assert run_native(build,tmp_path/'out',ctest)==0
