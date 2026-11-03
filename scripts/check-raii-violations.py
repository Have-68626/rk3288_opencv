#!/usr/bin/env python3
"""检查跟踪文件或改动文件内的Android裸锁调用；不覆盖open/close等其他族。"""
from pathlib import Path
import subprocess
import sys
from audit._common import Parser
from testing.raii import scan


def main():
    parser=Parser(description=__doc__)
    parser.add_argument('--changed',action='store_true')
    parser.add_argument('--base',default='HEAD')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    if args.changed:
        commands=[['git','diff','--name-only','-z',args.base,'--'],['git','ls-files','--others','--exclude-standard','-z']]
    else:
        commands=[['git','ls-files','-z']]
    names=set()
    for command in commands:
        names.update(subprocess.check_output(command,cwd=root).decode('utf-8').strip('\0').split('\0'))
    violations=0;count=0
    for name in sorted(names):
        path=root/name
        if path.suffix.lower() not in ('.cpp','.cc','.cxx','.h','.hpp') or not path.exists():continue
        if path.resolve()!=path.absolute() or not path.is_file():
            raise ValueError('UNSAFE_SOURCE_PATH')
        count+=1
        for item in scan(path.read_text(encoding='utf-8-sig')):
            print(f"{name}:{item['line']} {item['reason']}")
            violations+=1
    print(f'RAII_SCAN files={count} violations={violations} scope=android_window_bitmap_lexical')
    return 1 if violations else 0


if __name__=='__main__':
    sys.stdout.reconfigure(encoding='utf-8');sys.stderr.reconfigure(encoding='utf-8')
    raise SystemExit(main())
