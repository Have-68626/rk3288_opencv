"""结构化报告必须有本次证据，不能从汇总字段反推通过。"""

from pathlib import Path
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from testing.reports import read_junit, snapshot, verify_junit


def xml(tmp_path, content):
    path = tmp_path / "result.xml"
    path.write_text(content, encoding="utf-8")
    return path


def test_nested_junit_not_double_counted(tmp_path):
    path = xml(
        tmp_path,
        '<testsuites tests="2"><testsuite tests="2" failures="0" skipped="1"><testcase name="a"/><testcase name="b"><skipped/></testcase></testsuite></testsuites>',
    )
    assert read_junit([path]) == dict(
        total=2, passed=1, failed=0, skipped=1, unit="case"
    )


@pytest.mark.parametrize(
    "body",
    [
        "bad",
        '<testsuite tests="0"/>',
        '<testsuite tests="2"><testcase/></testsuite>',
        '<testsuite failures="0"><testcase><failure/></testcase></testsuite>',
        '<!DOCTYPE a [<!ENTITY x "value">]><testsuite><testcase>&x;</testcase></testsuite>',
    ],
)
def test_invalid_or_contradictory_reports_rejected(tmp_path, body):
    with pytest.raises(ValueError):
        read_junit([xml(tmp_path, body)])


def test_missing_report_rejected(tmp_path):
    with pytest.raises(ValueError):
        read_junit([tmp_path / "missing.xml"])


def test_old_unchanged_report_rejected(tmp_path):
    path = xml(tmp_path, '<testsuite tests="1"><testcase/></testsuite>')
    before = snapshot([str(path)])
    with pytest.raises(ValueError):
        verify_junit([str(path)], before, "case")


def test_new_failure_evidence_and_ctest_unit_preserved(tmp_path):
    path = tmp_path / "result.xml"
    before = snapshot([str(path)])
    path.write_text(
        '<testsuite tests="1" failures="1"><testcase><failure/></testcase></testsuite>',
        encoding="utf-8",
    )
    result = verify_junit([str(path)], before, "target")
    assert result["counts"] == dict(
        total=1, passed=0, failed=1, skipped=0, unit="target"
    )
    assert result["evidence"][0]["sha256"]


def test_touching_old_report_does_not_make_it_new(tmp_path):
    import os

    path = xml(tmp_path, '<testsuite tests="1"><testcase/></testsuite>')
    before = snapshot([str(path)])
    os.utime(path, ns=(path.stat().st_atime_ns, path.stat().st_mtime_ns + 1000000000))
    with pytest.raises(ValueError):
        verify_junit([str(path)], before, "case")


@pytest.mark.parametrize("bad", ["none", "wrong_text", "disabled"])
def test_android_apk_binding_rejects_wrong_build(tmp_path, bad):
    import zipfile
    from test_apk_native_inspection import ABIS, elf
    from testing.reports import verify_android_build

    apk = tmp_path / "app/build/outputs/apk/debug/app-debug.apk"
    apk.parent.mkdir(parents=True)
    ncnn = tmp_path / "ncnn"
    with zipfile.ZipFile(apk, "w") as archive:
        for abi, (bits, machine) in ABIS.items():
            data = elf(bits, machine)
            archive.writestr("lib/" + abi + "/libnative-lib.so", data)
            cache = tmp_path / "app/.cxx/Debug/hash" / abi
            cache.mkdir(parents=True)
            binary = tmp_path / "native" / abi / "libnative-lib.so"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(data)
            (cache / "CMakeCache.txt").write_text(
                "// 与真实CMakeCache相同，键前允许空行和注释\n\nRK_SKIP_OPENCV:BOOL=OFF\nRK_ENABLE_NCNN:BOOL="
                + ("OFF" if bad == "disabled" else "ON")
                + "\nncnn_DIR:PATH="
                + str(ncnn / abi / "lib/cmake/ncnn")
                + "\nCMAKE_LIBRARY_OUTPUT_DIRECTORY:UNINITIALIZED="
                + str(binary.parent)
                + "\n",
                encoding="utf-8",
            )
            (cache / "build.ninja").write_text(
                "FLAGS = -Dnative_lib_EXPORTS -DRK_HAVE_NCNN=1", encoding="utf-8"
            )
            if bad == "wrong_text":
                binary.write_bytes(data[:200] + b"X" + data[201:])
    if bad == "none":
        assert set(verify_android_build(tmp_path, ncnn)) == set(ABIS)
    else:
        with pytest.raises(ValueError):
            verify_android_build(tmp_path, ncnn)


def test_archived_junit_survives_source_replacement_and_is_redacted(tmp_path):
    from testing.reports import archive_junit

    source = xml(
        tmp_path,
        '<testsuite tests="1"><testcase name="token=synthetic-junit"/></testsuite>',
    )
    verified = verify_junit([str(source)], {}, "case")
    archived = archive_junit(verified["evidence"], tmp_path / "owned")
    source.write_text("later-run", encoding="utf-8")
    copy = Path(archived[0]["path"])
    assert copy != source and read_junit([copy])["passed"] == 1
    assert "synthetic-junit" not in copy.read_text(encoding="utf-8")
    assert archived[0]["source_sha256"] == verified["evidence"][0]["sha256"]


def test_archive_rejects_source_changed_after_verification(tmp_path):
    from testing.reports import archive_junit

    source = xml(tmp_path, "<testsuite><testcase/></testsuite>")
    verified = verify_junit([str(source)], {}, "case")
    source.write_text(
        '<testsuite><testcase name="changed"/></testsuite>', encoding="utf-8"
    )
    with pytest.raises(ValueError):
        archive_junit(verified["evidence"], tmp_path / "owned")
