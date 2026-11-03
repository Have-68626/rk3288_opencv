"""环境故障必须在启动业务前诊断，不能丢失系统错误码。"""
from pathlib import Path
import sys
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))


def test_temporary_probe_roundtrips_utf8_and_removes_only_own_file(tmp_path):
    from testing.environment import probe_temporary
    keep=tmp_path/'keep.txt';keep.write_text('保留',encoding='utf-8')
    result=probe_temporary(tmp_path)
    assert result['utf8_roundtrip'] is True
    assert list(tmp_path.iterdir())==[keep]


def test_python310_without_getencoding_still_passes_preflight(tmp_path,monkeypatch):
    import testing.environment as environment
    monkeypatch.delattr(environment.locale,'getencoding',raising=False)
    monkeypatch.setattr(environment.locale,'getpreferredencoding',lambda do_setlocale=False:'cp936')
    result=environment.probe_temporary(tmp_path)
    assert result['utf8_roundtrip'] is True
    assert result['locale_encoding']=='cp936'


def test_unwritable_temp_is_blocked_before_any_process(tmp_path, monkeypatch):
    from testing.runner import Step,execute_step
    import testing.environment as environment
    import testing.runner as runner
    monkeypatch.setattr(environment,'probe_temporary',lambda path: (_ for _ in ()).throw(PermissionError(13,'synthetic-private-detail')))
    monkeypatch.setattr(runner.subprocess,'Popen',lambda *a,**k:pytest.fail('不应启动'))
    result=execute_step(Step('test',[sys.executable,'--version'],tmp_path),tmp_path/'run')
    assert result['status']=='blocked'
    assert result['diagnostic']['errno']==13
    assert result['diagnostic']['stage']=='temporary_preflight'
    assert 'synthetic-private-detail' not in str(result)


def test_coordinator_preserves_safe_error_code_on_boundary_failure(tmp_path,monkeypatch):
    import importlib.util
    from testing.runner import Step
    spec=importlib.util.spec_from_file_location('environment_cli',Path(__file__).resolve().parents[2]/'scripts/run-test-suites.py')
    cli=importlib.util.module_from_spec(spec);spec.loader.exec_module(cli)
    monkeypatch.setattr(cli,'ROOT',tmp_path)
    monkeypatch.setattr(cli,'binding',lambda root:dict(head='fixture',workspace_sha256='fixed'))
    monkeypatch.setattr(cli,'collect_versions',lambda *args:None)
    monkeypatch.setattr(cli,'make_suites',lambda *args:[dict(id='docs',errors=[],steps=[Step('one',[],tmp_path)])])
    monkeypatch.setattr(cli,'execute_step',lambda *args:(_ for _ in ()).throw(OSError(28,'synthetic-private-detail')))
    assert cli.main(['run','--suite','docs'])==2
    import json
    result=json.loads(next((tmp_path/'cache/reports/test-suites').glob('*/summary.json')).read_text(encoding='utf-8'))
    assert result['diagnostic']['errno']==28
    assert 'synthetic-private-detail' not in str(result)
