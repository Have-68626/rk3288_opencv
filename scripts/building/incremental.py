"""B05 受控增量比较：只扰动 mtime、不改内容，复用原生构建与既有 verify 判据。

评估目的是回答「是否需要引入编译缓存」，因此只测量原生增量行为的可解释性，
不引入 ccache 等新依赖，也不把总耗时当作编译收益。
"""
import hashlib
import os
import re
import time
from pathlib import Path

from .profiles import safe_path

CASES = ('repeat', 'source', 'header')
# 只有 CMake 构建树（MSBuild/Ninja）参与；Gradle 与 Web 的增量由各自工具链负责，不在此冒充。
SUPPORTED = ('core', 'windows', 'android-cli')
OBJECT_SUFFIXES = ('.obj', '.o')


def object_digests(build_dir):
    """构建树内全部对象文件的摘要（相对路径 -> sha256），用于统计重编单元。"""
    root = safe_path(build_dir)
    digests = {}
    for path in sorted(root.rglob('*')):
        if path.suffix.lower() in OBJECT_SUFFIXES and path.is_file():
            digests[path.relative_to(root).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return digests


def diff_objects(before, after):
    """比较两次对象快照；changed 同时覆盖「内容变化」与「新增/缺失」。"""
    changed = sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))
    return dict(changed=changed, changed_count=len(changed), total=len(after))


def artifact_paths(plan):
    """按 record_outputs 的落点规则给出该配置的产物路径。"""
    build, profile = Path(plan['build_dir']), plan['canonical']
    if profile in ('core', 'windows'):
        base, suffix = build / 'Release', '.exe'
    elif profile == 'android-cli':
        base, suffix = build / 'bin', ''
    else:
        return {}
    return {name: base / (name + suffix) for name in plan['targets']}


def artifact_digests(plan):
    return {name: hashlib.sha256(path.read_bytes()).hexdigest()
            for name, path in artifact_paths(plan).items() if path.is_file()}


def project_sources(build_dir):
    """从生成的工程文件反推参与编译的源文件（不猜、不硬编码）。

    VS 生成器写在 `*.vcxproj` 的 ClCompile 里；Ninja 生成器写在 `build.ninja` 的
    `build <obj>: CXX_COMPILER__<...> <src>` 规则里。两条路径都解析，任一可用即可。
    """
    root = safe_path(build_dir)
    sources = set()
    for project in sorted(root.glob('*.vcxproj')):
        text = project.read_text(encoding='utf-8', errors='replace')
        sources.update(re.findall(r'<ClCompile Include="([^"]+)"', text))
    ninja = root / 'build.ninja'
    if ninja.is_file():
        for line in ninja.read_text(encoding='utf-8', errors='replace').splitlines():
            match = re.match(r'^build [^:]+: CXX_COMPILER__\S+ (\S+)', line)
            if match:
                sources.add(match.group(1))
    return sorted(sources)


def _resolve(name, build_dir):
    """工程文件里的源路径可能是绝对路径，也可能相对构建树（CMake 两种生成器都如此）。"""
    path = Path(name)
    return path if path.is_absolute() else safe_path(build_dir) / path


def include_dirs(build_dir):
    """从生成的工程文件里取出项目 include 路径，用于解析 `#include "X"`。

    真实工程的源文件普遍写 `#include "rk_win/X.h"` 这类以 include 根为基准的路径，
    只按源文件所在目录解析会全部找不到，进而误报「没有头文件可扰动」。
    """
    root = safe_path(build_dir)
    raw = []
    for project in sorted(root.glob('*.vcxproj')):
        text = project.read_text(encoding='utf-8', errors='replace')
        for value in re.findall(r'<AdditionalIncludeDirectories>([^<]*)</AdditionalIncludeDirectories>', text):
            raw += [item.strip().strip('"') for item in value.split(';') if item.strip()]
    ninja = root / 'build.ninja'
    if ninja.is_file():
        raw += re.findall(r'-I\s*(\S+)', ninja.read_text(encoding='utf-8', errors='replace'))
    found = []
    for item in raw:
        path = _resolve(item, build_dir)
        if path.is_dir():
            found.append(path.resolve())
    return sorted(set(found))


def pick_targets(build_dir, root):
    """挑选受控扰动目标：一个源文件，以及被最多源文件直接包含的项目头文件。

    头文件在 VS 生成的工程文件里没有 ClInclude 条目，因此从源文件的 #include 反推，
    按「源文件目录 + 工程 include 根」依次解析；找不到项目内目标时**响亮失败**，
    不用空扰动冒充「没有重编」。
    """
    root = safe_path(root)
    sources = [_resolve(name, build_dir) for name in project_sources(build_dir)]
    sources = [path for path in sources if path.is_file() and path.resolve().is_relative_to(root)]
    if not sources:
        raise ValueError('INCREMENTAL_SOURCES_UNAVAILABLE')
    roots = include_dirs(build_dir)
    counts = {}
    for source in sources:
        text = source.read_text(encoding='utf-8', errors='replace')
        for name in re.findall(r'^\s*#include\s+"([^"]+)"', text, re.M):
            for base in [source.parent, *roots]:
                candidate = (base / name).resolve()
                if candidate.is_file() and candidate.is_relative_to(root):
                    counts[candidate] = counts.get(candidate, 0) + 1
                    break
    if not counts:
        raise ValueError('INCREMENTAL_HEADER_UNAVAILABLE')
    header = max(sorted(counts), key=lambda path: (counts[path], str(path)))
    return dict(source=str(sources[0]), header=str(header), header_includers=counts[header])


def perturb(path):
    """只把 mtime 前移到当前时刻（内容与摘要不变），返回原 mtime 供还原。

    必须用「当前时刻」而不是「原 mtime + 偏移」：源文件 mtime 可能远早于对象文件，
    加常数仍会旧于产物，构建系统不会重编，测量就会把「没触发」误读成「无增量」。
    """
    target = safe_path(path)
    before = target.stat().st_mtime
    now = time.time()
    # Windows 的 time.time() 粒度约 15.6ms，刚写入文件的 mtime 可能略新于下一次读数；
    # 相等或倒退都会让构建系统不重编，因此保证比原 mtime 严格前进。
    stamp = now if now > before else before + 1.0
    os.utime(target, (stamp, stamp))
    return before


def restore(path, mtime):
    target = safe_path(path)
    os.utime(target, (mtime, mtime))


def case_targets(case, build_dir, root):
    """给出该用例的扰动目标；repeat 不需要扰动。"""
    if case == 'repeat':
        return None
    picked = pick_targets(build_dir, root)
    return picked['source'] if case == 'source' else picked['header']
