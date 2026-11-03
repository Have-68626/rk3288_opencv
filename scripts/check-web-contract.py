#!/usr/bin/env python3
"""真实前端API访问测试宿主；仅本机回环，独占临时配置，不装配相机。"""
from datetime import datetime, timezone
from pathlib import Path
import sys
import uuid
from audit._common import Parser
from testing.runner import RepoLock, Step, execute_step
from testing.redaction import safe_json


def main():
    parser=Parser(description=__doc__)
    parser.add_argument('--windows-test-exe',required=True,type=Path)
    parser.add_argument('--web-dir',type=Path,default=Path('web'))
    parser.add_argument('--node',default='node')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    if not args.windows_test_exe.is_file() or not (args.web_dir/'node_modules/vite').is_dir():
        print('CONTRACT_PREREQUISITE_MISSING')
        return 2
    output=root/'cache/reports/web-contract'/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:6])
    if output.parent.resolve()!=output.parent.absolute():
        print('UNSAFE_OUTPUT_ROOT')
        return 2
    with RepoLock(root):
        output.mkdir(parents=True,exist_ok=False)
        work=output/'work'
        step=Step('web-contract',[args.node,str(root/'scripts/testing/web-contract.cjs'),str(root),str(args.web_dir.resolve()),str(args.windows_test_exe.resolve()),str(work)],root,timeout=60,junit=(str(work/'junit.xml'),))
        result=execute_step(step,output/'execution')
        (output/'result.json').write_text(safe_json(result),encoding='utf-8')
    print('REPORT='+str(output/'result.json'))
    return 0 if result['status']=='passed' else 2


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8');sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
