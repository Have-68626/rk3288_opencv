"""收集双ABI原生库并核对编译选项；不把单份config.h当成所有变体证据。"""
from pathlib import Path
import re
import shutil

from _common import ABIS, Parser, ToolError, check_output, lock, regular_files, run_cli, validate_libraries, write_report

OPTIONS = [b'--enable-libx264', b'--enable-openssl', b'--enable-gpl', b'--enable-version3']


def check_capabilities(libraries):
    rows = {}
    for abi in ABIS:
        checks = {}
        for name in ['libavcodec.so', 'libavformat.so']:
            data = (Path(libraries) / abi / name).read_bytes()
            checks[name] = all(option in data for option in OPTIONS)
            if not checks[name]:
                raise ToolError('NATIVE_BUILD_OPTIONS_MISSING')
        rows[abi] = checks
    return rows


def collect(source, ndk, output):
    check_output(output)
    source, ndk = Path(source), Path(ndk)
    properties = (ndk / 'source.properties').read_text(encoding='utf8')
    if not re.search(r'^Pkg.Revision\s*=\s*' + re.escape(lock()['ndk_version']) + r'\s*$', properties, re.M):
        raise ToolError('NDK_VERSION_MISMATCH')
    hosts = list((ndk / 'toolchains/llvm/prebuilt').iterdir())
    if len(hosts) != 1:
        raise ToolError('NDK_HOST_AMBIGUOUS')
    platform = {abi: sorted(p.name for p in (hosts[0] / 'sysroot/usr/lib' / triple / '21').glob('*.so'))
                for abi, triple in [('armeabi-v7a', 'arm-linux-androideabi'), ('arm64-v8a', 'aarch64-linux-android')]}
    libraries = source / 'android/libs'
    rows = validate_libraries(libraries, platform)
    flags = check_capabilities(libraries)
    resources = source / 'android/ffmpeg-kit-android-lib/src/main/res'
    regular_files(resources)
    output = Path(output)
    output.mkdir(parents=True)
    shutil.copytree(libraries, output / 'libs')
    shutil.copytree(resources, output / 'res')
    write_report(output / 'platform-libraries.json', platform)
    report = {'runtime_verified': False, 'native_libraries': rows, 'compile_options_by_abi': flags,
              'scope': '静态构建选项与依赖核对；不证明所有协议或设备运行时能力'}
    write_report(output / 'capabilities.json', report)
    return report


def main():
    parser = Parser(description=__doc__)
    for name in ['source-dir', 'ndk-root', 'output']:
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    report = collect(args.source_dir, args.ndk_root, args.output)
    print('NATIVE_LIBRARIES=' + str(len(report['native_libraries'])))
    return 0


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
