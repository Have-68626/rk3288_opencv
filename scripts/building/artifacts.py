"""对照原始摘要核验产物，发布失败不覆盖已有目录。"""
import ctypes
from html.parser import HTMLParser
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
from urllib.parse import unquote, urlsplit
import zipfile

from .profiles import digest, safe_path, input_file

ANDROID_ABIS = ('armeabi-v7a', 'arm64-v8a')


def inventory(directory):
    directory = safe_path(directory)
    if not directory.is_dir():
        raise ValueError('MISSING_ARTIFACT_DIRECTORY')
    output = {}
    def visit(folder):
        for path in sorted(folder.iterdir()):
            safe_path(path)
            if path.is_dir():
                visit(path)
            elif path.is_file():
                output[path.relative_to(directory).as_posix()] = dict(sha256=digest(path), size=path.stat().st_size)
            else:
                raise ValueError('INVALID_ARTIFACT')
    visit(directory)
    return output


def android_native_libraries(apk_path, expected_abis=ANDROID_ABIS):
    apk_path = safe_path(apk_path)
    try:
        with zipfile.ZipFile(apk_path) as apk:
            names = apk.namelist()
            if len(names) != len(set(names)):
                raise ValueError('ANDROID_APK_DUPLICATE_ENTRY')
            if any('assets/face_models/' in name for name in names):
                raise ValueError('APK_CONTAINS_MODELS')
            libraries, abis = {}, set()
            for name in names:
                parts = name.split('/')
                if len(parts) != 3 or parts[0] != 'lib' or not parts[2].lower().endswith('.so'):
                    continue
                content = apk.read(name)
                abis.add(parts[1])
                libraries[name] = dict(sha256=hashlib.sha256(content).hexdigest(), size=len(content))
            if abis != set(expected_abis):
                raise ValueError('ANDROID_ABI_MISMATCH')
            if any(f'lib/{abi}/libnative-lib.so' not in libraries for abi in expected_abis):
                raise ValueError('ANDROID_NATIVE_LIBRARY_MISSING')
            return libraries
    except zipfile.BadZipFile:
        raise ValueError('ANDROID_APK_INVALID') from None


def check_cache(plan, require_cache=True):
    for key, entries in plan['fingerprints'].items():
        for name, expected in entries.items():
            path = input_file(Path(plan['paths'][key]), name, key)
            if not path.is_file() or digest(path) != expected:
                raise ValueError('INPUT_CHANGED')
    if not require_cache:
        return
    cache = safe_path(Path(plan['build_dir']) / 'CMakeCache.txt')
    if not cache.is_file():
        raise ValueError('MISSING_CMAKE_CACHE')
    values = dict(re.findall(r'^([^/#\r\n][^:\r\n]*):[^=\r\n]+=(.*)$', cache.read_text(encoding='utf-8'), re.M))
    expected = dict(plan['contract'], CMAKE_HOME_DIRECTORY=plan['root'])
    path_keys = {'CMAKE_HOME_DIRECTORY'}
    for field, key in [('googletest_source', 'FETCHCONTENT_SOURCE_DIR_GOOGLETEST'),
                       ('opencv_root', 'OPENCV_ROOT'), ('opencv_contrib_root', 'OPENCV_CONTRIB_ROOT')]:
        if field in plan['paths']:
            expected[key] = plan['paths'][field]
            path_keys.add(key)
    if 'windows_ncnn_dir' in plan['paths']:
        expected['ncnn_DIR'] = str(Path(plan['paths']['windows_ncnn_dir']) / 'lib/cmake/ncnn')
        path_keys.add('ncnn_DIR')
    for key, value in expected.items():
        actual = values.get(key, '').rstrip('\r')
        if key in path_keys:
            equal = bool(actual) and safe_path(actual) == safe_path(value)
        else:
            equal = actual == value
        if not equal:
            raise ValueError('CACHE_CONTRACT_' + key)


def check_android_cache(plan):
    root = safe_path(plan['build_dir']) / 'app/.cxx/Debug'
    expected_cmake = safe_path(Path(plan['paths']['android_sdk']) / 'cmake/3.22.1/bin/cmake.exe')
    ndk_properties = safe_path(Path(plan['paths']['android_ndk']) / 'source.properties')
    revision = re.search(r'(?m)^Pkg\.Revision\s*=\s*([^\s]+)\s*$',
                         ndk_properties.read_text(encoding='utf-8'))
    if not revision:
        raise ValueError('ANDROID_NDK_VERSION_UNVERIFIED')
    result = {}
    for abi in plan['abis']:
        expected = dict(CMAKE_HOME_DIRECTORY=plan['root'], ANDROID_ABI=abi, ANDROID_STL='c++_shared',
                        CMAKE_ANDROID_NDK=plan['paths']['android_ndk'],
                        CMAKE_COMMAND=str(expected_cmake),
                        RK_SKIP_OPENCV='ON' if plan['canonical'] == 'android-mock' else 'OFF',
                        RK_ENABLE_NCNN='OFF' if plan['canonical'] == 'android-mock' else 'ON',
                        RK_ENABLE_LIBYUV='OFF')
        if plan['canonical'] == 'android-real':
            expected.update(OPENCV_ROOT=plan['paths']['opencv_root'],
                            OPENCV_CONTRIB_ROOT=plan['paths']['opencv_contrib_root'],
                            ncnn_DIR=str(Path(plan['paths']['android_ncnn_dir']) / abi / 'lib/cmake/ncnn'))
        valid = False
        for cache in root.glob('*/' + abi + '/CMakeCache.txt'):
            values = dict(re.findall(r'^([^/#\r\n][^:\r\n]*):[^=\r\n]+=([^\r\n]*)$', safe_path(cache).read_text(encoding='utf-8'), re.M))
            def matches(key, value):
                actual = values.get(key, '')
                if key in ('CMAKE_HOME_DIRECTORY', 'CMAKE_ANDROID_NDK', 'OPENCV_ROOT',
                           'OPENCV_CONTRIB_ROOT', 'ncnn_DIR', 'CMAKE_COMMAND'):
                    return bool(actual) and safe_path(actual) == safe_path(value)
                return actual == value
            if all(matches(key, value) for key, value in expected.items()):
                result[abi] = dict(ndk_path=str(safe_path(values['CMAKE_ANDROID_NDK'])),
                                   ndk_version=revision.group(1),
                                   cmake_executable=str(safe_path(values['CMAKE_COMMAND'])))
                valid = True
                break
        if not valid:
            raise ValueError('ANDROID_CACHE_CONTRACT')
    return {'abis': result}


def pe_imports(filename):
    path = safe_path(filename)
    if path.stat().st_size > 256 * 1024 * 1024:
        raise ValueError('INVALID_PE')
    data = path.read_bytes()
    try:
        if data[:2] != b'MZ':
            raise ValueError()
        pe = struct.unpack_from('<I', data, 60)[0]
        if data[pe:pe + 4] != b'PE\0\0':
            raise ValueError()
        machine, count = struct.unpack_from('<HH', data, pe + 4)
        size = struct.unpack_from('<H', data, pe + 20)[0]
        optional = pe + 24
        if machine != 0x8664 or struct.unpack_from('<H', data, optional)[0] != 0x20b or size < 240 or not 0 < count < 128:
            raise ValueError()
        sections = []
        for index in range(count):
            length, address, raw_length, raw_address = struct.unpack_from('<IIII', data, optional + size + 40 * index + 8)
            sections.append((address, raw_length, raw_address))
        def offset(rva, length):
            matches = [raw + rva - address for address, raw_size, raw in sections if address <= rva and rva + length <= address + raw_size]
            if len(matches) != 1 or matches[0] + length > len(data):
                raise ValueError()
            return matches[0]
        def name(rva):
            start = offset(rva, 1)
            end = data.find(b'\0', start, min(len(data), start + 256))
            if end < 0:
                raise ValueError()
            value = data[start:end].decode('ascii')
            if not re.fullmatch(r'[A-Za-z0-9_.-]+\.dll', value, re.I):
                raise ValueError()
            return value
        output = []
        directory_count = struct.unpack_from('<I', data, optional + 108)[0]
        if not 2 <= directory_count <= 16:
            raise ValueError()
        for directory, width, name_offset in ((1, 20, 12), (13, 32, 4)):
            if directory >= directory_count:
                continue
            rva, length = struct.unpack_from('<II', data, optional + 112 + directory * 8)
            if not rva:
                if length:
                    raise ValueError()
                continue
            terminated = False
            for index in range(min(length // width, 4096)):
                entry = offset(rva + index * width, width)
                if not any(data[entry:entry + width]):
                    terminated = True
                    break
                if directory == 13 and struct.unpack_from('<I', data, entry)[0] != 1:
                    raise ValueError()
                output.append(name(struct.unpack_from('<I', data, entry + name_offset)[0]))
            if not terminated:
                raise ValueError()
        return sorted(set(output), key=str.casefold)
    except (ValueError, struct.error, UnicodeError, IndexError):
        raise ValueError('INVALID_PE') from None


def check_runtime(executable):
    imports = pe_imports(executable)
    system = None
    if os.name == 'nt':
        buffer = ctypes.create_unicode_buffer(32768)
        length = ctypes.windll.kernel32.GetSystemDirectoryW(buffer, len(buffer))
        if not 0 < length < len(buffer):
            raise ValueError('SYSTEM_DIRECTORY_UNVERIFIED')
        system = Path(buffer.value)
    for name in imports:
        lower = name.lower()
        if lower.startswith(('opencv_', 'ncnn', 'vcruntime', 'msvcp', 'msvcr', 'concrt')):
            raise ValueError('RUNTIME_DEPENDENCY')
        if lower in ('kernel32.dll', 'kernelbase.dll', 'ntdll.dll') or lower.startswith(('api-ms-win-', 'ext-ms-win-')):
            continue
        if system is None or not (system / name).is_file():
            raise ValueError('RUNTIME_DEPENDENCY')
    return imports


class _References(HTMLParser):
    def __init__(self):
        super().__init__()
        self.references = []
    def handle_starttag(self, tag, attrs):
        for key, value in attrs:
            if key in ('src', 'href') and value:
                self.references.append(value)


def check_web(directory, files):
    root = safe_path(directory)
    if 'index.html' not in files or not (root / 'index.html').read_text(encoding='utf-8').strip():
        raise ValueError('WEB_INDEX_MISSING')
    # 发布链不消费任意媒体或权重；当前前端只需代码、样式、矢量与字体资源。
    if any(Path(name).suffix.lower() not in ('.html', '.js', '.css', '.svg', '.json', '.txt', '.map', '.woff', '.woff2', '.ico') for name in files):
        raise ValueError('WEB_UNEXPECTED_FILE')
    parser = _References()
    parser.feed((root / 'index.html').read_text(encoding='utf-8'))
    for reference in parser.references:
        url = urlsplit(reference)
        if url.scheme or url.netloc or not url.path:
            continue
        relative = unquote(url.path).lstrip('/')
        if '\\' in relative or ':' in relative or '\0' in relative:
            raise ValueError('WEB_RESOURCE_PATH')
        path = safe_path(root / relative)
        if not path.is_relative_to(root) or path.relative_to(root).as_posix() not in files:
            raise ValueError('WEB_RESOURCE_MISSING')


def assemble_product(plan, executable, web, destination):
    destination = safe_path(destination)
    if destination.exists():
        raise ValueError('OUTPUT_EXISTS')
    web, executable = safe_path(web), safe_path(executable)
    web_files = inventory(web)
    check_web(web, web_files)
    dependencies = check_runtime(executable)
    root = safe_path(plan['root'])
    sources = {'win_local_service.exe': executable,
               'assets/lbpcascade_frontalface.xml': root / 'app/src/main/assets/lbpcascade_frontalface.xml',
               'config/windows_camera_face_recognition.ini': root / 'config/windows_camera_face_recognition.ini'}
    sources.update({'webroot/' + name: web / name for name in web_files})
    before = {name: dict(sha256=digest(source), size=source.stat().st_size) for name, source in sources.items()}
    destination.mkdir(parents=True, exist_ok=False)
    for name, source in sources.items():
        target = destination / name
        safe_path(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if digest(source) != before[name]['sha256'] or digest(target) != before[name]['sha256']:
            raise ValueError('SOURCE_CHANGED_DURING_COPY')
    if inventory(web) != web_files or any(digest(source) != before[name]['sha256'] for name, source in sources.items()):
        raise ValueError('SOURCE_CHANGED_DURING_COPY')
    manifest = dict(schema_version=1, directory=str(destination), identity=plan['identity'], kind='windows-product',
                    files=before, imports=dependencies)
    verify_manifest(manifest)
    return manifest


def verify_manifest(manifest):
    if manifest.get('schema_version') != 1 or not isinstance(manifest.get('files'), dict) or not manifest['files']:
        raise ValueError('INVALID_MANIFEST')
    directory = safe_path(manifest['directory'])
    actual = inventory(directory)
    if set(actual) != set(manifest['files']):
        raise ValueError('ARTIFACT_SET_CHANGED')
    if actual != manifest['files']:
        raise ValueError('ARTIFACT_CHANGED')
    if manifest.get('kind') == 'windows-product':
        check_runtime(directory / 'win_local_service.exe')
        check_web(directory / 'webroot', {name.removeprefix('webroot/'): value for name, value in actual.items() if name.startswith('webroot/')})
    if manifest.get('kind') == 'android-apk':
        profile = manifest.get('profile')
        run_id = manifest.get('run_id')
        if (profile not in ('android-mock', 'android-real') or directory.name != profile
                or directory.parent.name != 'product' or directory.parent.parent.name != run_id):
            raise ValueError('ANDROID_RUN_ID_MISMATCH')
        if set(actual) != {'app-debug.apk'}:
            raise ValueError('ANDROID_APK_FILE_SET')
        libraries = android_native_libraries(directory / 'app-debug.apk')
        if libraries != manifest.get('native_libraries'):
            raise ValueError('ANDROID_LIBRARY_MANIFEST')
        binding = manifest.get('android_build_binding')
        if profile == 'android-real':
            if not isinstance(binding, dict) or set(binding) != set(ANDROID_ABIS):
                raise ValueError('ANDROID_BUILD_BINDING_MISSING')
            for abi in ANDROID_ABIS:
                member = f'lib/{abi}/libnative-lib.so'
                entry = binding[abi]
                hash_fields = ('package_sha256', 'build_sha256', 'loadable_sha256',
                               'text_sha256', 'cmake_sha256', 'ninja_sha256')
                if (not isinstance(entry, dict)
                        or any(not isinstance(entry.get(key), str)
                               or not re.fullmatch('[a-f0-9]{64}', entry[key]) for key in hash_fields)
                        or entry['package_sha256'] != libraries[member]['sha256']):
                    raise ValueError('ANDROID_BUILD_BINDING_MISMATCH')
        elif binding is not None:
            raise ValueError('ANDROID_BUILD_BINDING_UNEXPECTED')
    return True
