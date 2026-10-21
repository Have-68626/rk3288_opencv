#!/usr/bin/env python3
"""按明确范围运行本地测试，保留本次证据，不代替远端CI。"""
from datetime import datetime, timezone
import hashlib
import json
import math
import os
import re
from pathlib import Path
import subprocess
import sys
import uuid

from audit._common import Parser, redact
from testing.catalog import SUITES, make_suites
from testing.runner import RepoLock, execute_step
from testing.reports import identity
from testing.metadata import plan_snapshot, collect_versions

ROOT = Path(__file__).resolve().parents[1]


def binding(root):
    head = (
        subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root)
        .strip()
        .decode("ascii")
    )
    # webroot是构建输出；未跟踪内容仅绑定约定源码文本，不扫描媒体或依赖缓存。
    diff = subprocess.check_output(
        ["git", "diff", "HEAD", "--", ".", ":(exclude)src/win/app/webroot"], cwd=root
    )
    others = (
        subprocess.check_output(
            ["git", "ls-files", "--others", "--exclude-standard", "-z"], cwd=root
        )
        .decode("utf-8")
        .strip("\0")
        .split("\0")
    )
    digest = hashlib.sha256(diff)
    for name in sorted(filter(None, others)):
        path = root / name
        parts = Path(name).parts
        if (
            parts[0]
            not in ("scripts", "tests", "src", "app", "web", "docs", "cmake", "config")
            or any(
                part.casefold()
                in (
                    "node_modules",
                    "build",
                    ".cxx",
                    "dist",
                    "coverage",
                    ".nyc_output",
                    "reports",
                    "private",
                )
                for part in parts
            )
            or path.suffix.lower().lstrip(".")
            not in "py js mjs cjs ts tsx jsx css html scss svg java kt kts gradle properties cpp c h hpp md ps1 sh bat json yaml yml cmake txt ini xml csv rc def manifest".split()
            or Path(name).is_relative_to("src/win/app/webroot")
            or not path.is_file()
            or path.resolve() != path.absolute()
        ):
            continue
        digest.update(name.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return dict(head=head, workspace_sha256=digest.hexdigest())


def sanitize(value, key=""):
    if isinstance(value, dict):
        return {k: sanitize(v, k) for k, v in value.items()}
    if isinstance(value, list):
        return [sanitize(item) for item in value]
    if isinstance(value, str):
        if (key == "head" or key.endswith("sha256")) and re.fullmatch(
            r"[a-f0-9]{40}|[a-f0-9]{64}", value
        ):
            return value
        return redact(value)
    return value


def save(out, report):
    text = json.dumps(sanitize(report), ensure_ascii=False, indent=2) + "\n"
    temporary = out / "summary.pending"
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, out / "summary.json")
    lines = [
        "# 本地测试套件结果",
        "",
        f"状态：{report['status']}",
        f"运行：{report['run_id']}",
        "原因：" + report.get("reason", ""),
        "范围：" + ", ".join(report["selected"]),
        "未选：" + ", ".join(report["not_selected"]),
        "",
        "| 套件 | 步骤 | 状态 | 原始退出码 | 计数与单位 | 原因 |",
        "|---|---|---|---|---|",
    ]
    for row in report["steps"]:
        counts = row.get("counts")
        detail = "无用例计数"
        if counts:
            detail = f"{counts['unit']}: 总计{counts['total']}，失败{counts['failed']}，跳过{counts['skipped']}"
        if row.get("internal_case_counts") == "unknown":
            detail += "；内部用例数未知"
        if row.get("observed_markers"):
            detail += "；" + ", ".join(row["observed_markers"])
        lines.append(
            f"| {row['suite']} | {row['name']} | {row['status']} | {row.get('raw_exit_code')} | {detail} | {row.get('reason','')} |"
        )
    lines += [
        "",
        "跳过、内部未知计数和UNVERIFIED不代表真实精度通过；未选套件未验收。",
        "本报告不是远端全量CI通过声明。",
    ]
    temporary = out / "summary-md.pending"
    temporary.write_text(redact("\n".join(lines)) + "\n", encoding="utf-8")
    os.replace(temporary, out / "summary.md")


def parser():
    p = Parser(description=__doc__)
    p.add_argument("action", choices=("list", "plan", "run"))
    p.add_argument("--suite", action="append", choices=SUITES)
    p.add_argument("--timeout", type=float)
    for flag in (
        "windows-build-dir",
        "web-dir",
        "java-home",
        "android-sdk",
        "opencv-root",
        "opencv-contrib-root",
        "android-ncnn-dir",
        "precision-tools-dir",
        "ncnn2table",
        "ncnn2int8",
        "public-cascade",
        "node",
        "pnpm",
        "cmake",
        "ctest",
        "pwsh",
    ):
        p.add_argument("--" + flag)
    return p


def main(argv=None):
    p = parser()
    options = p.parse_args(argv)
    if options.action == "list":
        print("\n".join(SUITES))
        return 0
    if not options.suite:
        p.error("缺少套件")
    if options.timeout is not None and (
        not math.isfinite(options.timeout) or options.timeout <= 0
    ):
        p.error("无效超时")
    if os.name != "nt":
        print("UNSUPPORTED_HOST", file=sys.stderr)
        return 2
    run_id = (
        datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
        + "-"
        + uuid.uuid4().hex[:6]
    )
    out = ROOT / "cache/reports/test-suites" / run_id
    try:
        suites = make_suites(ROOT, options.suite, options, out)
    except (ValueError, OSError, subprocess.SubprocessError):
        print("PLAN_FAILED", file=sys.stderr)
        return 2
    if options.action == "plan":
        print(
            json.dumps(
                sanitize(plan_snapshot(ROOT, suites, options)["suites"]),
                ensure_ascii=False,
                indent=2,
            )
        )
        return 0
    report = dict(
        schema_version=1,
        run_id=run_id,
        status="running",
        selected=[s["id"] for s in suites],
        not_selected=[s for s in SUITES if s not in options.suite],
        started=datetime.now(timezone.utc).isoformat(),
        steps=[],
        tools={},
        source_start=binding(ROOT),
    )
    # cache中的链接不能把本次输出重定向到其他工作区。
    if out.parent.resolve() != out.parent.absolute():
        print("UNSAFE_OUTPUT_ROOT", file=sys.stderr)
        return 2
    report["steps"] = [
        dict(
            suite=s["id"],
            name=step.name,
            status="not_run",
            raw_exit_code=None,
            reason="NOT_STARTED",
        )
        for s in suites
        for step in s["steps"]
    ]
    out.mkdir(parents=True, exist_ok=False)
    plan_file = out / "plan.json"
    with plan_file.open("x", encoding="utf-8") as stream:
        json.dump(
            sanitize(plan_snapshot(ROOT, suites, options)),
            stream,
            ensure_ascii=False,
            indent=2,
        )
    report["execution_plan"] = dict(path=str(plan_file), **identity(plan_file))
    save(out, report)
    exit_code = 0
    stopped = False
    position = 0
    try:
        with RepoLock(ROOT):
            collect_versions(suites, out / "versions", report["tools"])
            save(out, report)
            for suite in suites:
                for step in suite["steps"]:
                    row = dict(
                        suite=suite["id"],
                        name=step.name,
                        status="not_run",
                        raw_exit_code=None,
                        reason="PREVIOUS_FAILURE",
                    )
                    if not stopped:
                        if suite["errors"]:
                            row.update(
                                status="blocked", reason=",".join(suite["errors"])
                            )
                            exit_code = 2
                        else:
                            print(
                                f"START suite={suite['id']} step={step.name}",
                                flush=True,
                            )
                            row["status"] = "running"
                            report["steps"][position] = row
                            save(out, report)
                            row.update(
                                execute_step(step, out / suite["id"] / step.name)
                            )
                            print(
                                f"END suite={suite['id']} step={step.name} status={row['status']}",
                                flush=True,
                            )
                            if row["status"] != "passed":
                                exit_code = {
                                    "failed": 1,
                                    "timed_out": 124,
                                    "cancelled": 130,
                                }.get(row["status"], 2)
                        stopped = exit_code != 0
                    report["steps"][position] = row
                    position += 1
                    save(out, report)
    except KeyboardInterrupt:
        report["reason"] = "CANCELLED"
        exit_code = 130
    except RuntimeError as error:
        report["reason"] = (
            str(error)
            if str(error)
            in (
                "RESOURCE_BUSY",
                "LOCK_FAILED",
                "LOCK_RELEASE_FAILED",
                "UNSUPPORTED_HOST",
                "TOOL_VERSION_UNVERIFIED",
            )
            else "COORDINATION_OR_REPORT_FAILED"
        )
        exit_code = 2
    except (OSError, ValueError):
        report["reason"] = "COORDINATION_OR_REPORT_FAILED"
        exit_code = 2
    try:
        report["source_end"] = binding(ROOT)
    except (OSError, ValueError, subprocess.SubprocessError):
        report["source_end"] = None
        report["source_binding_error"] = "SOURCE_BINDING_UNAVAILABLE"
        if exit_code == 0:
            report["reason"] = "SOURCE_BINDING_UNAVAILABLE"
            exit_code = 2
    if (
        report["source_end"] is not None
        and report["source_start"] != report["source_end"]
    ):
        report["source_changed"] = True
        if exit_code != 130:
            report["reason"] = "SOURCE_CHANGED_DURING_RUN"
            exit_code = 2
    # 边界异常可能发生在执行器接手前，终态不能残留running；已完成证据不重写。
    for row in report["steps"]:
        if row["status"] == "running":
            row.update(
                status="cancelled" if exit_code == 130 else "blocked",
                reason=report.get("reason", "COORDINATION_OR_REPORT_FAILED"),
            )
    report["selected_execution_complete"] = all(
        row["status"] in ("passed", "failed") for row in report["steps"]
    )
    report["has_skips_or_unverified"] = any(
        row.get("counts", {}).get("skipped")
        or row.get("observed_markers")
        or row["status"] == "unverified"
        for row in report["steps"]
    )
    report.update(
        status="passed" if exit_code == 0 else "incomplete",
        exit_code=exit_code,
        ended=datetime.now(timezone.utc).isoformat(),
    )
    save(out, report)
    print("REPORT=" + str(out / "summary.md"), flush=True)
    return exit_code


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("CANCELLED", file=sys.stderr)
        raise SystemExit(130)
    except Exception:
        print("COORDINATOR_FAILED", file=sys.stderr)
        raise SystemExit(2)
