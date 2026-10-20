"""仅离线复判流证据；passed字段不替代帧、时长、退出及清理事实。"""
import hashlib
import math
from pathlib import Path

from _tool_support import Parser, check_output, load_json, run_cli, write_report


def number(value):
    return type(value) in (int, float) and math.isfinite(value)


def validate(row):
    if not isinstance(row, dict):
        return ["INVALID_SCHEMA"]
    mode = row.get("mode")
    if mode not in ("selftest", "file", "camera", "restart", "retry"):
        return ["INVALID_MODE"]
    if type(row.get("instrument_exit")) is not int or row["instrument_exit"] != 0:
        return ["INSTRUMENT_FAILED"]
    if not all(row.get(k) is True for k in ["target_app_stopped", "desktop_visible", "helper_removed", "owned_processes_stopped"]):
        return ["CLEANUP_UNVERIFIED"]
    if row.get("passed") is False:
        return ["REPORTED_FAILURE"]
    events = row.get("events")
    if not isinstance(events, list) or not events or any(not isinstance(e, dict) or not number(e.get("elapsed")) or e["elapsed"] < 0 for e in events):
        return ["INVALID_EVENTS"]
    if [e["elapsed"] for e in events] != sorted(e["elapsed"] for e in events):
        return ["EVENT_ORDER"]
    outcomes = [e for e in events if e.get("kind") == "outcome"]
    if len(outcomes) != 1 or outcomes[0].get("value") != "PASS":
        return ["OUTCOME_FAILED"]
    end = outcomes[0]["elapsed"]
    if events[-1] != outcomes[0]:
        return ["EVENT_ORDER"]
    if mode == "selftest":
        return [] if any(e.get("value") == "SELFTEST_PASS" for e in events) else ["SELFTEST_UNVERIFIED"]
    starts = [e["elapsed"] for e in events if e.get("kind") == "event" and e.get("value") == "STREAM_REQUESTED"]
    duration = row.get("seconds_requested")
    if not number(duration) or duration <= 0 or len(starts) != 1 or end - starts[0] < duration:
        return ["DURATION_UNVERIFIED"]
    if mode == "camera" and duration < 300:
        return ["CAMERA_TOO_SHORT"]
    receivers = row.get("receivers")
    expected = 2 if mode in ("restart", "retry") else 1
    if not isinstance(receivers, list) or len(receivers) != expected:
        return ["RECEIVER_COUNT"]
    for index, receiver in enumerate(receivers):
        if not isinstance(receiver, dict):
            return ["INVALID_RECEIVER"]
        if any(type(receiver.get(k)) is not int or receiver[k] < minimum for k, minimum in [("frames", 1), ("progress_updates", 2)]):
            return ["NO_DECODED_PROGRESS"]
        if receiver.get("listener_verified") is not True or receiver.get("early_exit") is not None:
            return ["LISTENER_UNVERIFIED"]
        if type(receiver.get("error_lines")) is not int or receiver["error_lines"] != 0:
            return ["DECODE_ERROR"]
        code = receiver.get("exit")
        if type(code) is not int or ((code == 0) if mode == "retry" and index == 0 else (code != 0)):
            return ["RECEIVER_EXIT"]
    def event_time(name):
        values = [e["elapsed"] for e in events if e.get("kind") == "event" and e.get("value") == name]
        return values[0] if len(values) == 1 else None
    if mode == "restart":
        stopped, restarted = event_time("STREAM_STOPPED"), event_time("STREAM_RESTARTED")
        if stopped is None or restarted is None or not starts[0] < stopped < restarted < end:
            return ["RESTART_UNVERIFIED"]
    if mode == "retry":
        retry = event_time("EXPLICIT_RETRY")
        if retry is None or not starts[0] < retry < end:
            return ["RETRY_UNVERIFIED"]
    return []


def main():
    parser = Parser(description=__doc__)
    parser.add_argument("--inputs", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    check_output(args.output)
    rows = []
    for index, path in enumerate(args.inputs):
        reasons = validate(load_json(path))
        rows.append({"input_index": index, "input_sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                     "passed": not reasons, "reasons": reasons})
    passed = all(r["passed"] for r in rows)
    write_report(args.output, {"schema_version": 1, "results": rows, "all_functional_passed": passed,
                               "full_acceptance_passed": False})
    print("FUNCTIONAL_PASS=" + str(passed).lower())
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(run_cli(main))
