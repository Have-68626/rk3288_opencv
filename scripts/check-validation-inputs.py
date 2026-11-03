#!/usr/bin/env python3
"""显式离线资产预检：0不用于精度验收；输入齐备也以77标记真实验证未执行。"""
from datetime import datetime,timezone
import json
from pathlib import Path
import sys
import uuid
from audit._common import Parser
from testing.validation import inspect_inputs
from testing.redaction import safe_json


def main():
    parser=Parser(description=__doc__)
    parser.add_argument('--manifest',required=True,type=Path)
    parser.add_argument('--allow-local-assets',action='store_true')
    args=parser.parse_args()
    try:
        data=json.loads(args.manifest.read_text(encoding='utf-8-sig'))
        result=inspect_inputs(data,args.manifest.resolve().parent,args.allow_local_assets)
    except (OSError,ValueError):
        print('VALIDATION_MANIFEST_INVALID')
        return 2
    root=Path(__file__).resolve().parents[1]
    output=root/'cache/reports/validation-inputs'/(datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S')+'-'+uuid.uuid4().hex[:6])
    if output.parent.resolve()!=output.parent.absolute():return 2
    output.mkdir(parents=True,exist_ok=False)
    (output/'result.json').write_text(safe_json(result),encoding='utf-8')
    print('REPORT='+str(output/'result.json'))
    print('TEST_UNVERIFIED accuracy=unverified device=not_run performance=not_run')
    return result['exit_code']


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8');sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
