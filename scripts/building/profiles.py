"""公共契约不接受本机JSON覆写，避免同一目录混用工具链与后端。"""
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from testing.catalog import installed_pnpm

PROFILES = ('core', 'windows', 'android-mock', 'android-real', 'android-cli', 'web', 'windows-product')
TOOLS = {'cmake', 'ctest', 'node', 'pnpm', 'ninja'}
FILES = {
    'googletest_source': ('CMakeLists.txt',),
    'opencv_root': ('CMakeLists.txt', 'modules/core/include/opencv2/core/version.hpp'),
    'opencv_contrib_root': ('modules/face/CMakeLists.txt',),
    'windows_ncnn_dir': ('lib/cmake/ncnn/ncnnConfig.cmake', 'lib/ncnn.lib'),
    'android_ncnn_dir': tuple(path for abi in ('armeabi-v7a', 'arm64-v8a')
                              for path in (abi + '/lib/cmake/ncnn/ncnnConfig.cmake', abi + '/lib/libncnn.a')),
    'android_ndk': ('source.properties', 'build/cmake/android.toolchain.cmake'),
    'android_sdk': ('platforms/android-34/android.jar', 'cmake/3.22.1/bin/cmake.exe'),
    'java_home': ('release', 'bin/java.exe'),
    'web_dir': ('package.json', 'pnpm-lock.yaml', 'node_modules/.modules.yaml',
                'node_modules/typescript/bin/tsc', 'node_modules/vite/bin/vite.js'),
}
REQUIRED = {
    'core': ('cmake', 'ctest', 'googletest_source'),
    'windows': ('cmake', 'ctest', 'googletest_source', 'opencv_root', 'opencv_contrib_root', 'windows_ncnn_dir'),
    'android-mock': ('java_home', 'android_sdk', 'android_ndk'),
    'android-real': ('java_home', 'android_sdk', 'android_ndk', 'opencv_root', 'opencv_contrib_root', 'android_ncnn_dir'),
    'android-cli': ('cmake', 'ninja', 'android_ndk', 'opencv_root', 'opencv_contrib_root'),
    'web': ('node', 'pnpm', 'web_dir'),
}
REQUIRED['windows-product'] = tuple(dict.fromkeys(REQUIRED['windows'] + REQUIRED['web']))
WINDOWS_TARGETS = ['win_local_service', 'core_unit_tests', 'win_unit_tests', 'face_infer_unit_tests', 'core_gtest_tests']


def validate_build_time(value):
    if not isinstance(value, str) or not re.fullmatch(r'\d{8}-\d{4}', value):
        raise ValueError('BUILD_TIME_INVALID')
    try:
        datetime.strptime(value, '%Y%m%d-%H%M')
    except ValueError:
        raise ValueError('BUILD_TIME_INVALID') from None
    return value


def safe_path(value):
    path = Path(value).absolute()
    for member in (path, *path.parents):
        try:
            status = member.lstat()
        except FileNotFoundError:
            continue
        if member.is_symlink() or getattr(status, 'st_file_attributes', 0) & 0x400:
            raise ValueError('LINK_PATH')
    return path.resolve()


def digest(path):
    path = safe_path(path)
    value = hashlib.sha256()
    with path.open('rb') as stream:
        while chunk := stream.read(1024 * 1024):
            value.update(chunk)
    return value.hexdigest()


def input_file(directory, name, key):
    candidate = directory / name if name else directory
    if key == 'web_dir' and name in ('node_modules/typescript/bin/tsc', 'node_modules/vite/bin/vite.js'):
        # pnpm自身的包链接是标准布局；只允许落在本项目的实体虚拟存储内。
        resolved = candidate.resolve()
        if resolved != candidate.absolute():
            store = safe_path(directory / 'node_modules/.pnpm')
            if not resolved.is_relative_to(store):
                raise ValueError('LINK_PATH')
        return safe_path(resolved)
    return safe_path(candidate)


def _pairs(items):
    output = {}
    for key, value in items:
        if key in output:
            raise ValueError('DUPLICATE_JSON_FIELD')
        output[key] = value
    return output


def load_inputs(filename):
    path = safe_path(filename)
    if path.stat().st_size > 65536:
        raise ValueError('INPUT_TOO_LARGE')
    data = json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=_pairs,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError('INVALID_JSON')))
    if not isinstance(data, dict) or set(data) != {'schema_version', 'paths'}:
        raise ValueError('INPUT_FIELDS')
    if type(data['schema_version']) is not int or data['schema_version'] != 1 or not isinstance(data['paths'], dict):
        raise ValueError('INPUT_SCHEMA')
    if set(data['paths']) - TOOLS - set(FILES):
        raise ValueError('INPUT_FIELDS')
    output = {}
    for key, value in data['paths'].items():
        if not isinstance(value, str) or not value or any(c in value for c in '\r\n\0'):
            raise ValueError('INVALID_INPUT_PATH')
        candidate = Path(value)
        output[key] = str(safe_path(candidate if candidate.is_absolute() else path.parent / candidate))
    return output


def cached_gradle(root):
    wrapper = root / 'gradle/wrapper/gradle-wrapper.properties'
    if not wrapper.is_file():
        return None, None, 'GRADLE_WRAPPER_MISSING'
    text = wrapper.read_text(encoding='utf-8')
    match = re.search(r'^distributionUrl=(.*)$', text, re.M)
    url = match.group(1).strip().replace('\\:', ':') if match else ''
    if url != 'https://services.gradle.org/distributions/gradle-9.0-bin.zip':
        return None, None, 'GRADLE_WRAPPER_VERSION_MISMATCH'
    home = safe_path(os.environ.get('GRADLE_USER_HOME') or Path.home() / '.gradle')
    # 与Wrapper的URL-MD5/base36布局一致，只接受已解包且有完成标记的发行包。
    value, code = int.from_bytes(hashlib.md5(url.encode('utf-8')).digest(), 'big'), ''
    while value:
        value, digit = divmod(value, 36)
        code = '0123456789abcdefghijklmnopqrstuvwxyz'[digit] + code
    base = home / 'wrapper/dists/gradle-9.0-bin' / code
    programs = list(base.glob('gradle-9.0*/bin/gradle.bat'))
    if len(programs) != 1 or not (base / 'gradle-9.0-bin.zip.ok').is_file():
        return home, None, 'GRADLE_DISTRIBUTION_MISSING'
    return home, safe_path(programs[0]), None


def make_plan(root, profile, inputs, run_dir, build_time=None):
    if profile not in PROFILES:
        raise ValueError('UNKNOWN_PROFILE')
    if set(inputs) - TOOLS - set(FILES):
        raise ValueError('INPUT_FIELDS')
    root, run_dir = safe_path(root), safe_path(run_dir)
    paths, fingerprints, errors = {}, {}, []
    for key in REQUIRED[profile]:
        value = inputs.get(key)
        if not value:
            errors.append('MISSING_' + key.upper())
            continue
        path = safe_path(value)
        paths[key] = str(path)
        names = list(('',) if key in TOOLS else FILES[key])
        if key in ('windows_ncnn_dir', 'android_ncnn_dir'):
            roots = [path] if key == 'windows_ncnn_dir' else [path / abi for abi in ('armeabi-v7a', 'arm64-v8a')]
            for directory in roots:
                for imported in (directory / 'lib/cmake').rglob('*.cmake'):
                    relative = imported.relative_to(path).as_posix()
                    if relative not in names:
                        names.append(relative)
                suffix = '*.lib' if key == 'windows_ncnn_dir' else '*.a'
                for library in (directory / 'lib').rglob(suffix):
                    relative = library.relative_to(path).as_posix()
                    if relative not in names:
                        names.append(relative)
        entries = {}
        for name in names:
            source = input_file(path, name, key)
            if not source.is_file():
                errors.append('MISSING_' + key.upper())
            else:
                entries[name] = digest(source)
        fingerprints[key] = entries
        if key in TOOLS and path.name.lower() not in (key + '.exe', key + '.cmd'):
            errors.append('INVALID_' + key.upper() + '_TOOL')
    if 'java_home' in paths and 'release' in fingerprints['java_home']:
        if not re.search(r'JAVA_VERSION="21[.\"]', Path(paths['java_home'], 'release').read_text(encoding='utf-8')):
            errors.append('JAVA_VERSION_MISMATCH')
    if 'web_dir' in paths and 'node_modules/.modules.yaml' in fingerprints['web_dir']:
        web = Path(paths['web_dir'])
        if installed_pnpm((web / 'node_modules/.modules.yaml').read_text(encoding='utf-8')) != 'pnpm@10.33.0':
            errors.append('WEB_DEPENDENCY_VERSION_MISMATCH')
        tracked = subprocess.run(['git', 'ls-files', '-z', '--', 'web'], cwd=root, capture_output=True)
        if tracked.returncode:
            errors.append('WEB_SOURCE_UNVERIFIED')
        else:
            for name in filter(None, tracked.stdout.decode('utf-8').split('\0')):
                source = root / name
                copy = web / source.relative_to(root / 'web')
                if not safe_path(copy).is_file() or digest(source) != digest(copy):
                    errors.append('WEB_SOURCE_MISMATCH')
                    break
    canonical = 'windows' if profile == 'windows-product' else profile
    if build_time is not None and canonical not in ('android-mock', 'android-real'):
        raise ValueError('BUILD_TIME_ANDROID_APP_ONLY')
    build_time_explicit = build_time is not None
    if canonical in ('android-mock', 'android-real'):
        build_time = validate_build_time(build_time) if build_time_explicit else datetime.now().strftime('%Y%m%d-%H%M')
    gradle_home = None
    if canonical in ('android-mock', 'android-real'):
        gradle_home, program, error = cached_gradle(root)
        if error:
            errors.append(error)
        else:
            for key, path in [('gradle_distribution', program), ('gradle_wrapper', root / 'gradle/wrapper/gradle-wrapper.properties')]:
                paths[key], fingerprints[key] = str(path), {'': digest(path)}
    contract = {'RK_SKIP_OPENCV': 'ON' if canonical in ('core', 'android-mock') else 'OFF'}
    abis, targets = [], []
    if canonical in ('core', 'windows'):
        contract.update(CMAKE_GENERATOR='Visual Studio 17 2022', CMAKE_GENERATOR_PLATFORM='x64',
                        CMAKE_MSVC_RUNTIME_LIBRARY='MultiThreaded$<$<CONFIG:Debug>:Debug>')
        targets = ['core_unit_tests'] if canonical == 'core' else list(WINDOWS_TARGETS)
    if canonical == 'windows':
        contract.update(BUILD_SHARED_LIBS='OFF', RK_ENABLE_NCNN='ON', RK_COPY_DEV_MODELS='OFF')
        source = root / 'cmake/windows.cmake'
        text = source.read_text(encoding='utf-8') if source.is_file() else ''
        if 'option(RK_COPY_DEV_MODELS' not in text or 'if(RK_COPY_DEV_MODELS AND EXISTS' not in text:
            errors.append('CORE_PATCH_REQUIRED')
    if canonical.startswith('android-'):
        abis = ['armeabi-v7a'] if canonical == 'android-cli' else ['armeabi-v7a', 'arm64-v8a']
        contract.update(ANDROID_STL='c++_static' if canonical == 'android-cli' else 'c++_shared')
        if canonical == 'android-cli':
            contract.update(CMAKE_GENERATOR='Ninja', ANDROID_ABI='armeabi-v7a', ANDROID_PLATFORM='android-21')
            targets = ['opencv_verify_cli']
        else:
            targets = [':app:assembleDebug', ':app:checkApkFaceModels']
    native_keys = REQUIRED[canonical] + (('gradle_distribution', 'gradle_wrapper') if canonical in ('android-mock', 'android-real') else ())
    identity_data = dict(root=str(root), profile=canonical, contract=contract,
                         paths={k: paths[k] for k in native_keys if k in paths},
                         fingerprints={k: fingerprints[k] for k in native_keys if k in fingerprints})
    identity = hashlib.sha256(json.dumps(identity_data, sort_keys=True).encode('utf-8')).hexdigest()
    build = root / 'cache/builds/build-coordination' / (canonical + '-' + identity[:12])
    safe_path(build)
    return dict(schema_version=1, profile=profile, canonical=canonical, root=str(root),
                paths=paths, fingerprints=fingerprints, contract=contract, identity=identity,
                identity_data=identity_data, build_dir=str(build), run_dir=str(run_dir),
                web_output=str(run_dir / 'web-output'), gradle_user_home=str(gradle_home) if gradle_home else None,
                targets=targets, abis=abis, build_time=build_time,
                build_time_explicit=build_time_explicit, errors=sorted(set(errors)))
