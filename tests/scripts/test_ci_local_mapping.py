"""B07：本地与 CI 目标映射必须覆盖全部 CI job、本地套件与构建配置。"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from building.profiles import PROFILES
from testing.catalog import SUITES

ROOT = Path(__file__).resolve().parents[2]
GUIDE = ROOT / 'docs/guides/ci-local-mapping.md'
WORKFLOWS = (ROOT / '.github/workflows/ci.yml', ROOT / '.github/workflows/docs-audit.yml')
MARKERS = ('未运行', '非阻断', '缺资产', '跳过')


def ci_jobs(text):
    """解析 `jobs:` 块下的两空格缩进键；`on:` 里的触发名不会被误当成 job。"""
    marker = '\njobs:\n'
    body = text[text.index(marker) + len(marker):]
    jobs = []
    for line in body.splitlines():
        if line and not line.startswith(' '):
            break
        match = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
        if match:
            jobs.append(match.group(1))
    return jobs


def all_jobs():
    jobs = []
    for workflow in WORKFLOWS:
        for name in ci_jobs(workflow.read_text(encoding='utf-8')):
            if name not in jobs:
                jobs.append(name)
    return jobs


def test_workflow_jobs_are_parsed_including_docs_audit():
    assert {'changes', 'repo-hygiene', 'unit-tests', 'docs-audit', 'web', 'android', 'windows'} <= set(all_jobs())


def test_mapping_lists_every_ci_job_suite_and_profile():
    guide = GUIDE.read_text(encoding='utf-8')
    missing = [name for name in all_jobs() if name not in guide]
    missing += [name for name in SUITES if name not in guide]
    missing += [name for name in PROFILES if name not in guide]
    assert missing == []


def test_mapping_marks_unrun_nonblocking_missing_and_skipped():
    guide = GUIDE.read_text(encoding='utf-8')
    assert [marker for marker in MARKERS if marker not in guide] == []
