"""CI非阻断策略由workflow决定；本模块只判读本轮原始结果。"""
import json
from pathlib import Path
from audit._common import redact
from .reports import snapshot, verify_junit, archive_junit, identity
from .redaction import safe_json


def start(output, patterns, unit='case', lcov=None):
    output=Path(output)
    if not patterns and not lcov:
        raise ValueError('MISSING_EVIDENCE_SPECIFICATION')
    before=snapshot(patterns)
    coverage_before=snapshot([lcov]) if lcov else {}
    output.mkdir(parents=True, exist_ok=False)
    (output/'state.json').write_text(json.dumps(dict(patterns=patterns, before=before, unit=unit,
        lcov=lcov,coverage_before=coverage_before)),encoding='utf-8')


def finish(output, raw_exit_code, summary=None):
    output=Path(output)
    state=json.loads((output/'state.json').read_text(encoding='utf-8'))
    result=dict(raw_exit_code=raw_exit_code,status='failed' if raw_exit_code else 'passed',exit_code=raw_exit_code)
    try:
        if state['patterns']:
            evidence=verify_junit(state['patterns'],state['before'],state['unit'])
            evidence['evidence']=archive_junit(evidence['evidence'], output/'evidence')
            result.update(evidence)
            if result['counts']['failed']:
                result.update(status='failed',exit_code=raw_exit_code or 1)
        if state.get('lcov'):
            path=Path(state['lcov']).absolute()
            current=identity(path)
            if state['coverage_before'].get(str(path),{}).get('sha256')==current['sha256']:
                raise ValueError('STALE_COVERAGE')
            coverage=path.read_text(encoding='utf-8')
            if not all(marker in coverage for marker in ('SF:','DA:','end_of_record')):
                raise ValueError('EMPTY_COVERAGE')
            archive=output/'coverage.lcov'
            archive.write_text(redact(coverage),encoding='utf-8')
            result['coverage_evidence']=dict(path=str(archive),source_sha256=current['sha256'],**identity(archive))
    except (ValueError,OSError):
        result.update(status='unverified',reason='INVALID_OR_STALE_JUNIT',exit_code=raw_exit_code or 2)
    text=safe_json(result)
    with (output/'result.json').open('x',encoding='utf-8') as stream:
        stream.write(text)
    if summary:
        with Path(summary).open('a',encoding='utf-8') as stream:
            stream.write(f"\n### {redact(output.name)}\n\n状态：{result['status']}；原始退出码：{raw_exit_code}；证据判定退出码：{result['exit_code']}\n")
    print(f"CI_RESULT status={result['status']} raw_exit_code={raw_exit_code} exit_code={result['exit_code']}")
    return result['exit_code']
