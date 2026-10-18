"""复用现有Android静态校验与独占输出，避免构建工具各自放宽边界。"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import zipfile

ANDROID = Path(__file__).resolve().parents[1] / 'android'
sys.path.insert(0, str(ANDROID))
from _tool_support import Parser, ToolError, check_output, run_cli, write_report

_spec = importlib.util.spec_from_file_location('_ffmpegkit_apk_validator', ANDROID / 'verify-apk-native.py')
_validator = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_validator)
elf_info = _validator.elf_info
LOCK_PATH = Path(__file__).with_name('sources.lock.json')
ABIS = {'armeabi-v7a': (1, 40), 'arm64-v8a': (2, 183)}


def lock():
    return json.loads(LOCK_PATH.read_text(encoding='utf8'))


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def run_checked(command, cwd=None):
    try:
        result = subprocess.run(list(map(str, command)), cwd=cwd, capture_output=True, timeout=600)
    except (OSError, subprocess.TimeoutExpired):
        raise ToolError('BUILD_COMMAND_FAILED') from None
    if result.returncode:
        raise ToolError('BUILD_COMMAND_FAILED')
    return result.stdout


def extract_zip(archive, output, expected_sha256):
    check_output(output)
    if sha256(archive) != expected_sha256:
        raise ToolError('SOURCE_DIGEST_MISMATCH')
    try:
        with zipfile.ZipFile(archive) as source:
            names = set()
            for item in source.infolist():
                name = item.filename
                path = PurePosixPath(name)
                if (not name or '\\' in name or ':' in name or path.is_absolute() or '..' in path.parts
                        or name in names or stat.S_ISLNK(item.external_attr >> 16)):
                    raise ToolError('UNSAFE_ARCHIVE_ENTRY')
                names.add(name)
            # 先验证整份清单，不能在发现后部越界时留下已解出的前半包。
            if source.testzip() is not None:
                raise ToolError('INVALID_SOURCE_ARCHIVE')
            Path(output).mkdir(parents=True)
            source.extractall(output)
            if os.name != 'nt':
                for item in source.infolist():
                    mode = (item.external_attr >> 16) & 0o777
                    if mode:
                        (Path(output) / item.filename).chmod(mode)
    except zipfile.BadZipFile:
        raise ToolError('INVALID_SOURCE_ARCHIVE') from None


def regular_files(root):
    root = Path(root)
    if (not root.is_dir() or root.is_symlink()
            or getattr(root.lstat(), 'st_file_attributes', 0) & 1024):
        raise ToolError('INVALID_INPUT_DIRECTORY')
    result = []
    pending = [root]
    while pending:
        for entry in pending.pop().iterdir():
            info = entry.lstat()
            if entry.is_symlink() or getattr(info, 'st_file_attributes', 0) & 1024:
                raise ToolError('INPUT_LINK_REJECTED')
            if entry.is_dir():
                pending.append(entry)
            elif entry.is_file():
                result.append(entry)
    return sorted(result)


def validate_libraries(libraries, platform):
    libraries = Path(libraries)
    if {p.name for p in libraries.iterdir() if p.is_dir()} != set(ABIS):
        raise ToolError('NATIVE_ABIS_MISMATCH')
    rows = []
    for abi, identity in ABIS.items():
        system = platform.get(abi)
        if (not isinstance(system, list) or 'libc.so' not in system
                or any(not isinstance(n, str) or not re.fullmatch(r'[A-Za-z0-9_+.-]+\.so', n) for n in system)):
            raise ToolError('INVALID_PLATFORM_LIBRARIES')
        files = regular_files(libraries / abi)
        if any(p.parent != libraries / abi or p.suffix != '.so' for p in files):
            raise ToolError('UNEXPECTED_NATIVE_INPUT')
        names = {p.name for p in files}
        if not {'libffmpegkit.so', 'libffmpegkit_abidetect.so', 'libavcodec.so', 'libavformat.so'} <= names:
            raise ToolError('MISSING_NATIVE_LIBRARY')
        if names.intersection(system):
            raise ToolError('SYSTEM_STUB_IN_BUNDLE')
        for path in files:
            info = elf_info(path.read_bytes())
            if (info['elf_class'], info['machine']) != identity:
                raise ToolError('NATIVE_ABI_MISMATCH')
            if info['has_textrel']:
                raise ToolError('NATIVE_TEXTREL')
            if set(info['needed']) - names - set(system):
                raise ToolError('NATIVE_DEPENDENCY_MISSING')
            rows.append({'abi': abi, 'file': path.name, 'sha256': sha256(path),
                         'bytes': path.stat().st_size, 'needed': info['needed']})
    return rows


def redact(line):
    line = re.sub(r'(?i)([a-z][a-z0-9+.-]*://)[^\s/@]+@', r'\1[已脱敏]@', line)
    line = re.sub(r'(?i)(authorization\s*[:=]\s*)(?:Bearer\s+|Basic\s+)?[^\r\n,;]+', r'\1[已脱敏]', line)
    line = re.sub(r'''(?i)(password|token|secret|api[_-]?key)(\s*[:=]\s*)("[^"]*"|'[^']*'|[^\s,;]+)''', r'\1\2[已脱敏]', line)
    line = re.sub(r'[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}', '[邮箱已脱敏]', line)
    return re.sub(r'(?<!\d)1[3-9]\d{9}(?!\d)', '[手机号已脱敏]', line)


if __name__ == '__main__' and sys.argv[1:] == ['--redact-stdin']:
    for line in sys.stdin:
        print(redact(line), end='', flush=True)
