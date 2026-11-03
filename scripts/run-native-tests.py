#!/usr/bin/env python3
"""固定五个Windows测试目标；内部执行由上层Job或调用者管理。"""
from pathlib import Path
from audit._common import Parser
from testing.native import run_native


def main():
    parser = Parser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--report-dir', required=True, type=Path)
    parser.add_argument('--ctest', default='ctest')
    parser.add_argument('--config', default='Release')
    args = parser.parse_args()
    return run_native(args.build_dir.resolve(), args.report_dir, args.ctest, args.config)


if __name__ == '__main__':
    raise SystemExit(main())
