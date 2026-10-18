"""显式使用Android引导类库，重新编译FFmpegKit与smart-exception的Java8类。"""
import importlib.util
from pathlib import Path
import struct
import zipfile

from _common import Parser, ToolError, check_output, extract_zip, lock, regular_files, run_checked, run_cli, sha256, write_report

REQUIRED_CLASSES = {'com/arthenica/ffmpegkit/FFmpegKit.class',
                    'com/arthenica/smartexception/AbstractExceptions.class'}


def inspect_classes(jar):
    try:
        with zipfile.ZipFile(jar) as source:
            names = source.namelist()
            if len(names) != len(set(names)) or not REQUIRED_CLASSES <= set(names):
                raise ToolError('JAVA_CLASSES_MISSING')
            classes = [name for name in names if name.endswith('.class')]
            for name in classes:
                data = source.read(name)
                if len(data) < 8 or data[:4] != b'\xca\xfe\xba\xbe' or struct.unpack_from('>H', data, 6)[0] != 52:
                    raise ToolError('JAVA_CLASS_VERSION_MISMATCH')
            return {'classes': len(classes), 'java_class_version': 52, 'sha256': sha256(jar)}
    except zipfile.BadZipFile:
        raise ToolError('INVALID_CLASSES_JAR') from None


def build(source, common_jar, java_jar, android_jar, javac, output):
    check_output(output)
    source = Path(source)
    spec = importlib.util.spec_from_file_location('_ffmpegkit_prepare', Path(__file__).with_name('prepare-native.py'))
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    prepare.verify(source)
    expected = lock()['java_source_sha256']
    if sha256(common_jar) != expected['common'] or sha256(java_jar) != expected['java']:
        raise ToolError('JAVA_SOURCE_DIGEST_MISMATCH')
    with zipfile.ZipFile(android_jar) as android:
        if 'android/content/Context.class' not in android.namelist():
            raise ToolError('ANDROID_BOOTCLASSPATH_REQUIRED')
    output = Path(output)
    output.mkdir(parents=True)
    extract_zip(common_jar, output / 'common-source', expected['common'])
    extract_zip(java_jar, output / 'java-source', expected['java'])
    sources = [p for directory in [source / 'android/ffmpeg-kit-android-lib/src/main/java',
                                  output / 'common-source', output / 'java-source']
               for p in regular_files(directory) if p.suffix == '.java']
    if not sources:
        raise ToolError('JAVA_SOURCES_MISSING')
    classes = output / 'classes'
    classes.mkdir()
    def quote(value):
        value = str(value).replace('\\', '/')
        if '\n' in value or '\r' in value:
            raise ToolError('INVALID_JAVA_PATH')
        return '"' + value.replace('"', '\\"') + '"'
    arguments = ['-encoding', 'UTF-8', '-source', '8', '-target', '8', '-proc:none',
                 '-bootclasspath', quote(Path(android_jar).resolve()), '-d', quote(classes.resolve())]
    arguments += [quote(p.resolve()) for p in sources]
    argument_file = output / 'javac.args'
    argument_file.write_text('\n'.join(arguments) + '\n', encoding='utf8')
    run_checked([javac, '@' + str(argument_file.resolve())])
    jar = output / 'classes.jar'
    with zipfile.ZipFile(jar, 'x', zipfile.ZIP_DEFLATED) as archive:
        for path in regular_files(classes):
            archive.write(path, path.relative_to(classes).as_posix())
    report = inspect_classes(jar)
    report.update({'android_bootclasspath_only': True, 'source_files': len(sources), 'runtime_verified': False})
    write_report(output / 'verification.json', report)
    return report


def main():
    parser = Parser(description=__doc__)
    for name in ['source-dir', 'common-sources', 'java-sources', 'android-jar', 'javac', 'output']:
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    report = build(args.source_dir, args.common_sources, args.java_sources, args.android_jar, args.javac, args.output)
    print('JAVA_CLASSES=' + str(report['classes']))
    return 0


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
