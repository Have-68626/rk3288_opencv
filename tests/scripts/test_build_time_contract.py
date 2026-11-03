"""B06：显式构建时间必须真正被 Gradle 消费，而不只是出现在命令行里。

命令行里存在 `-Prk.buildTime=` 只证明「发出去了」。若 `app/build.gradle` 不读该属性，
产物仍用墙钟时间，可复现构建就不成立——这条静态检查锁住那条接线，
真实产物差异由 `scripts/run-builds.py` 的两次构建对比验证。
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_gradle_reads_the_explicit_build_time_property():
    text = (ROOT / 'app/build.gradle').read_text(encoding='utf-8')
    assert "project.findProperty('rk.buildTime')" in text
    assert re.search(r'def buildStamp = declaredBuildTime == null', text)
    # 未显式给出时仍保留墙钟回退，避免改变默认构建行为。
    assert 'new Date().format("yyyyMMdd-HHmm")' in text
    # 形状校验：非法值直接失败，不把任意字符串注入 BuildConfig。
    assert 'buildStamp ==~ /\\d{8}-\\d{4}/' in text
    assert 'throw new GradleException' in text


def test_build_stamp_reaches_build_config():
    text = (ROOT / 'app/build.gradle').read_text(encoding='utf-8')
    assert 'buildConfigField("String", "BUILD_TIME", "\\"${buildStamp}\\"")' in text
