"""显式指定设备的只读数值采样；不启动应用，不保存原始系统输出。"""
import math
import re
import struct
import time

from _tool_support import (Adb, Parser, ToolError, check_output, check_package,
                           process_id, require_online, run_cli, write_report)


def parse_memory(meminfo, smaps=b""):
    match = re.search(rb"TOTAL PSS:\s*([0-9,]+)", meminfo)
    if not match:
        match = re.search(rb"^\s*TOTAL\s+(\d+)\s", meminfo, re.M)
    value = int(match[1].replace(b",", b"")) if match else None
    row = {"dumpsys_pss_observation": "missing" if value is None else "zero" if value == 0 else "positive"}
    if value and value > 0:
        row.update(pss_kib=value, pss_source="dumpsys")
    else:
        values = re.findall(rb"^Pss:\s*(\d+)\s+kB\s*$", smaps, re.M)
        total = sum(map(int, values))
        if total > 0:
            row.update(pss_kib=total, pss_source="smaps")
    return row


def parse_auxv(data, elf_class):
    fmt = {1: "<II", 2: "<QQ"}.get(elf_class)
    if not fmt or not data or len(data) % struct.calcsize(fmt):
        return None
    value = None
    for kind, candidate in struct.iter_unpack(fmt, data):
        if kind == 0:
            return value
        if kind == 17:
            if candidate <= 0 or (value is not None and candidate != value):
                return None
            value = candidate
    return None


def clock_ticks(adb):
    row = adb.run("shell", "getconf", "CLK_TCK")
    if row.returncode == 0 and row.stdout.strip().isdigit() and int(row.stdout) > 0:
        return int(row.stdout), "getconf"
    # 两次均调用cat，以实际工具ELF类确定auxv字宽，不能用Android主ABI猜工具字宽。
    executable = adb.run("exec-out", "cat", "/proc/self/exe")
    header = executable.stdout
    elf_class = header[4] if executable.returncode == 0 and header[:4] == b"\x7fELF" and header[5:6] == b"\x01" else None
    auxv = adb.run("exec-out", "cat", "/proc/self/auxv")
    hz = parse_auxv(auxv.stdout, elf_class) if auxv.returncode == 0 else None
    return hz, "auxv_AT_CLKTCK" if hz else None


def parse_cpu(data):
    fields = data.rsplit(b")", 1)[-1].split() if b")" in data else []
    try:
        ticks, start = int(fields[11]) + int(fields[12]), int(fields[19])
        return (ticks, start) if ticks >= 0 and start >= 0 else None
    except (IndexError, ValueError):
        return None


def collect(args, adb, monotonic=time.monotonic, sleep=time.sleep):
    require_online(adb)
    hz, source = clock_ticks(adb)
    start = monotonic()
    report = {"schema_version": 1, "samples": [], "clock_ticks_per_second": hz,
              "clock_source": source, "completed": False,
              "scope": "进程CPU为单核口径；频率仅采CPU0–3；温度保留原始值；不代表识别精度或端到端时延"}
    first_pid, previous = None, None

    def read(*command):
        row = adb.run(*command)
        return row.stdout if row.returncode == 0 else b""

    while monotonic() - start < args.duration:
        pid = process_id(adb, args.package)
        if pid is None or (first_pid is not None and first_pid != pid):
            report["stop_reason"] = "TARGET_NOT_RUNNING" if pid is None else "PID_CHANGED"
            break
        first_pid = pid
        began = monotonic()
        row = {"elapsed_seconds": round(began - start, 3)}
        cpu = parse_cpu(read("shell", "cat", f"/proc/{pid}/stat"))
        stamp = monotonic()
        if cpu and previous and hz and previous[1] == cpu[1] and stamp > previous[2] and cpu[0] >= previous[0]:
            row["process_cpu_one_core_percent"] = round(100 * (cpu[0] - previous[0]) / hz / (stamp - previous[2]), 2)
        previous = (*cpu, stamp) if cpu else None
        status = read("shell", "cat", f"/proc/{pid}/status")
        for key, output in [(b"VmRSS", "rss_kib"), (b"VmHWM", "peak_rss_kib"), (b"Threads", "thread_count")]:
            match = re.search(rb"^" + key + rb":\s*(\d+)", status, re.M)
            if match:
                row[output] = int(match[1])
        memory = read("shell", "dumpsys", "meminfo", pid)
        parsed = parse_memory(memory)
        if "pss_kib" not in parsed:
            smaps = read("shell", "cat", f"/proc/{pid}/smaps")
            parsed = parse_memory(memory, smaps)
        row.update(parsed)
        frequencies = {}
        for index in range(4):
            value = read("shell", "cat", f"/sys/devices/system/cpu/cpu{index}/cpufreq/scaling_cur_freq").strip()
            if value.isdigit() and int(value) > 0:
                frequencies[str(index)] = int(value)
        if frequencies:
            row["cpu_frequency_khz"] = frequencies
        value = read("shell", "cat", "/sys/class/thermal/thermal_zone0/temp").strip()
        if re.fullmatch(rb"-?\d+", value):
            row["thermal_zone0_raw"] = int(value)
        report["samples"].append(row)
        sleep(max(0, min(args.interval - (monotonic() - began), args.duration - (monotonic() - start))))
    else:
        report["completed"] = bool(report["samples"])
    rows = report["samples"]
    report["measurements_complete"] = bool(rows) and all(r.get("rss_kib", 0) > 0 and r.get("pss_kib", 0) > 0 for r in rows) and any("process_cpu_one_core_percent" in r for r in rows)
    return report


def main():
    parser = Parser(description=__doc__)
    for key in ["adb", "serial", "package", "output"]:
        parser.add_argument("--" + key, required=True)
    parser.add_argument("--duration", type=float, default=60)
    parser.add_argument("--interval", type=float, default=5)
    args = parser.parse_args()
    check_output(args.output)
    check_package(args.package)
    if not all(math.isfinite(v) and v > 0 for v in [args.duration, args.interval]):
        raise ToolError("INVALID_TIMING")
    report = collect(args, Adb(args.adb, args.serial))
    write_report(args.output, report)
    print("SAMPLES=" + str(len(report["samples"])))
    return 0 if report["completed"] and report["measurements_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(run_cli(main))
