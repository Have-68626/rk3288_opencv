"""轻量C++词法边界检查，不证明资源配对或对象生命周期正确。"""
import re

TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\([\s\S]*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[{};()]')
CALLS={'ANativeWindow_lock','ANativeWindow_unlockAndPost','AndroidBitmap_lockPixels','AndroidBitmap_unlockPixels'}
WRAPPERS={'ScopedWindowLock','ScopedBitmapLock'}


def scan(source):
    tokens=[m for m in TOKEN.finditer(source) if not (m.group().startswith(('//','/*','R"','"',"'")))]
    scopes=[]; pending=False; result=[]
    for index,match in enumerate(tokens):
        token=match.group()
        if token in ('class','struct') and index+1<len(tokens):
            pending=tokens[index+1].group() in WRAPPERS
        elif token==';':
            pending=False
        elif token=='{':
            scopes.append(pending);pending=False
        elif token=='}':
            if scopes:scopes.pop()
        elif token in CALLS and index+1<len(tokens) and tokens[index+1].group()=='(' and not any(scopes):
            result.append(dict(line=source.count('\n',0,match.start())+1,reason='BARE_ANDROID_LOCK',symbol=token))
    if any(scopes):
        result.append(dict(line=source.count('\n')+1,reason='UNCLOSED_SCOPE'))
    return result
