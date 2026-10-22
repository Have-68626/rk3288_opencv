"""真实子进程验证失败传播、资源独占与所有权清理。"""

from pathlib import Path
import os
import subprocess
import sys
import time
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from testing.runner import Step, execute_step, RepoLock

pytestmark = pytest.mark.skipif(os.name != "nt", reason="本轮运行宿主为Windows")


def step(tmp_path, code, **kwargs):
    return Step("child", [sys.executable, "-c", code], tmp_path, **kwargs)


def test_nonzero_is_failure_and_log_redacted(tmp_path):
    result = execute_step(
        step(tmp_path, "print('token='+'synthetic-value');raise SystemExit(7)"),
        tmp_path / "out",
    )
    assert result["status"] == "failed" and result["raw_exit_code"] == 7
    assert "synthetic-value" not in (tmp_path / "out/log.txt").read_text(
        encoding="utf-8"
    )


def test_success_without_required_junit_is_unverified(tmp_path):
    item = step(tmp_path, "pass", junit=(str(tmp_path / "missing.xml"),))
    assert execute_step(item, tmp_path / "out")["status"] == "unverified"


def test_timeout_kills_owned_descendant(tmp_path):
    pidfile = tmp_path / "descendant.txt"
    code = "import subprocess,sys,time;from pathlib import Path;p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);Path(sys.argv[1]).write_text(str(p.pid));time.sleep(60)"
    item = Step("tree", [sys.executable, "-c", code, str(pidfile)], tmp_path, timeout=2)
    result = execute_step(item, tmp_path / "out", heartbeat=0.1)
    assert result["status"] == "timed_out"
    assert pidfile.exists()
    import ctypes

    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pidfile.read_text()))
    if handle:
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        assert code.value != 259, "本次后代进程仍在运行"


def test_second_process_cannot_acquire_repo_lock(tmp_path):
    scripts = Path(__file__).resolve().parents[2] / "scripts"
    code = "import sys;sys.path.insert(0,sys.argv[1]);from testing.runner import RepoLock;from pathlib import Path\ntry:\n with RepoLock(Path(sys.argv[2])):pass\nexcept RuntimeError:sys.exit(2)"
    with RepoLock(tmp_path):
        result = subprocess.run(
            [sys.executable, "-c", code, str(scripts), str(tmp_path)]
        )
        assert result.returncode == 2
    with RepoLock(tmp_path):
        pass


def test_interrupt_leaves_cancelled_result(tmp_path, monkeypatch):
    import testing.runner as runner

    original = runner.time.sleep

    def cancel(seconds):
        monkeypatch.setattr(runner.time, "sleep", original)
        raise KeyboardInterrupt

    monkeypatch.setattr(runner.time, "sleep", cancel)
    result = execute_step(
        step(tmp_path, "import time;time.sleep(60)"), tmp_path / "out"
    )
    assert result["status"] == "cancelled"


def test_batch_path_and_argument_with_spaces(tmp_path):
    from testing.catalog import native

    directory = tmp_path / "path with space"
    directory.mkdir()
    script = directory / "fixture.cmd"
    script.write_bytes(
        b'@echo off\r\nif "%~1"=="space value" (exit /b 7) else (exit /b 9)\r\n'
    )
    result = execute_step(
        Step("batch", native(script, "space value"), tmp_path), tmp_path / "out"
    )
    assert result["raw_exit_code"] == 7


def test_job_assignment_failure_never_starts_actual_command(tmp_path, monkeypatch):
    import testing.runner as runner

    marker = tmp_path / "unexpected.txt"

    def fail(self, process):
        raise RuntimeError("JOB_ASSIGN_FAILED")

    monkeypatch.setattr(runner._Job, "assign", fail)
    start = time.monotonic()
    result = execute_step(
        step(tmp_path, "from pathlib import Path;Path('unexpected.txt').touch()"),
        tmp_path / "out",
    )
    assert result["status"] == "blocked" and not marker.exists()
    assert result["resources"]["observed"]["measurement"] == "unavailable"
    assert result["resources"]["observed"]["peak_active_processes"] is None
    assert result["resources"]["observed"]["peak_job_memory_bytes"] is None
    assert time.monotonic() - start < 3


def test_cleanup_failure_cannot_be_success(tmp_path, monkeypatch):
    import testing.runner as runner

    original = runner._Job.close

    def fail(self):
        original(self)
        raise RuntimeError("PROCESS_CLEANUP_FAILED")

    monkeypatch.setattr(runner._Job, "close", fail)
    result = execute_step(step(tmp_path, "pass"), tmp_path / "out")
    assert (
        result["status"] == "blocked" and result["reason"] == "PROCESS_CLEANUP_FAILED"
    )


def test_child_temp_is_explicit_and_parent_unchanged(tmp_path):
    import json

    parent = (os.environ.get("TEMP"), os.environ.get("TMP"))
    temporary = tmp_path / "owned temp"
    resultfile = tmp_path / "environment.json"
    code = "import os,json;from pathlib import Path;Path(os.environ['TMP']).joinpath('probe').touch();Path('environment.json').write_text(json.dumps([os.environ['TEMP'],os.environ['TMP']]))"
    item = step(tmp_path, code, env={"TEMP": str(temporary), "TMP": str(temporary)})
    result = execute_step(item, tmp_path / "out")
    assert result["status"] == "passed"
    assert json.loads(resultfile.read_text()) == [str(temporary), str(temporary)]
    assert (os.environ.get("TEMP"), os.environ.get("TMP")) == parent


def test_step_retains_its_junit_after_shared_report_is_overwritten(tmp_path):
    from testing.reports import read_junit, identity

    source = tmp_path / "shared.xml"
    code = "from pathlib import Path;Path('shared.xml').write_text('<testsuite><testcase/></testsuite>',encoding='utf-8')"
    result = execute_step(step(tmp_path, code, junit=(str(source),)), tmp_path / "out")
    assert result["status"] == "passed"
    saved = Path(result["evidence"][0]["path"])
    source.write_text("later-run", encoding="utf-8")
    assert saved.is_relative_to(tmp_path / "out")
    assert read_junit([saved])["passed"] == 1
    assert identity(saved)["sha256"] == result["evidence"][0]["sha256"]


def test_job_usage_is_reported_for_successful_step(tmp_path):
    item = Step(
        "measured",
        [sys.executable, "-c", "import time;time.sleep(.3)"],
        tmp_path,
        cpu_slots=1,
        exclusive_resources=("output:measured",),
    )
    result = execute_step(item, tmp_path / "measured")
    assert result["status"] == "passed"
    assert result["resources"]["declared"]["cpu_slots"] == 1
    observed = result["resources"]["observed"]
    assert observed["measurement"] == "windows_job_object"
    assert observed["peak_job_memory_bytes"] > 0
    assert observed["peak_active_processes"] >= 1


def test_memory_budget_terminates_only_owned_job(tmp_path):
    pidfile = tmp_path / "descendant.txt"
    code = (
        "import subprocess,sys,time;from pathlib import Path;"
        "p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']);"
        "Path(sys.argv[1]).write_text(str(p.pid));"
        "data=bytearray(96*1024*1024);time.sleep(60)"
    )
    item = Step(
        "over-budget",
        [sys.executable, "-c", code, str(pidfile)],
        tmp_path,
        timeout=5,
        memory_budget_mb=48,
    )
    result = execute_step(item, tmp_path / "over-budget", heartbeat=0.05)
    assert result["status"] == "resource_limit_exceeded"
    assert result["reason"] == "MEMORY_BUDGET_EXCEEDED"
    assert result["resources"]["observed"]["peak_job_memory_bytes"] > 48 * 1024 * 1024
    assert pidfile.exists()
    import ctypes

    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pidfile.read_text()))
    if handle:
        code = ctypes.c_ulong()
        ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
        ctypes.windll.kernel32.CloseHandle(handle)
        assert code.value != 259, "预算终止后仍有本次后代进程运行"
