"""读取当前JUnit证据，不信任调用者的passed字段。"""

import glob
import hashlib
from pathlib import Path
import xml.etree.ElementTree as ET
from audit._common import redact


def identity(path):
    path = Path(path)
    if (
        path.is_symlink()
        or not path.is_file()
        or path.stat().st_size > 20 * 1024 * 1024
    ):
        raise ValueError("INVALID_REPORT_FILE")
    raw = path.read_bytes()
    return {
        "sha256": hashlib.sha256(raw).hexdigest(),
        "mtime_ns": path.stat().st_mtime_ns,
    }


def paths_for(patterns):
    return sorted(
        {Path(p).absolute() for pattern in patterns for p in glob.glob(str(pattern))}
    )


def snapshot(patterns):
    return {str(path): identity(path) for path in paths_for(patterns)}


def read_junit(paths, unit="case"):
    if not paths:
        raise ValueError("MISSING_JUNIT")
    total = failed = skipped = 0
    for path in paths:
        identity(path)
        raw = Path(path).read_bytes()
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeError:
            raise ValueError("INVALID_JUNIT_ENCODING") from None
        if "<!DOCTYPE" in text.upper() or "<!ENTITY" in text.upper():
            raise ValueError("UNSAFE_XML")
        try:
            root = ET.fromstring(text)
        except ET.ParseError:
            raise ValueError("INVALID_JUNIT") from None
        if root.tag not in ("testsuites", "testsuite"):
            raise ValueError("INVALID_JUNIT")
        cases = list(root.iter("testcase"))
        if not cases:
            raise ValueError("EMPTY_JUNIT")
        for case in cases:
            markers = [
                case.find(tag) is not None for tag in ("failure", "error", "skipped")
            ]
            if sum(markers) > 1:
                raise ValueError("CONTRADICTORY_CASE")
        for suite in root.iter():
            if suite.tag not in ("testsuite", "testsuites"):
                continue
            descendants = list(suite.iter("testcase"))
            expected = {
                "tests": len(descendants),
                "failures": sum(c.find("failure") is not None for c in descendants),
                "errors": sum(c.find("error") is not None for c in descendants),
                "skipped": sum(c.find("skipped") is not None for c in descendants),
            }
            for key, count in expected.items():
                if key in suite.attrib:
                    try:
                        actual = int(suite.attrib[key])
                    except ValueError:
                        raise ValueError("INVALID_JUNIT_COUNT") from None
                    if actual != count:
                        raise ValueError("CONTRADICTORY_JUNIT_COUNT")
        total += len(cases)
        failed += sum(
            c.find("failure") is not None or c.find("error") is not None for c in cases
        )
        skipped += sum(c.find("skipped") is not None for c in cases)
    return dict(
        total=total,
        passed=total - failed - skipped,
        failed=failed,
        skipped=skipped,
        unit=unit,
    )


def verify_junit(patterns, before, unit="case"):
    paths = paths_for(patterns)
    if not paths:
        raise ValueError("MISSING_JUNIT")
    evidence = []
    for path in paths:
        current = identity(path)
        if before.get(str(path), {}).get("sha256") == current["sha256"]:
            raise ValueError("STALE_JUNIT")
        evidence.append(dict(path=str(path), **current))
    markers = sorted(
        {
            marker
            for path in paths
            for marker in ("TEST_UNVERIFIED", "TEST_SKIP_ABSENT", "[  SKIPPED ]")
            if marker in path.read_text(encoding="utf-8-sig")
        }
    )
    return dict(
        counts=read_junit(paths, unit), evidence=evidence, observed_markers=markers
    )


def archive_junit(evidence, output):
    """固定Gradle报告会被下一通道覆盖，独占留存脱敏副本及原始摘要。"""
    output = Path(output)
    output.mkdir(exist_ok=False)
    archived = []
    for number, item in enumerate(evidence):
        raw = Path(item["path"]).read_bytes()
        if hashlib.sha256(raw).hexdigest() != item["sha256"]:
            raise ValueError("JUNIT_CHANGED_AFTER_VERIFICATION")
        document = ET.fromstring(raw.decode("utf-8-sig"))
        for node in document.iter():
            if node.text:
                node.text = redact(node.text)
            if node.tail:
                node.tail = redact(node.tail)
            node.attrib = {key: redact(value) for key, value in node.attrib.items()}
        path = output / f"{number:03d}.xml"
        with path.open("xb") as stream:
            stream.write(ET.tostring(document, encoding="utf-8", xml_declaration=True))
        archived.append(
            dict(
                path=str(path),
                source_path=item["path"],
                source_sha256=item["sha256"],
                **identity(path),
            )
        )
    return archived


def verify_android_build(root, ncnn):
    """复用ELF解析绑定实际APK代码与启用ncnn的双ABI构建，不声明设备有效。"""
    import importlib.util
    import re
    import sys
    import zipfile

    android = Path(__file__).resolve().parents[1] / "android"
    sys.path.insert(0, str(android))
    try:
        spec = importlib.util.spec_from_file_location(
            "_suite_apk_inspection", android / "verify-apk-native.py"
        )
        inspector = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(inspector)
    finally:
        sys.path.pop(0)
    root = Path(root).resolve()
    result = {}
    with zipfile.ZipFile(root / "app/build/outputs/apk/debug/app-debug.apk") as apk:
        for abi, expected in inspector.ABIS.items():
            packaged_bytes = apk.read("lib/" + abi + "/libnative-lib.so")
            packaged = inspector.elf_info(packaged_bytes)
            if (packaged["elf_class"], packaged["machine"]) != expected:
                raise ValueError("APK_ABI_MISMATCH")
            for cache in (root / "app/.cxx/Debug").glob("*/" + abi + "/CMakeCache.txt"):
                if not cache.resolve().is_relative_to(root):
                    continue
                text = cache.read_text(encoding="utf-8")
                values = dict(
                    re.findall(
                        r"^([^#/\r\n:=][^:=\r\n]*):[^=\r\n]+=([^\r\n]*)$", text, re.M
                    )
                )
                if (
                    values.get("RK_SKIP_OPENCV") != "OFF"
                    or values.get("RK_ENABLE_NCNN") != "ON"
                ):
                    continue
                if (
                    Path(values.get("ncnn_DIR", "")).resolve()
                    != (Path(ncnn) / abi / "lib/cmake/ncnn").resolve()
                ):
                    continue
                ninja = cache.parent / "build.ninja"
                if not ninja.is_file():
                    continue
                if not any(
                    "-Dnative_lib_EXPORTS" in line and "-DRK_HAVE_NCNN=1" in line
                    for line in ninja.read_text(encoding="utf-8").splitlines()
                ):
                    continue
                binary = (
                    Path(values.get("CMAKE_LIBRARY_OUTPUT_DIRECTORY", ""))
                    / "libnative-lib.so"
                )
                if not binary.resolve().is_relative_to(root) or not binary.is_file():
                    continue
                binary_bytes = binary.read_bytes()
                compiled = inspector.elf_info(binary_bytes)
                build_sha256 = hashlib.sha256(binary_bytes).hexdigest()
                package_sha256 = hashlib.sha256(packaged_bytes).hexdigest()
                if (compiled["text_sha256"] != packaged["text_sha256"]
                        or compiled["loadable_sha256"] != packaged["loadable_sha256"]):
                    continue
                result[abi] = dict(
                    package_sha256=package_sha256,
                    build_sha256=build_sha256,
                    loadable_sha256=compiled["loadable_sha256"],
                    text_sha256=compiled["text_sha256"],
                    cmake_sha256=hashlib.sha256(cache.read_bytes()).hexdigest(),
                    ninja_sha256=hashlib.sha256(ninja.read_bytes()).hexdigest(),
                    scope="构建装配绑定，非设备有效性",
                )
                break
            else:
                raise ValueError("ANDROID_BUILD_BINDING_MISSING")
    return result
