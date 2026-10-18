"""明确目标的只读状态快照；不安装、启动、卸载或清理任何应用。"""
import hashlib
from pathlib import PurePosixPath
import re

from _tool_support import (Adb, Parser, ToolError, check_output, check_package,
                           process_id, require_online, run_cli, write_report)


def check_arguments(args):
    for package in [args.package, args.helper_package, args.launcher_package]:
        check_package(package)
    if not re.fullmatch(r"[a-fA-F0-9]{64}", args.expected_sha256):
        raise ToolError("INVALID_SHA256")
    if args.temporary_input and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.temporary_input):
        raise ToolError("INVALID_TEMPORARY_NAME")


def verify(args, adb):
    check_arguments(args)
    require_online(adb)
    row = adb.run("shell", "pm", "path", args.package)
    match = re.fullmatch(rb"package:(/data/app/[A-Za-z0-9_./=+~-]+/base\.apk)\s*", row.stdout)
    if row.returncode != 0 or not match:
        raise ToolError("INSTALLED_APK_PATH_UNVERIFIED")
    path = match[1].decode("ascii")
    if ".." in PurePosixPath(path).parts:
        raise ToolError("INSTALLED_APK_PATH_UNVERIFIED")
    hashed = adb.run("shell", "sha256sum", path)
    match = re.match(rb"^([a-fA-F0-9]{64})\s", hashed.stdout) if hashed.returncode == 0 else None
    if match:
        digest, method = match[1].decode("ascii").lower(), "device_sha256sum"
    else:
        payload = adb.run("exec-out", "cat", path)
        if payload.returncode != 0 or not payload.stdout:
            raise ToolError("INSTALLED_APK_READ_FAILED")
        # 不依赖某次APK长度，也不将已装代码包另存为本机副本。
        digest, method = hashlib.sha256(payload.stdout).hexdigest(), "host_memory_sha256"
    stopped = process_id(adb, args.package) is None
    activity = adb.run("shell", "dumpsys", "activity", "activities")
    if activity.returncode != 0:
        raise ToolError("ACTIVITY_QUERY_FAILED")
    expression = rb"(?:topResumedActivity|mResumedActivity)\s*[:=][^\r\n]*?\s([A-Za-z0-9_.]+)/[A-Za-z0-9_.$]+"
    resumed = re.findall(expression, activity.stdout)
    desktop = bool(resumed) and all(p.decode("ascii") == args.launcher_package for p in resumed)
    helpers = adb.run("shell", "pm", "list", "packages", args.helper_package)
    if helpers.returncode != 0:
        raise ToolError("HELPER_QUERY_FAILED")
    packages = []
    for line in helpers.stdout.splitlines():
        match = re.fullmatch(rb"package:([A-Za-z0-9_.]+)", line.strip())
        if not match:
            raise ToolError("HELPER_QUERY_FAILED")
        packages.append(match[1].decode("ascii"))
    absent = args.helper_package not in packages
    temporary_absent = None
    if args.temporary_input:
        temporary = adb.run("shell", "run-as", args.package, "test", "-e", "cache/" + args.temporary_input)
        if temporary.returncode not in (0, 1) or temporary.stderr.strip():
            raise ToolError("TEMP_QUERY_FAILED")
        temporary_absent = temporary.returncode == 1
    identity = digest == args.expected_sha256.lower()
    return {"schema_version": 1, "candidate_identity_matches": identity, "installed_apk_sha256": digest,
            "hash_method": method, "target_app_stopped": stopped, "desktop_visible": desktop,
            "helper_absent": absent, "temporary_input_checked": args.temporary_input is not None,
            "temporary_input_absent": temporary_absent,
            "passed": identity and stopped and desktop and absent and temporary_absent is not False,
            "scope": "单次只读快照；未请求的临时输入检查不代表已清理，也不保证随后状态不变"}


def main():
    parser = Parser(description=__doc__)
    for key in ["adb", "serial", "package", "helper-package", "launcher-package", "expected-sha256", "output"]:
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--temporary-input")
    args = parser.parse_args()
    check_output(args.output)
    # 无效输入先拒绝，不能为了发现参数错误而访问设备。
    check_arguments(args)
    report = verify(args, Adb(args.adb, args.serial))
    write_report(args.output, report)
    print("STATE_PASS=" + str(report["passed"]).lower())
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(run_cli(main))
