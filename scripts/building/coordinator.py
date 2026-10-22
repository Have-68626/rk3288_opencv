"""固定构建阶段复用测试执行器；构建与验证分别签署。"""
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import sys
import zipfile

from audit._common import redact
from testing.catalog import native
from testing.environment import diagnostic
from testing.metadata import collect_versions
from testing.runner import RepoLock, Step, execute_step
from testing.resources import ResourceBudget, ResourceCoordinator
from testing.reports import verify_android_build
from .profiles import digest, safe_path, input_file, validate_build_time
from .artifacts import (check_cache, check_android_cache, inventory, check_web,
                        assemble_product, verify_manifest, android_native_libraries)


def record_json(value):
    def clean(item):
        if isinstance(item, dict):
            return {key: clean(value) for key, value in item.items()}
        if isinstance(item, list):
            return [clean(value) for value in item]
        if isinstance(item, str):
            return item if re.fullmatch('[a-f0-9]{64}', item) else redact(item)
        return item
    return json.dumps(clean(value), ensure_ascii=False, indent=2, allow_nan=False, default=str)


def source_binding(root):
    source = Path(__file__).resolve().parents[1] / 'run-test-suites.py'
    spec = importlib.util.spec_from_file_location('_build_source_binding', source)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.binding(root)


def child_environment(plan):
    paths = plan['paths']
    env = dict(COREPACK_ENABLE_NETWORK='0', CI='true', CYPRESS_COVERAGE='0')
    prefixes = []
    if 'node' in paths:
        prefixes.append(str(Path(paths['node']).parent))
    if 'java_home' in paths:
        env['JAVA_HOME'] = paths['java_home']
        prefixes.append(str(Path(paths['java_home']) / 'bin'))
    if prefixes:
        env['PATH'] = os.pathsep.join(prefixes + [os.environ.get('PATH', '')])
    if 'android_sdk' in paths:
        env.update(ANDROID_HOME=paths['android_sdk'], ANDROID_SDK_ROOT=paths['android_sdk'])
    if plan.get('gradle_user_home'):
        env['GRADLE_USER_HOME'] = plan['gradle_user_home']
    if plan['canonical'] == 'android-real':
        env['NCNN_DIR'] = paths['android_ncnn_dir']
    elif plan['canonical'] == 'android-mock':
        env['NCNN_DIR'] = str(Path(plan['build_dir']) / 'disabled-ncnn')
    env.update(TEMP=str(Path(plan['root']) / 'cache/reports/.tmp' / ('b-' + Path(plan['run_dir']).name[-10:])),
               TMP=str(Path(plan['root']) / 'cache/reports/.tmp' / ('b-' + Path(plan['run_dir']).name[-10:])))
    return env


def gradle_init(plan):
    def literal(value):
        return "'" + str(value).replace('\\', '/').replace("'", "\\'") + "'"
    base = Path(plan['build_dir'])
    return ("allprojects { p -> p.plugins.withId('com.android.application') {\n"
            "p.layout.buildDirectory.set(new File(" + literal(base / 'app/build') + "))\n"
            "p.android.externalNativeBuild.cmake.buildStagingDirectory = new File(" + literal(base / 'app/.cxx') + ")\n"
            "p.android.ndkPath = " + literal(plan['paths']['android_ndk']) + "\n} }\n")


def command_steps(action, plan):
    paths, profile = plan['paths'], plan['canonical']
    root, out, build = map(Path, (plan['root'], plan['run_dir'], plan['build_dir']))
    values = plan.get('resource_budget', {})
    budget = ResourceBudget(
        cpu_slots=values.get('cpu_slots', 2),
        memory_budget_mb=values.get('memory_budget_mb'),
    )
    env, result = child_environment(plan), []
    def add(phase, name, command, cwd=root, timeout=3600, junit=(), unit='case',
            cpu_slots=1, resources=()):
        result.append((phase, Step(name, list(map(str, command)), Path(cwd), timeout=timeout,
                                   env=dict(env), junit=tuple(map(str, junit)), unit=unit,
                                   cpu_slots=cpu_slots,
                                   memory_budget_mb=budget.memory_budget_mb,
                                   exclusive_resources=tuple(resources) or (f'output:{name}',),
                                   resource_budget=budget)))
    android = profile in ('android-mock', 'android-real')
    if android:
        gradle = [root / 'gradlew.bat', '--no-daemon', '--offline',
                  f'--max-workers={budget.cpu_slots}',
                  '--init-script', out / 'gradle.init.gradle']
        if plan.get('build_time'):
            gradle.append('-Prk.buildTime=' + plan['build_time'])
        if profile == 'android-mock':
            gradle += ['-PRK_SKIP_OPENCV=true']
        else:
            gradle += ['-POPENCV_ROOT=' + paths['opencv_root'], '-POPENCV_CONTRIB_ROOT=' + paths['opencv_contrib_root']]
    if action != 'verify':
        if profile in ('core', 'windows', 'android-cli'):
            command = [paths.get('cmake', 'cmake'), '-S', root, '-B', build, '-G', plan['contract']['CMAKE_GENERATOR']]
            if profile in ('core', 'windows'):
                command += ['-A', 'x64', '-DFETCHCONTENT_FULLY_DISCONNECTED=ON',
                            '-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=' + paths.get('googletest_source', '')]
            else:
                command += ['-DCMAKE_TOOLCHAIN_FILE=' + str(Path(paths['android_ndk']) / 'build/cmake/android.toolchain.cmake'),
                            '-DCMAKE_MAKE_PROGRAM=' + paths['ninja'], '-DCMAKE_BUILD_TYPE=Release']
            for key, value in plan['contract'].items():
                if key not in ('CMAKE_GENERATOR', 'CMAKE_GENERATOR_PLATFORM'):
                    command.append('-D' + key + '=' + value)
            command += ['-DRK_ENABLE_LIBYUV=OFF', '-DRK_NCNN_FETCHCONTENT=OFF']
            if profile != 'core':
                command += ['-DOPENCV_ROOT=' + paths['opencv_root'], '-DOPENCV_CONTRIB_ROOT=' + paths['opencv_contrib_root'],
                            '-DWITH_IPP=OFF', '-DOPENCV_DOWNLOAD_IPPICV=OFF', '-DWITH_FFMPEG=OFF', '-DOPENCV_FFMPEG_SKIP_DOWNLOAD=ON']
            if profile == 'windows':
                command += ['-DNCNN_DIR=' + paths['windows_ncnn_dir'], '-Dncnn_DIR=' + str(Path(paths['windows_ncnn_dir']) / 'lib/cmake/ncnn')]
            add('configure', profile + '-configure', command, timeout=1800,
                resources=(f'build-tree:{profile}',))
        elif android:
            add('configure', profile + '-configure', native(*gradle, *[':app:configureCMakeDebug[' + abi + ']' for abi in plan['abis']]), timeout=1800,
                cpu_slots=budget.cpu_slots, resources=('output:android-app-build',))
        if action == 'configure':
            return result
        if plan['profile'] == 'windows-product' or profile == 'web':
            web = Path(paths['web_dir'])
            add('build', 'web-typescript', [paths['node'], input_file(web, 'node_modules/typescript/bin/tsc', 'web_dir'), '-b'], cwd=web)
            add('build', 'web-bundle', [paths['node'], input_file(web, 'node_modules/vite/bin/vite.js', 'web_dir'),
                                       'build', '--outDir', plan['web_output']], cwd=web)
            add('build', 'web-budget', [paths['node'], root / 'web/scripts/check-bundle-budget.mjs', '--webroot', plan['web_output']], cwd=web)
        if profile in ('core', 'windows', 'android-cli'):
            add('build', profile + '-build', [paths.get('cmake', 'cmake'), '--build', build, '--config', 'Release',
                                             '--parallel', str(budget.cpu_slots), '--target', *plan['targets']], timeout=14400,
                cpu_slots=budget.cpu_slots, resources=(f'build-tree:{profile}',))
        elif android:
            add('build', profile + '-build', native(*gradle, *plan['targets']), timeout=7200,
                cpu_slots=budget.cpu_slots, resources=('output:android-app-build', 'output:android-apk'))
    elif profile == 'core':
        add('verify', 'core-tests', [paths['ctest'], '--test-dir', build, '-C', 'Release', '-R', '^core_unit_tests$',
                                    '--output-on-failure', '--output-junit', out / 'checks/core.xml'], junit=[out / 'checks/core.xml'], unit='target')
    elif profile == 'windows':
        add('verify', 'windows-native', [sys.executable, root / 'scripts/run-native-tests.py', '--ctest', paths['ctest'],
                                       '--build-dir', build, '--report-dir', out / 'checks/native'], junit=[out / 'checks/native/cases.xml'], unit='case_execution')
        result[-1][1].native_report = out / 'checks/native'
        if plan['profile'] == 'windows-product':
            add('verify', 'windows-web-contract', [paths['node'], root / 'scripts/testing/web-contract.cjs', root,
                paths['web_dir'], build / 'bin/Release/win_unit_tests.exe', out / 'checks/http'], junit=[out / 'checks/http/junit.xml'], timeout=90)
    elif android:
        add('verify', 'android-tests', native(*gradle, ':app:testDebugUnitTest', '--rerun', ':app:lintDebug'),
            junit=[build / 'app/build/test-results/testDebugUnitTest/TEST-*.xml'], timeout=3600,
            cpu_slots=budget.cpu_slots, resources=('output:android-test-results',))
    if action == 'verify' and (profile == 'web' or plan['profile'] == 'windows-product'):
        web = Path(paths['web_dir'])
        add('verify', 'web-budget', [paths['node'], root / 'web/scripts/check-bundle-budget.mjs', '--webroot',
                                    plan.get('verify_web_output', plan['web_output'])], cwd=web)
        add('verify', 'web-lint', native(paths['pnpm'], 'run', 'lint'), cwd=web)
        add('verify', 'web-tests', native(paths['pnpm'], 'run', 'test', '--outputFile.junit=' + str(out / 'checks/web.xml')),
            cwd=web, junit=[out / 'checks/web.xml'])
    return result


def _copy_outputs(sources, destination, identity):
    destination = safe_path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    files = {}
    for name, source in sources.items():
        source = safe_path(source)
        before = digest(source)
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if digest(source) != before or digest(target) != before:
            raise ValueError('OUTPUT_CHANGED_DURING_COPY')
        files[name] = dict(sha256=before, size=target.stat().st_size)
    return dict(schema_version=1, directory=str(destination), identity=identity, kind='build-outputs', files=files)


def record_outputs(plan):
    root, out, build = map(Path, (plan['root'], plan['run_dir'], plan['build_dir']))
    profile = plan['canonical']
    if plan['profile'] == 'windows-product':
        return assemble_product(plan, build / 'bin/Release/win_local_service.exe', plan['web_output'], out / 'product/windows')
    if profile == 'web':
        files = inventory(plan['web_output'])
        check_web(plan['web_output'], files)
        return dict(schema_version=1, directory=plan['web_output'], identity=plan['identity'], kind='web', files=files)
    if profile in ('android-mock', 'android-real'):
        apk = build / 'app/build/outputs/apk/debug/app-debug.apk'
        source_libraries = android_native_libraries(apk, plan['abis'])
        binding = verify_android_build(build, plan['paths']['android_ncnn_dir']) if profile == 'android-real' else None
        result = _copy_outputs({'app-debug.apk': apk}, out / ('product/' + profile), plan['identity'])
        snapshot_libraries = android_native_libraries(Path(result['directory']) / 'app-debug.apk', plan['abis'])
        if source_libraries != snapshot_libraries:
            raise ValueError('ANDROID_APK_CHANGED_DURING_COPY')
        result.update(kind='android-apk', profile=profile, run_id=out.name,
                      native_libraries=snapshot_libraries)
        if binding:
            if set(binding) != set(plan['abis']):
                raise ValueError('ANDROID_BUILD_BINDING_MISSING')
            for abi in plan['abis']:
                member = f'lib/{abi}/libnative-lib.so'
                if binding[abi].get('package_sha256') != snapshot_libraries[member]['sha256']:
                    raise ValueError('ANDROID_BUILD_BINDING_MISMATCH')
            result['android_build_binding'] = binding
        return result
    binaries = build / ('Release' if profile == 'core' else 'bin' if profile == 'android-cli' else 'bin/Release')
    return _copy_outputs({name + ('' if profile == 'android-cli' else '.exe'): binaries / (name + ('' if profile == 'android-cli' else '.exe'))
                          for name in plan['targets']}, out / ('product/' + profile), plan['identity'])


def _reason(error):
    message = str(error)
    return message if re.fullmatch('[A-Z][A-Z0-9_]{0,100}', message) else 'BUILD_EXECUTION_FAILED'


def record_android_toolchain(plan, out, tool_records):
    check_cache(plan, require_cache=False)
    toolchain = check_android_cache(plan)
    executables = {item['cmake_executable'] for item in toolchain['abis'].values()}
    if len(executables) != 1:
        raise ValueError('ANDROID_CMAKE_ABI_MISMATCH')
    executable = executables.pop()
    versions = {}
    collect_versions([dict(
        errors=[],
        tools={'android_cmake': executable},
        steps=[Step('android-cmake-version', [executable, '--version'], Path(plan['root']),
                    timeout=15, env=child_environment(plan))],
    )], out / 'toolchain', versions)
    match = re.search(r'(?m)^cmake version ([0-9]+(?:\.[0-9]+){1,2})(?:[-+][A-Za-z0-9._-]+)?\s*$',
                      versions['android_cmake']['version'])
    if not match:
        raise ValueError('ANDROID_CMAKE_VERSION_UNVERIFIED')
    cmake_version = match.group(1)
    if cmake_version != '3.22.1':
        raise ValueError('ANDROID_CMAKE_VERSION_MISMATCH')
    tool_records.update(versions)
    for item in toolchain['abis'].values():
        item['cmake_version'] = cmake_version
    return toolchain


def run(action, plan, prior_report=None):
    out = safe_path(plan['run_dir'])
    out.mkdir(parents=True, exist_ok=False)
    root = Path(plan['root'])
    values = plan.get('resource_budget', {})
    budget = ResourceBudget(
        cpu_slots=values.get('cpu_slots', 2),
        memory_budget_mb=values.get('memory_budget_mb'),
    )
    resource_coordinator = ResourceCoordinator(budget)
    report = dict(schema_version=1, action=action, profile=plan['profile'], status='blocked', exit_code=2,
                  reason='', plan=plan, steps=[], tools={},
                  build_time=plan.get('build_time'),
                  resource_budget=dict(cpu_slots=budget.cpu_slots,
                                       memory_budget_mb=budget.memory_budget_mb,
                                       max_parallel_steps=budget.max_parallel_steps),
                  max_parallel_steps_observed=0,
                  started=datetime.now(timezone.utc).isoformat())
    def save():
        temporary = out / 'summary.pending'
        with temporary.open('w', encoding='utf-8') as stream:
            stream.write(record_json(report))
        os.replace(temporary, out / 'summary.json')
        lines = ['# 构建统筹结果', '', '动作：' + action, '配置：' + plan['profile'], '状态：' + report['status']]
        if report.get('build_time'):
            lines.append('Android诊断构建时间：' + report['build_time'])
        lines += [
                 '原因：' + report['reason'],
                 f"预算：CPU {budget.cpu_slots}；内存 {budget.memory_budget_mb if budget.memory_budget_mb is not None else '仅观测'} MB；阶段并发上限 {budget.max_parallel_steps}",
                 f"实际阶段并发峰值：{report['max_parallel_steps_observed']}", '',
                 '| 阶段 | 状态 | 原始退出码 | 峰值进程 | 峰值内存字节 |', '|---|---|---|---:|---:|']
        for row in report['steps']:
            observed = row.get('resources', {}).get('observed', {})
            peak_processes = observed.get('peak_active_processes')
            peak_memory = observed.get('peak_job_memory_bytes')
            lines.append(
                f"| {row['name']} | {row['status']} | {row.get('raw_exit_code')} | "
                f"{peak_processes if peak_processes is not None else '不可用'} | "
                f"{peak_memory if peak_memory is not None else '不可用'} |"
            )
        if report.get('android_toolchain'):
            lines += ['', '## Android有效工具链', '', '| ABI | NDK版本 | CMake版本 | CMake执行文件 |', '|---|---|---|---|']
            for abi, item in report['android_toolchain']['abis'].items():
                lines.append(f"| {abi} | {item['ndk_version']} | {item['cmake_version']} | {item['cmake_executable']} |")
        lines += ['', '配置、构建、验证与设备验收分别判读；本报告不代表远端CI通过。']
        (out / 'summary.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    try:
        report['source_start'] = source_binding(root)
        save()
        with RepoLock(root):
            if plan['errors']:
                raise ValueError('PREFLIGHT_FAILED')
            check_cache(plan, require_cache=False)
            if action == 'verify':
                original = safe_path(prior_report)
                before = digest(original)
                old = json.loads(original.read_text(encoding='utf-8'))
                if old.get('status') != 'passed' or old.get('action') != 'build' or old.get('profile') != plan['profile']:
                    raise ValueError('PRIOR_BUILD_NOT_VERIFIED')
                if plan['canonical'] in ('android-mock', 'android-real'):
                    old_build_time = old.get('build_time')
                    if not old_build_time:
                        raise ValueError('PRIOR_BUILD_TIME_UNAVAILABLE')
                    validate_build_time(old_build_time)
                    if plan.get('build_time_explicit') and plan.get('build_time') != old_build_time:
                        raise ValueError('BUILD_TIME_MISMATCH')
                    plan['build_time'] = old_build_time
                    report['build_time'] = old_build_time
                if old.get('source_end') != report['source_start'] or old['plan']['identity'] != plan['identity']:
                    raise ValueError('PRIOR_BUILD_SOURCE_CHANGED')
                verify_manifest(old['artifacts'])
                report.update(artifacts=old['artifacts'], based_on=dict(path=str(original), sha256=before))
                if plan['canonical'] == 'web' or plan['profile'] == 'windows-product':
                    plan['verify_web_output'] = str(Path(old['artifacts']['directory']) / 'webroot') if plan['profile'] == 'windows-product' else old['artifacts']['directory']
            build = safe_path(plan['build_dir'])
            contract_file = build / 'build-contract.json'
            if contract_file.exists():
                if json.loads(contract_file.read_text(encoding='utf-8')).get('identity') != plan['identity']:
                    raise ValueError('BUILD_DIRECTORY_IDENTITY_MISMATCH')
            elif (build / 'CMakeCache.txt').exists():
                raise ValueError('UNMANAGED_BUILD_DIRECTORY')
            if (build / 'CMakeCache.txt').is_file():
                check_cache(plan)
            if action != 'verify':
                build.mkdir(parents=True, exist_ok=True)
                if not contract_file.exists():
                    with contract_file.open('x', encoding='utf-8') as stream:
                        stream.write(record_json(dict(identity=plan['identity'], contract=plan['identity_data'])))
            (out / 'checks').mkdir()
            if plan['canonical'] in ('android-mock', 'android-real'):
                (out / 'gradle.init.gradle').write_text(gradle_init(plan), encoding='utf-8')
            steps = command_steps(action, plan)
            tools = {key: path for key, path in plan['paths'].items() if key in ('cmake', 'ctest', 'node', 'pnpm', 'ninja')}
            if 'java_home' in plan['paths']:
                tools['java'] = str(Path(plan['paths']['java_home']) / 'bin/java.exe')
            first = steps[0][1] if steps else Step('preflight', [sys.executable, '-V'], root, env=child_environment(plan))
            collect_versions([dict(errors=[], tools=tools, steps=[first])], out / 'versions', report['tools'])
            if 'pnpm' in report['tools'] and report['tools']['pnpm']['version'].strip() != '10.33.0':
                raise ValueError('PNPM_VERSION_MISMATCH')
            report['steps'] = [dict(name=step.name, phase=phase, status='not_run', raw_exit_code=None) for phase, step in steps]
            report.update(status='running', exit_code=0)
            save()
            for index, (phase, step) in enumerate(steps):
                print('START build=' + plan['profile'] + ' step=' + step.name, flush=True)
                report['steps'][index]['status'] = 'running'
                save()
                with resource_coordinator.acquire(step.resource_claim):
                    report['max_parallel_steps_observed'] = max(
                        report['max_parallel_steps_observed'], 1
                    )
                    result = execute_step(step, out / 'steps' / step.name)
                report['steps'][index].update(result)
                save()
                print('END step=' + step.name + ' status=' + result['status'], flush=True)
                if result['status'] != 'passed':
                    report.update(status=result['status'], exit_code=130 if result['status'] == 'cancelled' else 1,
                                  reason=result.get('reason', 'STEP_FAILED'))
                    break
                if phase == 'configure' and plan['canonical'] in ('core', 'windows', 'android-cli'):
                    check_cache(plan)
                if phase == 'configure' and plan['canonical'] in ('android-mock', 'android-real'):
                    report['android_toolchain'] = record_android_toolchain(plan, out, report['tools'])
            else:
                check_cache(plan, require_cache=plan['canonical'] in ('core', 'windows', 'android-cli'))
                if plan['canonical'] in ('android-mock', 'android-real'):
                    if not report.get('android_toolchain'):
                        report['android_toolchain'] = record_android_toolchain(plan, out, report['tools'])
                if action == 'build':
                    report['artifacts'] = record_outputs(plan)
                    verify_manifest(report['artifacts'])
                if action == 'verify':
                    verify_manifest(report['artifacts'])
                    if digest(original) != before:
                        raise ValueError('PRIOR_REPORT_CHANGED')
                report.update(status='passed', exit_code=0)
    except KeyboardInterrupt:
        report.update(status='cancelled', exit_code=130, reason='CANCELLED')
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, zipfile.BadZipFile) as error:
        report.update(status='blocked', exit_code=2, reason=_reason(error), diagnostic=diagnostic(error, action))
    finally:
        for step in report['steps']:
            if step['status'] == 'running':
                step['status'] = 'cancelled' if report['exit_code'] == 130 else 'blocked'
        try:
            report['source_end'] = source_binding(root)
            if report.get('source_start') != report['source_end']:
                report['source_changed'] = True
                if report['exit_code'] != 130:
                    report.update(status='unverified', exit_code=2, reason='SOURCE_CHANGED')
        except (OSError, ValueError, RuntimeError):
            report.update(status='unverified', exit_code=2, reason='SOURCE_BINDING_UNAVAILABLE')
        save()
    return report
