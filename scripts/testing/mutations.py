"""固定变异与判据；未变异的外层Job负责收回测试故意泄漏的后代。"""

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

from .runner import Step, execute_step
from .reports import identity, read_junit


@dataclass(frozen=True)
class Mutation:
    name: str
    file: str
    old: str
    new: str
    test: str


MUTATIONS = (
    Mutation(
        "ignore-exit",
        "scripts/testing/runner.py",
        'status="passed" if code == 0 else "failed"',
        'status="passed"',
        "tests/scripts/test_suite_runner.py::test_nonzero_is_failure_and_log_redacted",
    ),
    Mutation(
        "old-report",
        "scripts/testing/reports.py",
        'raise ValueError("STALE_JUNIT")',
        "pass",
        "tests/scripts/test_suite_reports.py::test_old_unchanged_report_rejected",
    ),
    Mutation(
        "empty-report",
        "scripts/testing/reports.py",
        'raise ValueError("EMPTY_JUNIT")',
        "pass",
        "tests/scripts/test_suite_reports.py::test_invalid_or_contradictory_reports_rejected",
    ),
    Mutation(
        "continue-after-failure",
        "scripts/run-test-suites.py",
        "stopped = exit_code != 0",
        "stopped = False",
        "tests/scripts/test_suite_catalog.py::test_failed_step_stops_later_steps_and_writes_report",
    ),
    Mutation(
        "ignore-lock",
        "scripts/testing/runner.py",
        "CreateMutexW(None, False, self.name)",
        "CreateMutexW(None, False, self.name + str(os.getpid()))",
        "tests/scripts/test_suite_runner.py::test_second_process_cannot_acquire_repo_lock",
    ),
    Mutation(
        "leak-descendants",
        "scripts/testing/runner.py",
        "    def close(self):\n",
        "    def close(self):\n        return\n",
        "tests/scripts/test_suite_runner.py::test_timeout_kills_owned_descendant",
    ),
)


def source_files(root):
    return sorted(
        [
            *Path(root).joinpath("scripts/testing").glob("*.py"),
            Path(root) / "scripts/audit/_common.py",
            Path(root) / "scripts/run-test-suites.py",
            Path(root) / "scripts/check-test-suite-mutations.py",
            *[
                Path(root) / "tests/scripts" / name
                for name in (
                    "test_suite_catalog.py",
                    "test_suite_runner.py",
                    "test_suite_reports.py",
                    "fixtures/suite_mutation_plugin.py",
                )
            ],
        ]
    )


def clone_sources(root, target):
    for source in source_files(root):
        destination = Path(target) / source.relative_to(root)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as stream:
            stream.write(source.read_bytes())


def is_detected(result):
    if result.get("raw_exit_code") != 1 or result.get("status") != "failed":
        return False
    try:
        evidence = result.get("evidence", [])
        paths = [Path(item["path"]) for item in evidence]
        for path, item in zip(paths, evidence):
            if identity(path)["sha256"] != item["sha256"]:
                return False
        counts = read_junit(paths)
        if not counts["failed"] or counts["skipped"]:
            return False
        failures = []
        for path in paths:
            document = ET.fromstring(path.read_text(encoding="utf-8-sig"))
            if list(document.iter("error")):
                return False
            failures.extend(document.iter("failure"))
        for failure in failures:
            text = " ".join([*failure.attrib.values(), *failure.itertext()])
            if any(
                marker in text
                for marker in (
                    "ImportError",
                    "ModuleNotFoundError",
                    "SyntaxError",
                    "ERROR collecting",
                    "fixture '",
                )
            ):
                return False
            if not any(
                marker in text for marker in ("AssertionError", "Failed: DID NOT RAISE")
            ):
                return False
        return bool(failures)
    except (OSError, ValueError, KeyError, ET.ParseError):
        return False


def run_mutations(root, output, names=None, timeout=90):
    root = Path(root).resolve()
    output = Path(output).absolute()
    selected = [m for m in MUTATIONS if names is None or m.name in names]
    if not selected or (names is not None and set(names) - {m.name for m in MUTATIONS}):
        raise ValueError("INVALID_MUTATION")
    if output.resolve() != output:
        raise ValueError("UNSAFE_OUTPUT")
    output.mkdir(parents=True, exist_ok=False)
    before = {
        str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in source_files(root)
    }
    report = dict(
        schema_version=1,
        status="running",
        exit_code=2,
        results=[dict(name=m.name, status="not_run", detected=False) for m in selected],
    )

    def save():
        pending = output / "results.pending"
        pending.write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )
        os.replace(pending, output / "results.json")

    def execute(name, tests):
        target = output / name / "source"
        clone_sources(root, target)
        if name != "control":
            mutation = next(m for m in selected if m.name == name)
            path = target / mutation.file
            text = path.read_text(encoding="utf-8")
            if text.count(mutation.old) != 1:
                raise ValueError("NON_UNIQUE_MUTATION_TARGET")
            path.write_text(text.replace(mutation.old, mutation.new), encoding="utf-8")
        execution = output / name / "execution"
        temporary = (
            root
            / "cache/reports/.tmp"
            / hashlib.sha256(str(output).encode()).hexdigest()[:12]
        )
        command = [
            sys.executable,
            "-m",
            "pytest",
            "-p",
            "suite_mutation_plugin",
            *tests,
            "-q",
            "--tb=line",
            "--basetemp",
            str(output / name / "p"),
            "--junitxml",
            str(execution / "junit.xml"),
        ]
        return execute_step(
            Step(
                name,
                command,
                root,
                timeout=timeout,
                junit=(str(execution / "junit.xml"),),
                env={
                    "RK_SUITE_MUTANT": str(target),
                    "PYTHONPATH": str(root / "tests/scripts/fixtures"),
                    "PYTEST_DISABLE_PLUGIN_AUTOLOAD": "1",
                    "TEMP": str(temporary),
                    "TMP": str(temporary),
                },
            ),
            execution,
        )

    save()
    try:
        report["baseline"] = execute("control", sorted({m.test for m in selected}))
        baseline = report["baseline"]
        if (
            baseline["status"] != "passed"
            or not baseline.get("counts", {}).get("total")
            or baseline["counts"]["skipped"]
        ):
            report["reason"] = "BASELINE_NOT_VERIFIED"
            report["exit_code"] = 130 if baseline["status"] == "cancelled" else 2
        else:
            report["exit_code"] = 0
            for index, mutation in enumerate(selected):
                result = execute(mutation.name, [mutation.test])
                detected = is_detected(result)
                report["results"][index] = dict(
                    name=mutation.name,
                    status="detected" if detected else result["status"],
                    detected=detected,
                    execution=result,
                )
                print(f"MUTATION name={mutation.name} detected={detected}", flush=True)
                save()
                if result["status"] == "cancelled":
                    report["exit_code"] = 130
                    break
                if not detected:
                    report["exit_code"] = 1
    except KeyboardInterrupt:
        report.update(exit_code=130, reason="CANCELLED")
    except (OSError, RuntimeError, ValueError):
        report.update(exit_code=2, reason="MUTATION_SETUP_OR_REPORT_FAILED")
    finally:
        after = {
            str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in source_files(root)
        }
        report["source_unchanged"] = before == after
        if before != after:
            report.update(exit_code=2, reason="SOURCE_CHANGED")
        report["source_sha256"] = hashlib.sha256(
            json.dumps(before, sort_keys=True).encode()
        ).hexdigest()
        report["status"] = "passed" if report["exit_code"] == 0 else "incomplete"
        save()
    return report
