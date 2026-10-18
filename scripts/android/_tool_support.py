"""只共享固定错误、只读ADB调用与独占输出，不处理业务判据。"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys


class ToolError(ValueError):
    pass


class Parser(argparse.ArgumentParser):
    def error(self, message):
        # 参数或异常正文可能携带设备地址、路径或凭据，只输出固定诊断。
        self.print_usage(sys.stderr)
        self.exit(2, "INVALID_ARGUMENTS\n")


def check_package(value):
    if not re.fullmatch(r"[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+", value, re.ASCII):
        raise ToolError("INVALID_PACKAGE")
    return value


def check_output(path):
    if os.path.lexists(path):
        raise ToolError("OUTPUT_EXISTS")


def write_report(path, report):
    check_output(path)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    try:
        with Path(path).open("x", encoding="utf8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write("\n")
    except FileExistsError:
        raise ToolError("OUTPUT_EXISTS") from None


def load_json(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8-sig"),
                          parse_constant=lambda _: (_ for _ in ()).throw(ValueError()))
    except (OSError, UnicodeError, ValueError):
        raise ToolError("INVALID_JSON") from None


class Adb:
    def __init__(self, executable, serial):
        if not serial or any(c.isspace() or ord(c) < 32 for c in serial):
            raise ToolError("INVALID_SERIAL")
        self.executable, self.serial = executable, serial

    def run(self, *args):
        try:
            return subprocess.run([self.executable, "-s", self.serial, *args],
                                  capture_output=True, timeout=90 if args[:1] == ("exec-out",) else 15)
        except (OSError, subprocess.TimeoutExpired):
            raise ToolError("ADB_UNAVAILABLE") from None


def require_online(adb):
    row = adb.run("get-state")
    if row.returncode != 0 or row.stdout.strip() != b"device":
        raise ToolError("ADB_NOT_ONLINE")


def process_id(adb, package):
    row = adb.run("shell", "pidof", package)
    value = row.stdout.strip()
    if row.returncode == 0 and re.fullmatch(rb"\d+", value):
        return value.decode("ascii")
    if row.returncode == 1 and not value and not row.stderr.strip():
        return None
    raise ToolError("PROCESS_QUERY_FAILED")


def run_cli(main):
    try:
        return main()
    except ToolError as error:
        print(str(error), file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("CANCELLED", file=sys.stderr)
        return 130
    except Exception:
        print("TOOL_FAILED", file=sys.stderr)
        return 2
