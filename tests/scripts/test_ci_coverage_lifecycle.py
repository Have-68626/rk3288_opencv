"""按真实workflow顺序调用观测CLI，模拟插件内先生成LCOV；不启动shell或服务。"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import pytest

ROOT=Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('e2e_exit',[0,7])
def test_workflow_accepts_current_plugin_coverage_and_preserves_failure(tmp_path,e2e_exit):
    workflow=(ROOT/'.github/workflows/ci.yml').read_text(encoding='utf-8')
    section=workflow.split('      - name: Test (E2E)\n',1)[1].split('      - name: Upload Artifacts',1)[0]
    body=section.split('        run: |\n',1)[1]
    web=tmp_path/'web';web.mkdir()
    env=os.environ.copy()
    env['GITHUB_STEP_SUMMARY']=str(tmp_path/'step-summary.md')
    variables={'$E2E_RAW':str(e2e_exit),'$COVERAGE_RAW':'0'}
    exits={}
    for raw in body.splitlines():
        line=raw.strip()
        if line.startswith('pnpm e2e:run:coverage') or line=='pnpm coverage:report':
            coverage=web/'coverage/lcov.info';coverage.parent.mkdir(exist_ok=True)
            coverage.write_text('SF:synthetic.ts\nDA:1,1\nend_of_record\n',encoding='utf-8')
            if line.startswith('pnpm e2e:run:coverage'):
                junit=web/'tests/reports/ci-observed/cypress/current.xml'
                junit.parent.mkdir(parents=True,exist_ok=True)
                junit.write_text('<testsuite><testcase name="synthetic"/></testsuite>',encoding='utf-8')
        elif line.startswith('python3 ../scripts/record-ci-result.py '):
            arguments=[variables.get(arg,arg) for arg in shlex.split(line)[2:]]
            result=subprocess.run([sys.executable,str(ROOT/'scripts/record-ci-result.py'),*arguments],
                cwd=web,env=env,capture_output=True,text=True,encoding='utf-8',timeout=10)
            if arguments[0]=='start':assert result.returncode==0,result.stderr
            else:exits[Path(arguments[arguments.index('--output')+1]).name]=result.returncode
    assert (exits['e2e'] or exits['coverage'])==e2e_exit
    record=json.loads((web/'tests/reports/ci-observed/coverage/result.json').read_text(encoding='utf-8'))
    assert record['status']=='passed'
    assert record['raw_exit_code']==0
