"""本地ADB替身验证身份摘要与清理事实，禁止实机写操作。"""
import hashlib
from types import SimpleNamespace

import pytest

from android_tool_test_support import load_tool, result


PAYLOAD = b"fixture-apk-bytes"


class FakeAdb:
    def __init__(self, *, running=False, desktop=True, helper=False, unavailable=False):
        self.commands = []
        self.running, self.desktop, self.helper, self.unavailable = running, desktop, helper, unavailable

    def run(self, *args):
        self.commands.append(args)
        if self.unavailable: return result(b"", 1, b"raw-error-must-not-be-returned")
        if args == ("get-state",): return result(b"device\n")
        if args[:3] == ("shell", "pm", "path"):
            return result(b"package:/data/app/com.example.app-1/base.apk\n")
        if args[:2] == ("shell", "sha256sum"): return result(b"", 127)
        if args[:2] == ("exec-out", "cat"): return result(PAYLOAD)
        if args[:2] == ("shell", "pidof"): return result(b"23\n") if self.running else result(b"", 1)
        if args[:3] == ("shell", "dumpsys", "activity"):
            package = "com.example.home" if self.desktop else "com.example.launcherSpoof"
            return result(("mResumedActivity: ActivityRecord{0 u0 " + package + "/.Screen t2}\n").encode())
        if args[:4] == ("shell", "pm", "list", "packages"):
            return result(b"package:com.example.helper\n") if self.helper else result(b"")
        raise AssertionError("未允许的命令")


def args():
    return SimpleNamespace(package="com.example.app", helper_package="com.example.helper",
                           launcher_package="com.example.home", expected_sha256=hashlib.sha256(PAYLOAD).hexdigest(),
                           temporary_input=None)


def test_state_hash_fallback_has_no_fixed_apk_length_and_is_read_only():
    tool = load_tool("verify-device-state.py")
    adb = FakeAdb()
    report = tool.verify(args(), adb)
    assert report["passed"] is True
    assert report["hash_method"] == "host_memory_sha256"
    assert report["candidate_identity_matches"] is True
    assert not any(set(c).intersection({"install", "uninstall", "connect", "am", "push", "rm", "logcat"}) for c in adb.commands)


@pytest.mark.parametrize("option", ["running", "desktop", "helper"])
def test_running_app_wrong_desktop_or_helper_prevents_pass(option):
    tool = load_tool("verify-device-state.py")
    options = {option: option != "desktop"}
    assert tool.verify(args(), FakeAdb(**options))["passed"] is False


def test_adb_error_cannot_be_interpreted_as_absent_process():
    tool = load_tool("verify-device-state.py")
    with pytest.raises(ValueError): tool.verify(args(), FakeAdb(unavailable=True))


def test_wrong_sha_does_not_pass():
    tool = load_tool("verify-device-state.py")
    config = args()
    config.expected_sha256 = "0" * 64
    assert tool.verify(config, FakeAdb())["passed"] is False


@pytest.mark.parametrize("field,value", [("package", "bad;command"), ("temporary_input", "../outside")])
def test_invalid_parameters_fail_before_any_adb(field, value):
    tool = load_tool("verify-device-state.py")
    config, adb = args(), FakeAdb()
    setattr(config, field, value)
    with pytest.raises(ValueError): tool.verify(config, adb)
    assert adb.commands == []
