"""使用自造流证据检验门禁，报告的passed字段不是信任根。"""
from copy import deepcopy
import json
import subprocess
import sys

import pytest

from android_tool_test_support import ROOT, load_tool


def evidence(mode="camera"):
    seconds = 300 if mode == "camera" else 30
    events = [{"kind": "event", "value": "STREAM_REQUESTED", "elapsed": 1}]
    if mode == "restart":
        events += [{"kind": "event", "value": "STREAM_STOPPED", "elapsed": 11},
                   {"kind": "event", "value": "STREAM_RESTARTED", "elapsed": 16}]
    if mode == "retry":
        events += [{"kind": "event", "value": "EXPLICIT_RETRY", "elapsed": 22}]
    events += [{"kind": "outcome", "value": "PASS", "elapsed": seconds + 2}]
    receiver = {"frames": 100, "progress_updates": 8, "exit": 0,
                "error_lines": 0, "listener_verified": True, "early_exit": None}
    receivers = [deepcopy(receiver) for _ in range(2 if mode in ("restart", "retry") else 1)]
    if mode == "retry":
        receivers[0]["exit"] = 1
    return {"mode": mode, "seconds_requested": seconds, "events": events, "receivers": receivers,
            "instrument_exit": 0, "target_app_stopped": True, "desktop_visible": True,
            "helper_removed": True, "owned_processes_stopped": True, "passed": True}


@pytest.mark.parametrize("mode", ["camera", "file", "restart", "retry"])
def test_valid_evidence_passes(mode):
    assert load_tool("validate-stream-evidence.py").validate(evidence(mode)) == []


@pytest.mark.parametrize("mutation", ["zero_frames", "receiver_error", "no_desktop", "short_camera", "fake_pass", "decode_error", "retry_without_outage", "reversed_restart"])
def test_evidence_mutations_are_rejected(mutation):
    tool = load_tool("validate-stream-evidence.py")
    row = evidence("retry" if mutation == "retry_without_outage" else "restart" if mutation == "reversed_restart" else "camera")
    if mutation == "zero_frames": row["receivers"][0]["frames"] = 0
    if mutation == "receiver_error": row["receivers"][-1]["exit"] = 1
    if mutation == "no_desktop": row["desktop_visible"] = False
    if mutation == "short_camera": row["seconds_requested"] = 30
    if mutation == "fake_pass": row["events"][-1]["value"] = "STREAM_ENDED"
    if mutation == "decode_error": row["receivers"][0]["error_lines"] = 1
    if mutation == "retry_without_outage": row["receivers"][0]["exit"] = 0
    if mutation == "reversed_restart": row["events"][1]["elapsed"] = 17
    assert tool.validate(row)


@pytest.mark.parametrize("row", [{}, [], {"mode": "camera", "events": None}])
def test_bad_schema_is_rejected(row):
    assert load_tool("validate-stream-evidence.py").validate(row)


def test_stream_cli_bad_json_is_sanitized(tmp_path):
    tool = ROOT / "scripts/android/validate-stream-evidence.py"
    assert tool.exists(), "缺少正式证据门禁"
    bad = tmp_path / "bad.json"
    bad.write_text('原始异常不应输出', encoding='utf-8')
    run = subprocess.run([sys.executable, str(tool), "--inputs", str(bad), "--output", str(tmp_path / "out.json")],
                         capture_output=True, text=True, encoding="utf8")
    assert run.returncode == 2
    assert "原始异常" not in run.stdout + run.stderr
    assert "Traceback" not in run.stderr
