"""静态校验双ABI与Java8后组装新的AAR，不覆盖消费件。"""
import importlib.util
import io
import json
from pathlib import Path
import zipfile

from _common import Parser, ToolError, check_output, regular_files, run_cli, sha256, validate_libraries, write_report

_spec = importlib.util.spec_from_file_location('_ffmpegkit_java', Path(__file__).with_name('build-java.py'))
_java = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_java)


def bundle(libraries, classes, resources, consumer_rules, licenses, output, platform):
    check_output(output)
    rows = validate_libraries(libraries, platform)
    java = _java.inspect_classes(classes)
    if not licenses or any(not Path(p).is_file() or not Path(p).stat().st_size for p in licenses):
        raise ToolError('LICENSE_INPUTS_REQUIRED')
    files = regular_files(resources)
    if not any('license' in p.name.lower() for p in files):
        raise ToolError('NATIVE_LICENSES_MISSING')
    content = {
        'AndroidManifest.xml': b'<?xml version="1.0" encoding="utf-8"?><manifest xmlns:android="http://schemas.android.com/apk/res/android" package="com.arthenica.ffmpegkit"><uses-sdk android:minSdkVersion="21" /></manifest>',
        'classes.jar': Path(classes).read_bytes(), 'R.txt': b'',
        'proguard.txt': Path(consumer_rules).read_bytes(),
    }
    for path in licenses:
        name = 'assets/licenses/' + Path(path).name
        if name in content:
            raise ToolError('LICENSE_NAME_COLLISION')
        content[name] = Path(path).read_bytes()
    for path in files:
        if not path.name.startswith('.'):
            content['res/' + path.relative_to(resources).as_posix()] = path.read_bytes()
    for row in rows:
        content['jni/' + row['abi'] + '/' + row['file']] = (Path(libraries) / row['abi'] / row['file']).read_bytes()
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(content.items()):
            info = zipfile.ZipInfo(name, date_time=(2021, 1, 1, 0, 0, 0))
            archive.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
    # 所有输入校验结束后再独占创建输出，失败不留半个AAR。
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    with Path(output).open('xb') as target:
        target.write(buffer.getvalue())
    return {'schema_version': 1, 'sha256': sha256(output), 'bytes': Path(output).stat().st_size,
            'min_sdk': 21, 'armv7_requires_neon': True, 'runtime_verified': False,
            'java': java, 'native_libraries': rows,
            'license': 'GPL-3.0-or-later（启用x264）', 'scope': '静态ELF/依赖/Java与打包校验，不是设备验收'}


def main():
    parser = Parser(description=__doc__)
    for name in ['native-libs', 'classes-jar', 'resources', 'consumer-rules', 'platform-libraries', 'output', 'report']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--licenses', nargs='+', required=True)
    args = parser.parse_args()
    output, report_path = Path(args.output).resolve(), Path(args.report).resolve()
    if output == report_path or output in report_path.parents or report_path in output.parents:
        raise ToolError('OUTPUT_REPORT_OVERLAP')
    check_output(args.report)
    report = bundle(args.native_libs, args.classes_jar, args.resources, args.consumer_rules,
                    args.licenses, args.output, json.loads(Path(args.platform_libraries).read_text(encoding='utf8')))
    write_report(args.report, report)
    print('AAR_SHA256=' + report['sha256'])
    return 0


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
