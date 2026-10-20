"""离线汇总白名单数值；每个输入为独立窗口，不回填缺测。"""
import hashlib
import math
from pathlib import Path
import statistics

from _tool_support import Parser, ToolError, check_output, load_json, run_cli, write_report

FIELDS = ["rss_kib", "peak_rss_kib", "thread_count", "process_cpu_one_core_percent", "thermal_zone0_raw"]


def stats(values):
    if not values:
        return None
    if any(type(v) not in (int, float) or not math.isfinite(v) for v in values):
        raise ToolError("INVALID_METRIC")
    values = sorted(values)
    return {"samples": len(values), "min": values[0], "median": statistics.median(values),
            "p95": values[math.ceil(.95 * len(values)) - 1], "max": values[-1]}


def summarize(inputs):
    windows = []
    for index, path in enumerate(inputs):
        data = load_json(path)
        if not isinstance(data, dict) or not ("samples" in data or "pss_kib_samples" in data):
            raise ToolError("INVALID_REPORT")
        rows = data.get("samples", [])
        if not isinstance(rows, list) or any(not isinstance(r, dict) for r in rows):
            raise ToolError("INVALID_REPORT")
        legacy_pss = data.get("pss_kib_samples", [])
        if not isinstance(legacy_pss, list):
            raise ToolError("INVALID_REPORT")
        if any(type(v) not in (int, float) or not math.isfinite(v) for v in legacy_pss):
            raise ToolError("INVALID_METRIC")
        # 保留无RSS的独立PSS诊断；有RSS字段的运行样本不能把退出零值当稳态。
        runtime_window = any("rss_kib" in r for r in rows)
        valid = [r for r in rows if not runtime_window or
                 (type(r.get("rss_kib")) in (int, float) and r["rss_kib"] > 0)]
        metrics = {key: stats([r[key] for r in valid if key in r]) for key in FIELDS}
        by_source = {}
        for source in ["dumpsys", "smaps"]:
            values = [r["pss_kib"] for r in valid if r.get("pss_source") == source and r.get("pss_kib", 0) > 0]
            if source == "smaps":
                values += [r["smaps_pss_kib"] for r in valid if r.get("smaps_pss_kib", 0) > 0]
            else:
                # 旧驱动同样用零值表示缺测，不能将其混入有效PSS统计。
                values += [v for v in legacy_pss if v > 0]
            if values:
                by_source[source] = stats(values)
        window = {"input_index": index, "input_sha256": hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                  "sample_count": len(rows), "excluded_rss_samples": len(rows) - len(valid),
                  "metrics": metrics, "pss_by_source": by_source,
                  "completed": data.get("completed") if type(data.get("completed")) is bool else None}
        windows.append(window)
    return {"schema_version": 1, "windows": windows,
            "scope": "仅数值汇总；不同文件独立，PSS按来源区分，不证明功能或完整精度验收"}


def main():
    parser = Parser(description=__doc__)
    parser.add_argument("--inputs", nargs="+", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    check_output(args.output)
    report = summarize(args.inputs)
    write_report(args.output, report)
    print("WINDOWS=" + str(len(report["windows"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(run_cli(main))
