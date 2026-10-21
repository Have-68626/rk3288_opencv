"""分别读取原生目标和内部框架证据，不把目标通过数充当用例数。"""
import os
from pathlib import Path
import re
import subprocess
import xml.etree.ElementTree as ET

from audit._common import redact
from .reports import identity, read_junit
from .redaction import safe_json

TARGETS = ('core_unit_tests', 'win_unit_tests', 'face_infer_unit_tests',
           'core_gtest_tests', 'engine_run_reentry_behavior')
BOOL_TARGETS = ('win_unit_tests', 'face_infer_unit_tests')


def xml_root(path):
    identity(path)
    text = Path(path).read_text(encoding='utf-8-sig')
    if '<!DOCTYPE' in text.upper() or '<!ENTITY' in text.upper():
        raise ValueError('UNSAFE_XML')
    try:
        root = ET.fromstring(text)
    except ET.ParseError:
        raise ValueError('INVALID_NATIVE_XML') from None
    if root.tag not in ('testsuites', 'testsuite'):
        raise ValueError('INVALID_NATIVE_XML')
    return root


def parse_target(target, ctest, google, *, expected_name=True):
    if target not in TARGETS:
        raise ValueError('INVALID_NATIVE_TARGET')
    read_junit([ctest], 'target')
    root = xml_root(ctest)
    entries = list(root.iter('testcase'))
    if len(entries) != 1 or (expected_name and entries[0].get('name') != target):
        raise ValueError('NATIVE_TARGET_MISMATCH')
    text = '\n'.join(e.text or '' for e in root.iter('system-out'))
    cases = []
    if target in BOOL_TARGETS:
        found = re.findall(r'^TEST_(PASS|FAIL) name=([^\r\n]+)$', text, re.M)
        summaries = re.findall(r'^TEST_SUMMARY pass=(\d+) fail=(\d+) total=(\d+)\s*$', text, re.M)
        if len(summaries) != 1 or not found or len({n for _, n in found}) != len(found):
            raise ValueError('INVALID_BOOL_SUMMARY')
        passed, failed, total = map(int, summaries[0])
        if (passed, failed, total) != (sum(s == 'PASS' for s, _ in found), sum(s == 'FAIL' for s, _ in found), len(found)):
            raise ValueError('CONTRADICTORY_BOOL_SUMMARY')
        cases += [dict(identity=f'{target}:bool:{name}', name=name, framework='bool',
                       status='passed' if status == 'PASS' else 'failed') for status, name in found]
    disabled = 0
    if target != 'face_infer_unit_tests':
        if not Path(google).is_file():
            raise ValueError('MISSING_GTEST')
        google_root = xml_root(google)
        read_junit([google])
        executable = 'core_gtest_tests' if target == 'engine_run_reentry_behavior' else target
        seen = set()
        for case in google_root.iter('testcase'):
            name = case.get('classname', '') + '.' + case.get('name', '')
            if not case.get('name') or name in seen:
                raise ValueError('INVALID_GTEST_CASE')
            seen.add(name)
            markers = [case.find(tag) is not None for tag in ('failure', 'error', 'skipped')]
            if case.get('status') == 'notrun':
                if any(markers):
                    raise ValueError('CONTRADICTORY_GTEST_CASE')
                disabled += 1
                continue
            if sum(markers) > 1:
                raise ValueError('CONTRADICTORY_GTEST_CASE')
            status = 'failed' if any(markers[:2]) else 'skipped' if markers[2] else 'passed'
            cases.append(dict(identity=f'{executable}:gtest:{name}', name=name,
                              framework='gtest', status=status))
        if not any(c['framework'] == 'gtest' for c in cases):
            raise ValueError('EMPTY_GTEST')
    if not cases:
        raise ValueError('EMPTY_NATIVE_CASES')
    return dict(target=target, cases=cases, disabled=disabled,
                asset_unverified='TEST_SKIP_ABSENT' in text or 'TEST_UNVERIFIED' in text)


def run_native(build, output, ctest, config='Release'):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    reports = []
    overall = 0
    for target in TARGETS:
        directory = output/target
        directory.mkdir()
        target_xml, google = directory/'ctest.xml', directory/'gtest.xml'
        environment = os.environ.copy()
        # 每个CTest独占输出，重入目标不能覆盖默认GoogleTest报告。
        environment['GTEST_OUTPUT'] = 'xml:' + str(google)
        command = [str(ctest), '--test-dir', str(build), '-C', config, '-R', '^'+target+'$',
                   '--output-on-failure', '--test-output-size-passed', '16777216',
                   '--test-output-size-failed', '16777216', '--output-junit', str(target_xml)]
        process = subprocess.run(command, env=environment, capture_output=True,
                                 text=True, encoding='utf-8', errors='replace')
        print(redact(process.stdout + process.stderr), flush=True)
        item = dict(target=target, raw_exit_code=process.returncode)
        try:
            item.update(parse_target(target, target_xml, google))
            target_counts = read_junit([target_xml], 'target')
            failed = any(c['status'] == 'failed' for c in item['cases'])
            if (failed or target_counts['failed']) and process.returncode == 0:
                raise ValueError('NATIVE_FAILURE_EXIT_MISMATCH')
            item['status'] = 'passed' if process.returncode == 0 else 'failed'
            if process.returncode:
                overall = overall or 1
        except (OSError, ValueError, ET.ParseError):
            item.update(status='unverified', reason='INVALID_NATIVE_EVIDENCE')
            overall = 2
        for path in (target_xml, google):
            if path.is_file():
                try:
                    document=xml_root(path)
                    for node in document.iter():
                        if node.text:node.text=redact(node.text)
                        if node.tail:node.tail=redact(node.tail)
                        node.attrib={key:redact(value) for key,value in node.attrib.items()}
                    ET.ElementTree(document).write(path,encoding='utf-8',xml_declaration=True)
                except (ValueError,ET.ParseError):
                    path.write_text('<invalid-native-evidence/>',encoding='utf-8')
        reports.append(item)
    cases = ET.Element('testsuites')
    for item in reports:
        suite = ET.SubElement(cases, 'testsuite', name=item['target'])
        if item['status'] == 'unverified':
            case = ET.SubElement(suite, 'testcase', name='evidence_unverified')
            ET.SubElement(case, 'error', message='INVALID_NATIVE_EVIDENCE')
        else:
            for row in item['cases']:
                case = ET.SubElement(suite, 'testcase', name=row['name'], classname=item['target']+':'+row['framework'])
                if row['status'] != 'passed':
                    ET.SubElement(case, 'skipped' if row['status'] == 'skipped' else 'failure')
    ET.ElementTree(cases).write(output/'cases.xml', encoding='utf-8', xml_declaration=True)
    executed = [c for item in reports for c in item.get('cases', [])]
    result = dict(schema_version=1, targets=reports, exit_code=overall,
                  case_executions=len(executed), unique_cases=len({c['identity'] for c in executed}),
                  counts=read_junit([output/'cases.xml'], 'case_execution'))
    (output/'native.json').write_text(safe_json(result), encoding='utf-8')
    return overall


def verify_bundle(output):
    output = Path(output)
    targets, paths = [], []
    for target in TARGETS:
        ctest, google = output/target/'ctest.xml', output/target/'gtest.xml'
        targets.append(parse_target(target, ctest, google))
        paths.append(ctest)
        if target != 'face_infer_unit_tests':
            paths.append(google)
    executed = [case for item in targets for case in item['cases']]
    counts = read_junit([output/'cases.xml'], 'case_execution')
    if counts['total'] != len(executed) or counts['failed'] != sum(c['status']=='failed' for c in executed) or counts['skipped'] != sum(c['status']=='skipped' for c in executed):
        raise ValueError('CONTRADICTORY_NATIVE_COUNTS')
    return dict(targets=len(targets), target_counts=read_junit([output/t/'ctest.xml' for t in TARGETS],'target'),
                case_executions=counts,
                unique_cases=len({c['identity'] for c in executed}),
                disabled_not_run=sum(t['disabled'] for t in targets),
                asset_unverified_targets=[t['target'] for t in targets if t['asset_unverified']]), paths
