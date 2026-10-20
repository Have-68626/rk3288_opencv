"""从固定ZIP准备离线构建源码；拒绝未知补丁前提与覆盖已有输出。"""
from pathlib import Path
import json
import os
import re

from _common import Parser, ToolError, check_output, extract_zip, lock, run_checked, run_cli, sha256, write_report


def patch_source(root, jobs):
    root = Path(root)
    if type(jobs) is not int or not 1 <= jobs <= 64:
        raise ToolError('INVALID_JOB_COUNT')
    replacements = {
        'android.sh': ('BUILD_VERSION=$(git describe --tags --always 2>>"${BASEDIR}"/build.log)',
                       'BUILD_VERSION="' + lock()['ffmpegkit']['version'] + '"', 1),
        'scripts/function.sh': ('echo $(nproc)', 'echo ' + str(jobs), 1),
        'scripts/android/cpu-features.sh': ('$(android_ndk_cmake) || return 1', '$(android_ndk_cmake) -DBUILD_TESTING=OFF || return 1', 1),
        'android/jni/Android.mk': ('MY_ARM_NEON := false', 'MY_ARM_NEON := true', 2),
    }
    planned = {}
    for name, (old, new, expected_count) in replacements.items():
        text = (root / name).read_text(encoding='utf8')
        if text.count(old) != expected_count:
            raise ToolError('PATCH_PRECONDITION_FAILED')
        planned[root / name] = text.replace(old, new)
    removed = 0
    for path in root.rglob('*.sh'):
        text = planned.get(path, path.read_text(encoding='utf8'))
        changed, count = re.subn(r'(?m)^(\s*)env(\s+1>>.*)?$', r"\1printf '[环境列表已省略]\\n'\2", text)
        removed += count
        if count:
            planned[path] = changed
    if not removed:
        raise ToolError('ENVIRONMENT_PATCH_MISSING')
    for path, text in planned.items():
        path.write_text(text, encoding='utf8', newline='\n')
    return {'environment_dumps_removed': removed, 'jobs': jobs,
            'patched_files': {p.relative_to(root).as_posix(): sha256(p) for p in planned}}


def verify(root):
    root = Path(root)
    metadata = json.loads((root / '.ffmpegkit-prepared.json').read_text(encoding='utf8'))
    if metadata['source_zip_sha256'] != lock()['ffmpegkit']['sha256']:
        raise ToolError('SOURCE_DIGEST_MISMATCH')
    for name, digest in metadata['patched_files'].items():
        if sha256(root / name) != digest:
            raise ToolError('PATCHED_SOURCE_CHANGED')
    for name, expected in lock()['sources'].items():
        actual = run_checked(['git', '-C', root / expected.get('directory', 'src/' + name), 'rev-parse', 'HEAD']).decode().strip()
        if actual != expected['commit']:
            raise ToolError('DEPENDENCY_COMMIT_MISMATCH')
    return metadata


def prepare(archive, output, mirrors, jobs):
    check_output(output)
    if not isinstance(mirrors, dict) or set(mirrors) != set(lock()['sources']):
        raise ToolError('MIRROR_INPUTS_MISMATCH')
    # 在解包前检查全部依赖，避免边准备边静默补下载。
    for name, expected in lock()['sources'].items():
        actual = run_checked(['git', '-C', mirrors[name], 'rev-parse', 'HEAD']).decode().strip()
        if actual != expected['commit']:
            raise ToolError('DEPENDENCY_COMMIT_MISMATCH')
    extract_zip(archive, output, lock()['ffmpegkit']['sha256'])
    root = Path(output) / 'ffmpeg-kit-6.0.LTS'
    metadata = patch_source(root, jobs)
    for name, expected in lock()['sources'].items():
        destination = root / expected.get('directory', 'src/' + name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        run_checked(['git', '-c', 'core.autocrlf=false', '-c', 'core.hooksPath=' + os.devnull,
                     'clone', '--quiet', '--no-hardlinks', mirrors[name], destination])
        run_checked(['git', '-c', 'core.hooksPath=' + os.devnull, '-C', destination,
                     'checkout', '--quiet', '--detach', expected['commit']])
    metadata['source_zip_sha256'] = lock()['ffmpegkit']['sha256']
    metadata['scope'] = '源码准备与固定提交绑定，不代表构建或设备验收'
    write_report(root / '.ffmpegkit-prepared.json', metadata)
    verify(root)
    return metadata


def main():
    parser = Parser(description=__doc__)
    parser.add_argument('--source-zip')
    parser.add_argument('--output')
    parser.add_argument('--mirrors')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--source-dir')
    args = parser.parse_args()
    if args.verify_only:
        if not args.source_dir:
            raise ToolError('SOURCE_DIR_REQUIRED')
        verify(args.source_dir)
    else:
        if not all([args.source_zip, args.output, args.mirrors]):
            raise ToolError('PREPARE_INPUTS_REQUIRED')
        prepare(args.source_zip, args.output, json.loads(Path(args.mirrors).read_text(encoding='utf8')), args.jobs)
    print('SOURCE_INPUTS_VERIFIED=true')
    return 0


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
