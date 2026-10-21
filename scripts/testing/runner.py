"""Windows资源互斥与受控进程树；不清理其他会话的进程。"""

from dataclasses import dataclass, field
import ctypes
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from audit._common import redact
from .reports import (
    snapshot,
    verify_junit,
    verify_android_build,
    identity,
    archive_junit,
)


@dataclass
class Step:
    name: str
    command: list
    cwd: Path
    timeout: float = 600
    junit: tuple = ()
    unit: str = "case"
    env: dict = field(default_factory=dict)
    audit: Path | None = None
    android_ncnn: Path | None = None
    native_report: Path | None = None


class RepoLock:
    def __init__(self, root):
        self.name = (
            "Global\\RKTestSuites_"
            + hashlib.sha256(str(Path(root).resolve()).casefold().encode()).hexdigest()
        )

    def __enter__(self):
        if os.name != "nt":
            raise RuntimeError("UNSUPPORTED_HOST")
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateMutexW.argtypes = [W.LPVOID, W.BOOL, W.LPCWSTR]
        self.api.CreateMutexW.restype = W.HANDLE
        self.api.WaitForSingleObject.argtypes = [W.HANDLE, W.DWORD]
        self.api.ReleaseMutex.argtypes = [W.HANDLE]
        self.api.CloseHandle.argtypes = [W.HANDLE]
        self.handle = self.api.CreateMutexW(None, False, self.name)
        if not self.handle:
            raise RuntimeError("LOCK_FAILED")
        state = self.api.WaitForSingleObject(self.handle, 0)
        if state not in (0, 0x80):
            self.api.CloseHandle(self.handle)
            raise RuntimeError("RESOURCE_BUSY")
        return self

    def __exit__(self, *args):
        released = self.api.ReleaseMutex(self.handle)
        closed = self.api.CloseHandle(self.handle)
        if not released or not closed:
            raise RuntimeError("LOCK_RELEASE_FAILED")


class _Limits(ctypes.Structure):
    _fields_ = [
        ("process_time", ctypes.c_longlong),
        ("job_time", ctypes.c_longlong),
        ("flags", W.DWORD),
        ("minimum", ctypes.c_size_t),
        ("maximum", ctypes.c_size_t),
        ("active", W.DWORD),
        ("affinity", ctypes.c_size_t),
        ("priority", W.DWORD),
        ("scheduling", W.DWORD),
    ]


class _Io(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_ulonglong)
        for name in (
            "read",
            "write",
            "other",
            "read_bytes",
            "write_bytes",
            "other_bytes",
        )
    ]


class _Extended(ctypes.Structure):
    _fields_ = [
        ("basic", _Limits),
        ("io", _Io),
        ("process_memory", ctypes.c_size_t),
        ("job_memory", ctypes.c_size_t),
        ("peak_process", ctypes.c_size_t),
        ("peak_job", ctypes.c_size_t),
    ]


class _Job:
    def __init__(self):
        if os.name != "nt":
            raise RuntimeError("UNSUPPORTED_HOST")
        self.api = ctypes.WinDLL("kernel32", use_last_error=True)
        self.api.CreateJobObjectW.argtypes = [W.LPVOID, W.LPCWSTR]
        self.api.CreateJobObjectW.restype = W.HANDLE
        self.api.SetInformationJobObject.argtypes = [
            W.HANDLE,
            ctypes.c_int,
            W.LPVOID,
            W.DWORD,
        ]
        self.api.AssignProcessToJobObject.argtypes = [W.HANDLE, W.HANDLE]
        self.api.TerminateJobObject.argtypes = [W.HANDLE, W.UINT]
        self.api.CloseHandle.argtypes = [W.HANDLE]
        self.handle = self.api.CreateJobObjectW(None, None)
        if not self.handle:
            raise RuntimeError("JOB_CREATE_FAILED")
        limits = _Extended()
        limits.basic.flags = (
            0x2000  # 所有权句柄关闭时收回后代，避免控制器意外退出后遗留。
        )
        if not self.api.SetInformationJobObject(
            self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)
        ):
            self.api.CloseHandle(self.handle)
            raise RuntimeError("JOB_CONFIG_FAILED")

    def assign(self, process):
        if not self.api.AssignProcessToJobObject(
            self.handle, W.HANDLE(int(process._handle))
        ):
            raise RuntimeError("JOB_ASSIGN_FAILED")

    def close(self):
        stopped = self.api.TerminateJobObject(self.handle, 1)
        closed = self.api.CloseHandle(self.handle)
        if not stopped or not closed:
            raise RuntimeError("PROCESS_CLEANUP_FAILED")


# 实际命令先等待启动闸门；父进程加入Job后才放行，避免后代逃出所有权边界。
_GATE = "import sys,json,subprocess\nif sys.stdin.buffer.read(1)!=b'R':sys.exit(125)\ntry:sys.exit(subprocess.call(json.loads(sys.argv[1])))\nexcept OSError:sys.exit(127)"


def execute_step(step, output, heartbeat=30):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    result = dict(
        name=step.name, status="blocked", raw_exit_code=None, reason="", elapsed=0
    )
    if not step.timeout > 0:
        raise ValueError("INVALID_TIMEOUT")
    started = time.monotonic()
    process = job = reader = None
    errors = []
    markers = set()
    before = {}
    environment = os.environ.copy()
    environment.update(step.env)
    environment.update(PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
    stage='temporary_preflight'
    try:
        from .environment import probe_temporary, diagnostic
        temporary = Path(step.env.get("TMP", output / "tmp"))
        if temporary.resolve() != temporary.absolute():
            raise ValueError("UNSAFE_TEMP")
        result['environment_preflight']=probe_temporary(temporary)
        stage='process_setup'
        environment.update(TEMP=str(temporary), TMP=str(temporary))
        result["temporary_directory"] = str(temporary)
        before = snapshot(step.junit)
        if step.native_report and step.native_report.exists():
            raise ValueError('NATIVE_OUTPUT_ALREADY_EXISTS')
        job = _Job()
        process = subprocess.Popen(
            [
                sys.executable,
                "-X",
                "utf8=1",
                "-c",
                _GATE,
                json.dumps(list(map(str, step.command))),
            ],
            cwd=step.cwd,
            env=environment,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=0x08000000 | 0x200,
        )
        job.assign(process)

        def capture():
            try:
                with (output / "log.txt").open("x", encoding="utf-8") as log:
                    discard = False
                    while line := process.stdout.readline(65537):
                        if len(line) == 65537 and not line.endswith("\n"):
                            if not discard:
                                log.write("[超长日志行已省略]\n")
                                log.flush()
                            discard = True
                            continue
                        if discard:
                            discard = False
                            continue
                        safe = redact(line)
                        log.write(safe)
                        log.flush()
                        for marker in (
                            "TEST_UNVERIFIED",
                            "TEST_SKIP_ABSENT",
                            "[  SKIPPED ]",
                        ):
                            if marker in safe:
                                markers.add(marker)
            except Exception:
                errors.append("LOG_CAPTURE_FAILED")

        reader = threading.Thread(target=capture, daemon=True)
        reader.start()
        process.stdin.write("R")
        process.stdin.flush()
        process.stdin.close()
        result["pid"] = process.pid
        beat = started
        while process.poll() is None:
            now = time.monotonic()
            if errors:
                raise RuntimeError(errors[0])
            if now - started >= step.timeout:
                result.update(status="timed_out", reason="TIMEOUT")
                break
            if now - beat >= heartbeat:
                print(
                    f"ALIVE step={step.name} pid={process.pid} seconds={int(now-started)}",
                    flush=True,
                )
                beat = now
            time.sleep(0.05)
        else:
            code = process.returncode
            result.update(
                status="passed" if code == 0 else "failed",
                raw_exit_code=code,
                reason="" if code == 0 else "PROCESS_EXIT_NONZERO",
            )
            if code == 127:
                result.update(status="blocked", reason="CHILD_START_FAILED")
    except KeyboardInterrupt:
        result.update(status="cancelled", reason="CANCELLED")
    except (OSError, RuntimeError, ValueError) as error:
        result.update(status="blocked", reason="EXECUTION_SETUP_FAILED")
        result['diagnostic']=diagnostic(error,stage)
    finally:
        if process and process.stdin and not process.stdin.closed:
            process.stdin.close()
        if job:
            try:
                job.close()
            except RuntimeError:
                errors.append("PROCESS_CLEANUP_FAILED")
        if process:
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                errors.append("PROCESS_CLEANUP_FAILED")
        if reader:
            reader.join(timeout=10)
            if reader.is_alive():
                errors.append("LOG_CAPTURE_FAILED")
        if errors:
            result.update(status="blocked", reason=errors[0])
    if process and result["raw_exit_code"] is None:
        result["raw_exit_code"] = process.returncode
    result["elapsed"] = round(time.monotonic() - started, 3)
    result["observed_markers"] = sorted(markers)
    if result["status"] in ("passed", "failed") and step.junit:
        try:
            evidence = verify_junit(step.junit, before, step.unit)
            evidence["evidence"] = archive_junit(
                evidence["evidence"], output / "evidence"
            )
            evidence["observed_markers"] = sorted(
                markers | set(evidence["observed_markers"])
            )
            result.update(evidence)
            if step.unit == "target":
                result["internal_case_counts"] = "unknown"
            if result["counts"]["failed"] and result["status"] == "passed":
                result.update(status="failed", reason="JUNIT_FAILURE")
        except (ValueError, OSError):
            result.update(status="unverified", reason="INVALID_OR_STALE_JUNIT")
    if result['status'] in ('passed', 'failed') and step.native_report:
        try:
            from .native import verify_bundle
            details, paths = verify_bundle(step.native_report)
            result['native'] = details
            result['native_evidence'] = archive_junit(
                [dict(path=str(path), **identity(path)) for path in paths],
                output / 'native-evidence',
            )
        except (ValueError, OSError):
            result.update(status='unverified', reason='INVALID_NATIVE_EVIDENCE')
    if result["status"] == "passed" and step.android_ncnn:
        try:
            result["android_build_binding"] = verify_android_build(
                step.cwd, step.android_ncnn
            )
        except Exception:
            result.update(
                status="unverified", reason="ANDROID_BUILD_BINDING_UNVERIFIED"
            )
    if result["status"] == "passed" and step.audit:
        try:
            reports = list(Path(step.audit).glob("*.json"))
            if len(reports) != 1:
                raise ValueError()
            report = json.loads(reports[0].read_text(encoding="utf-8"))
            if not isinstance(report.get("runId"), str) or not isinstance(
                report.get("files"), dict
            ):
                raise ValueError()
            result["evidence"] = [dict(path=str(reports[0]), **identity(reports[0]))]
            defects = report["defects"]
            if not isinstance(defects, list):
                raise ValueError()
            if any(d["severity"] in ("high", "medium") for d in defects):
                result.update(status="failed", reason="DOC_GATE_FAILED")
        except (OSError, ValueError, KeyError, TypeError):
            result.update(status="unverified", reason="INVALID_AUDIT_REPORT")
    return result
