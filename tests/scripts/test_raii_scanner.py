"""词法扫描只承诺两个Android锁族，不冒充C++语义或open/close配对验证。"""
from pathlib import Path
import sys
import pytest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))


@pytest.mark.parametrize('source',[
    'class ScopedWindowLock { void f(){ ANativeWindow_lock(w,b,0); } }; ANativeWindow_lock(w,b,0);',
    'class ScopedWindowLock { void f(){ ANativeWindow_lock(w,b,0); } }; void g(){ ANativeWindow_lock(w,b,0); }',
    'struct ScopedBitmapLock; void g(){ AndroidBitmap_unlockPixels(e,b); }',
    'void g(){ AndroidBitmap_lockPixels(e,b,p); }',
])
def test_outside_wrapper_calls_are_detected_even_on_same_line(source):
    from testing.raii import scan
    assert len(scan(source))==1


@pytest.mark.parametrize('source',[
    'class ScopedWindowLock { void f(){ ANativeWindow_lock(w,b,0); ANativeWindow_unlockAndPost(w); } };',
    'const char* a=R"tag(\n{{ ANativeWindow_lock(w,b,0);\n)tag";',
    '// ANativeWindow_lock(w,b,0);\n/* AndroidBitmap_lockPixels(e,b,p); */',
    'const char* s="AndroidBitmap_unlockPixels(e,b);";',
])
def test_comments_strings_and_wrapper_body_are_not_false_positives(source):
    from testing.raii import scan
    assert scan(source)==[]


def test_unclosed_wrapper_is_visible_instead_of_silent_exemption():
    from testing.raii import scan
    assert scan('class ScopedWindowLock { void f(){ ANativeWindow_lock(w,b,0); }')[0]['reason']=='UNCLOSED_SCOPE'
