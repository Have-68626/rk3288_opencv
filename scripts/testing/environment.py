"""只检查本次临时目录，不修改系统区域设置或全局环境。"""
import locale
from pathlib import Path
import sys
import uuid


def probe_temporary(path):
    path=Path(path)
    if path.resolve()!=path.absolute():
        raise ValueError('UNSAFE_TEMP')
    path.mkdir(parents=True,exist_ok=True)
    probe=path/('.utf8-probe-'+uuid.uuid4().hex)
    created=False
    try:
        with probe.open('x',encoding='utf-8') as stream:
            created=True
            stream.write('临时目录编码验证')
        if probe.read_text(encoding='utf-8')!='临时目录编码验证':
            raise ValueError('UTF8_ROUNDTRIP_FAILED')
    finally:
        if created:probe.unlink()
    return dict(utf8_roundtrip=True,python_utf8_mode=bool(sys.flags.utf8_mode),
                locale_encoding=locale.getpreferredencoding(False),child_encoding='utf-8')


def diagnostic(error,stage):
    return dict(stage=stage,type=type(error).__name__,errno=getattr(error,'errno',None),
                winerror=getattr(error,'winerror',None))
