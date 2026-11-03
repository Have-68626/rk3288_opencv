#!/usr/bin/env python3
"""运行前快照与运行后判读分离，避免shell继续执行吞掉真实失败。"""
import os
from pathlib import Path
from audit._common import Parser
from testing.ci import start, finish


def main():
    parser=Parser(description=__doc__)
    parser.add_argument("action", choices=("start", "finish"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--junit", action="append", default=[])
    parser.add_argument("--unit", choices=("case", "target"), default="case")
    parser.add_argument("--lcov")
    parser.add_argument("--exit-code", type=int)
    args=parser.parse_args()
    if args.action=='start':
        start(args.output,args.junit,args.unit,args.lcov)
        return 0
    if args.exit_code is None:
        parser.error('finish必须提供原始退出码')
    return finish(args.output,args.exit_code,os.environ.get('GITHUB_STEP_SUMMARY'))


if __name__=='__main__':
    raise SystemExit(main())
