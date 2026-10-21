"""非阻断CI也必须保留原始失败和本轮证据。"""
from pathlib import Path
import json
import sys
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]/'scripts'))


def test_nonblocking_exit_seven_remains_visible(tmp_path):
    from testing.ci import start, finish
    report=tmp_path/'junit.xml'
    state=tmp_path/'state'
    start(state, [str(report)])
    report.write_text('<testsuite><testcase name="one"><failure/></testcase></testsuite>',encoding='utf-8')
    assert finish(state, 7, tmp_path/'summary.md') == 7
    result=json.loads((state/'result.json').read_text(encoding='utf-8'))
    assert result['raw_exit_code']==7 and result['status']=='failed'
    assert '7' in (tmp_path/'summary.md').read_text(encoding='utf-8')


@pytest.mark.parametrize('existing', [False, True])
def test_missing_or_unchanged_report_cannot_pass(tmp_path, existing):
    from testing.ci import start, finish
    report=tmp_path/'junit.xml'
    if existing:
        report.write_text('<testsuite><testcase name="one"/></testsuite>',encoding='utf-8')
    state=tmp_path/'state'
    start(state, [str(report)])
    assert finish(state, 0, tmp_path/'summary.md')==2
    assert json.loads((state/'result.json').read_text(encoding='utf-8'))['status']=='unverified'


def test_fresh_failed_junit_overrides_zero_exit(tmp_path):
    from testing.ci import start, finish
    report=tmp_path/'junit.xml'; state=tmp_path/'state'
    start(state,[str(report)])
    report.write_text('<testsuite><testcase name="one"><failure/></testcase></testsuite>',encoding='utf-8')
    assert finish(state,0,tmp_path/'summary.md')==1


def test_start_cannot_reuse_prior_state(tmp_path):
    from testing.ci import start
    start(tmp_path/'state', [str(tmp_path/'x.xml')])
    with pytest.raises(FileExistsError):
        start(tmp_path/'state', [str(tmp_path/'x.xml')])


def test_coverage_requires_current_nonempty_lcov(tmp_path):
    from testing.ci import start,finish
    report=tmp_path/'coverage.info'
    state=tmp_path/'state'
    start(state,[],lcov=str(report))
    report.write_text('SF:synthetic.ts\nDA:1,1\nend_of_record\n',encoding='utf-8')
    assert finish(state,0)==0
    start(tmp_path/'stale',[],lcov=str(report))
    assert finish(tmp_path/'stale',0)==2
