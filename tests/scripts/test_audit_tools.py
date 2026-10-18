"""审计工具验证真实输入输出，不执行历史工作区中的改写脚本。"""
import csv
import json
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'scripts' / 'audit'
BASELINE = '1' * 40


def run(name, *args):
    assert (TOOLS / name).is_file(), '正式工具入口缺失'
    return subprocess.run([sys.executable, str(TOOLS / name), *map(str, args)],
                          capture_output=True, text=True, encoding='utf-8', timeout=15)


def scoring_inputs(tmp_path):
    rubric = tmp_path / 'rubric.json'
    rubric.write_text(json.dumps({'version': 'fixture-v1', 'max_score': 4, 'dimensions': [
        {'id': 'A', 'weight': 60, 'criteria': ['A-1', 'A-2']},
        {'id': 'B', 'weight': 40, 'criteria': ['B-1', 'B-2']},
    ]}), encoding='utf-8')
    rows = [dict(criterion_id=c, dimension_id=d, weight=w, score=s, evidence_ids=e)
            for c, d, w, s, e in [('A-1', 'A', '30', '4', 'E1'),
                                  ('A-2', 'A', '30', '', ''),
                                  ('B-1', 'B', '20', '2', 'E2'),
                                  ('B-2', 'B', '20', '0', 'E3')]]
    source = tmp_path / 'criteria.csv'
    write_rows(source, rows)
    return rubric, source, rows


def write_rows(path, rows):
    with path.open('w', encoding='utf-8', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def score(rubric, source, output, baseline=BASELINE):
    return run('score-audit.py', '--rubric', rubric, '--criteria', source,
               '--baseline', baseline, '--output', output)


def test_unknown_scores_are_intervals_not_zero_or_full_evidence(tmp_path):
    rubric, source, _ = scoring_inputs(tmp_path)
    output = tmp_path / 'scorecard.json'
    result = score(rubric, source, output)
    assert result.returncode == 0, result.stderr
    report = json.loads(output.read_text(encoding='utf-8'))
    assert report['baseline_sha'] == BASELINE
    assert report['rubric_version'] == 'fixture-v1'
    assert report['overall']['lower'] == 40
    assert report['overall']['upper'] == 70
    assert report['overall']['coverage'] == .7
    assert report['overall']['verified_score'] == pytest.approx(57.142857, abs=.0001)
    assert report['overall']['provisional'] is True


@pytest.mark.parametrize('defect', ['duplicate', 'missing', 'unknown', 'dimension',
                                  'negative', 'over', 'fraction', 'nan', 'no_evidence', 'weight'])
def test_invalid_criteria_fail_without_output(tmp_path, defect):
    rubric, source, rows = scoring_inputs(tmp_path)
    if defect == 'duplicate': rows.append(rows[0].copy())
    elif defect == 'missing': rows.pop()
    elif defect == 'unknown': rows[0]['criterion_id'] = 'C-1'
    elif defect == 'dimension': rows[0]['dimension_id'] = 'B'
    elif defect == 'no_evidence': rows[0]['evidence_ids'] = ' '
    elif defect == 'weight': rows[0]['weight'] = '20'
    else: rows[0]['score'] = {'negative': '-1', 'over': '5', 'fraction': '1.5', 'nan': 'NaN'}[defect]
    write_rows(source, rows)
    output = tmp_path / 'out.json'
    assert score(rubric, source, output).returncode == 2
    assert not output.exists()


@pytest.mark.parametrize('defect', ['sum', 'duplicate_dimension', 'duplicate_criterion', 'nan', 'empty'])
def test_invalid_rubric_is_rejected(tmp_path, defect):
    rubric, source, _ = scoring_inputs(tmp_path)
    data = json.loads(rubric.read_text())
    if defect == 'sum': data['dimensions'][0]['weight'] = 59
    elif defect == 'duplicate_dimension': data['dimensions'][1]['id'] = 'A'
    elif defect == 'duplicate_criterion': data['dimensions'][1]['criteria'][0] = 'A-1'
    elif defect == 'nan': data['dimensions'][0]['weight'] = 'NaN'
    elif defect == 'empty': data['dimensions'][0]['criteria'] = []
    rubric.write_text(json.dumps(data), encoding='utf-8')
    output = tmp_path / 'out.json'
    assert score(rubric, source, output).returncode == 2
    assert not output.exists()


def test_all_unknown_has_no_verified_score(tmp_path):
    rubric, source, rows = scoring_inputs(tmp_path)
    for row in rows: row['score'] = ''
    write_rows(source, rows)
    output = tmp_path / 'out.json'
    assert score(rubric, source, output).returncode == 0
    overall = json.loads(output.read_text())['overall']
    assert overall['lower'] == 0 and overall['upper'] == 100
    assert overall['coverage'] == 0 and overall['verified_score'] is None


def test_legacy_csv_may_omit_unused_trailing_column(tmp_path):
    rubric, source, _ = scoring_inputs(tmp_path)
    lines = source.read_text().splitlines()
    lines[0] += ',finding_ids'
    source.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    assert score(rubric, source, tmp_path / 'out.json').returncode == 0


def test_missing_required_evidence_column_is_not_treated_as_unknown(tmp_path):
    rubric, source, _ = scoring_inputs(tmp_path)
    lines = source.read_text().splitlines()
    lines[1] = lines[1].rsplit(',', 1)[0]
    source.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    assert score(rubric, source, tmp_path / 'out.json').returncode == 2


def test_score_rejects_bad_baseline_and_existing_output(tmp_path):
    rubric, source, _ = scoring_inputs(tmp_path)
    output = tmp_path / 'out.json'
    assert score(rubric, source, output, 'not-a-sha').returncode == 2
    output.write_text('preserve', encoding='utf-8')
    assert score(rubric, source, output).returncode == 2
    assert output.read_text() == 'preserve'
    before = source.read_bytes()
    assert score(rubric, source, source).returncode == 2
    assert source.read_bytes() == before


def test_review_map_is_only_assignment_and_handles_spaces(tmp_path):
    source = tmp_path / 'tracked.txt'
    source.write_text('src/cpp/src/Engine.cpp\ndocs/my guide.md\nsrc/win/third_party/x.c\n', encoding='utf-8')
    output = tmp_path / 'review.csv'
    result = run('generate-review-map.py', '--files', source, '--output', output)
    assert result.returncode == 0, result.stderr
    with output.open(encoding='utf-8-sig', newline='') as stream: rows = list(csv.DictReader(stream))
    assert len(rows) == 3
    assert rows[0]['risk'] == 'high'
    assert rows[1]['path'] == 'docs/my guide.md'
    assert rows[2]['owner_class'] == 'third-party'
    assert all(row['review_state'] == 'assigned' and row['evidence_ids'] == '' for row in rows)
    assert 'heuristic' in result.stdout


@pytest.mark.parametrize('bad', ['../outside.py', 'C:/outside.py', '/absolute.py',
                               'src/../x.py', 'src\\x.py', 'src' + chr(92) + 'single.py',
                               'src/a.py\nsrc/a.py', ''])
def test_review_map_rejects_invalid_or_duplicate_paths(tmp_path, bad):
    source = tmp_path / 'tracked.txt'; source.write_text(bad, encoding='utf-8')
    output = tmp_path / 'out.csv'
    assert run('generate-review-map.py', '--files', source, '--output', output).returncode == 2
    assert not output.exists()


def test_review_map_does_not_overwrite(tmp_path):
    source = tmp_path / 'tracked.txt'; source.write_text('src/a.py\n')
    output = tmp_path / 'out.csv'; output.write_text('preserve')
    assert run('generate-review-map.py', '--files', source, '--output', output).returncode == 2
    assert output.read_text() == 'preserve'


def test_dashboard_escapes_and_redacts_untrusted_fields(tmp_path):
    rubric, source, _ = scoring_inputs(tmp_path)
    scorecard = tmp_path / 'score.json'; assert score(rubric, source, scorecard).returncode == 0
    findings = tmp_path / 'findings.jsonl'
    findings.write_text(json.dumps({'id': 'F1', 'severity': 'P2', 'status': 'candidate',
        'title': '<script>alert(1)</script>',
        'impact': 'Authorization: Bearer fixture-credential; password="fixture private value" contact@example.invalid 13800000000'})+'\n')
    coverage = tmp_path / 'coverage.csv'
    write_rows(coverage, [dict(path='<img src=x onerror=alert(1)>', review_state='assigned', evidence_ids='')])
    output = tmp_path / 'dashboard.html'
    result = run('generate-dashboard.py', '--scorecard', scorecard, '--findings', findings,
                 '--coverage', coverage, '--output', output)
    assert result.returncode == 0, result.stderr
    page = output.read_text(encoding='utf-8')
    assert '<script>' not in page and '<img src=x' not in page
    assert '&lt;script&gt;' in page and '&lt;img' in page
    assert 'fixture-credential' not in page and 'fixture private value' not in page
    assert 'contact@example.invalid' not in page and '13800000000' not in page
    assert 'heuristic' in page and BASELINE in page
    assert 'Content-Security-Policy' in page
    assert run('generate-dashboard.py', '--scorecard', scorecard, '--findings', findings,
               '--coverage', coverage, '--output', output).returncode == 2


def test_dashboard_bad_json_does_not_leak_or_create_output(tmp_path):
    source = tmp_path / 'bad.json'; source.write_text('{invalid fixture data')
    output = tmp_path / 'out.html'
    result = run('generate-dashboard.py', '--scorecard', source, '--findings', source,
                 '--coverage', source, '--output', output)
    assert result.returncode == 2 and 'fixture data' not in result.stderr
    assert not output.exists()
