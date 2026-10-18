"""验证只读采样、缺测和独立窗口，不依赖本机缓存或实机。"""
import json
import struct
import subprocess
import sys
from types import SimpleNamespace

import pytest

from android_tool_test_support import ROOT, load_tool, result


@pytest.mark.parametrize("memory,smaps,expected,source", [
    (b" TOTAL 123 0 0\n", b"", 123, "dumpsys"),
    (b" TOTAL PSS: 1,234\n", b"", 1234, "dumpsys"),
    (b" TOTAL 0 0 0\n", b"Pss: 40 kB\nPss: 60 kB\n", 100, "smaps"),
    (b" TOTAL 0 0 0\n", b"", None, None),
])
def test_pss_sources_and_zero_are_explicit(memory, smaps, expected, source):
    tool = load_tool("collect-runtime-metrics.py")
    row = tool.parse_memory(memory, smaps)
    assert row.get("pss_kib") == expected
    assert row.get("pss_source") == source
    if b" TOTAL 0" in memory:
        assert row["dumpsys_pss_observation"] == "zero"


@pytest.mark.parametrize("elf_class,fmt", [(1, "<II"), (2, "<QQ")])
def test_auxv_uses_verified_word_size(elf_class, fmt):
    tool = load_tool("collect-runtime-metrics.py")
    data = struct.pack(fmt, 17, 100) + struct.pack(fmt, 0, 0)
    assert tool.parse_auxv(data, elf_class) == 100
    assert tool.parse_auxv(data, None) is None
    assert tool.parse_auxv(data[:-1], elf_class) is None
    assert tool.parse_auxv(struct.pack(fmt, 0, 0), elf_class) is None


class Clock:
    value = 0

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class ReadonlyAdb:
    def __init__(self, pids=None):
        self.commands = []
        self.pids = iter(pids or [b"123"] * 20)
        self.ticks = 10

    def run(self, *args):
        self.commands.append(args)
        if args == ("get-state",):
            return result(b"device\n")
        if args[:2] == ("shell", "getconf"):
            return result(b"100\n")
        if args[:2] == ("shell", "pidof"):
            pid = next(self.pids, b"")
            return result(pid, 0 if pid else 1)
        if args[-1].endswith("/stat"):
            fields = ["S"] + ["0"] * 22
            fields[11] = str(self.ticks)
            fields[19] = "42"
            self.ticks += 10
            return result(("123 (含右括号)的进程) " + " ".join(fields)).encode())
        if args[-1].endswith("/status"):
            return result(b"VmRSS:\t200 kB\nVmHWM:\t250 kB\nThreads:\t4\n")
        if args[:3] == ("shell", "dumpsys", "meminfo"):
            return result(b" TOTAL 0 0\n")
        if args[-1].endswith("/smaps"):
            return result(b"Pss: 80 kB\n")
        return result(b"1608000\n")


def test_collection_is_read_only_and_cpu_is_measured():
    tool = load_tool("collect-runtime-metrics.py")
    clock, adb = Clock(), ReadonlyAdb()
    args = SimpleNamespace(package="com.example.app", duration=3, interval=1)
    report = tool.collect(args, adb, clock.now, clock.sleep)
    assert len(report["samples"]) == 3
    assert report["samples"][1]["process_cpu_one_core_percent"] == 10
    assert report["samples"][0]["pss_source"] == "smaps"
    assert report["completed"] is True
    assert all(command[0] in ("get-state", "shell", "exec-out") for command in adb.commands)
    forbidden = {"install", "uninstall", "connect", "start", "force-stop", "push", "rm", "logcat"}
    assert not any(forbidden.intersection(command) for command in adb.commands)


@pytest.mark.parametrize("pids,reason", [([b"123", b"124"], "PID_CHANGED"), ([b"123", b""], "TARGET_NOT_RUNNING")])
def test_process_change_ends_window_without_mixing(pids, reason):
    tool = load_tool("collect-runtime-metrics.py")
    clock = Clock()
    report = tool.collect(SimpleNamespace(package="com.example.app", duration=3, interval=1),
                          ReadonlyAdb(pids), clock.now, clock.sleep)
    assert report["completed"] is False
    assert report["stop_reason"] == reason
    assert len(report["samples"]) == 1


def test_metrics_cli_refuses_existing_output_before_adb(tmp_path):
    tool = ROOT / "scripts/android/collect-runtime-metrics.py"
    assert tool.exists(), "缺少正式采样器"
    output = tmp_path / "已有.json"
    output.write_text("保留", encoding="utf8")
    run = subprocess.run([sys.executable, str(tool), "--adb", str(tmp_path / "不存在"),
                          "--serial", "fixture", "--package", "com.example.app", "--output", str(output)],
                         capture_output=True, text=True, encoding="utf8")
    assert run.returncode == 2
    assert "OUTPUT_EXISTS" in run.stderr
    assert output.read_text(encoding="utf8") == "保留"
    run = subprocess.run([sys.executable, str(tool)], capture_output=True, text=True, encoding="utf8")
    assert run.returncode == 2


def test_summary_keeps_windows_and_smaps_supplement_separate(tmp_path):
    tool = load_tool("summarize-runtime-metrics.py")
    first = tmp_path / "first.json"
    second = tmp_path / "second.json"
    first.write_text(json.dumps({"samples": [{"rss_kib": 200}, {"rss_kib": 220}], "completed": True}))
    second.write_text(json.dumps({"samples": [{"smaps_pss_kib": 90}], "private": "不得复制的字段"}))
    report = tool.summarize([first, second])
    assert len(report["windows"]) == 2
    assert report["windows"][0]["metrics"]["rss_kib"]["median"] == 210
    assert report["windows"][0]["pss_by_source"] == {}
    assert report["windows"][1]["pss_by_source"]["smaps"]["median"] == 90
    assert "不得复制" not in json.dumps(report, ensure_ascii=False)


def test_summary_rejects_nonfinite_input(tmp_path):
    tool = load_tool("summarize-runtime-metrics.py")
    path = tmp_path / "bad.json"
    path.write_text('{"samples":[{"rss_kib":NaN}]}')
    with pytest.raises(ValueError):
        tool.summarize([path])


def test_runtime_summary_excludes_shutdown_row_without_rss(tmp_path):
    tool = load_tool("summarize-runtime-metrics.py")
    path = tmp_path / "runtime.json"
    path.write_text(json.dumps({"samples": [{"rss_kib": 200, "thread_count": 60}, {"thread_count": 2}]}))
    window = tool.summarize([path])["windows"][0]
    assert window["metrics"]["thread_count"]["min"] == 60
    assert window["excluded_rss_samples"] == 1


@pytest.mark.parametrize("values,expected", [
    ([0, -1, 80, 100], {"samples": 2, "min": 80, "median": 90, "p95": 100, "max": 100}),
    ([0, -1], None),
])
def test_legacy_pss_summary_excludes_missing_values(tmp_path, values, expected):
    tool = load_tool("summarize-runtime-metrics.py")
    path = tmp_path / "legacy.json"
    path.write_text(json.dumps({"pss_kib_samples": values}))
    window = tool.summarize([path])["windows"][0]
    assert window["pss_by_source"].get("dumpsys") == expected


@pytest.mark.parametrize("values", [[True], ["100"], {"value": 100}])
def test_legacy_pss_summary_rejects_invalid_types(tmp_path, values):
    tool = load_tool("summarize-runtime-metrics.py")
    path = tmp_path / "invalid-legacy.json"
    path.write_text(json.dumps({"pss_kib_samples": values}))
    with pytest.raises(ValueError):
        tool.summarize([path])
