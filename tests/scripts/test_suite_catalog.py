"""固定范围及计划不会意外执行测试或改动工作区。"""

from pathlib import Path
from types import SimpleNamespace
import importlib.util
import os
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from testing.catalog import select, native, make_suites

ROOT = Path(__file__).resolve().parents[2]


def test_selection_sorted_and_deduplicated():
    assert select(["web", "docs", "web"]) == ["docs", "web"]
    with pytest.raises(ValueError):
        select(["unknown"])


def test_missing_native_build_is_blocked(tmp_path):
    suite = make_suites(
        ROOT, ["windows"], SimpleNamespace(windows_build_dir=tmp_path), tmp_path / "out"
    )[0]
    assert "MISSING_WINDOWS_BUILD" in suite["errors"]
    assert not (tmp_path / "out").exists()


def test_shell_operators_rejected_for_batch():
    with pytest.raises(ValueError):
        native("pnpm.cmd", "value&other")


def load_cli():
    spec = importlib.util.spec_from_file_location(
        "suite_cli", ROOT / "scripts/run-test-suites.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_plan_does_not_execute_or_create_output(monkeypatch, tmp_path, capsys):
    import json

    cli = load_cli()
    monkeypatch.setattr(cli, "execute_step", lambda *a: pytest.fail("plan执行了子进程"))
    before = (
        set((ROOT / "cache/reports/test-suites").glob("*"))
        if (ROOT / "cache/reports/test-suites").exists()
        else set()
    )
    assert cli.main(["plan", "--suite", "docs", "--cpu-budget", "3", "--memory-budget-mb", "4096"]) == 0
    plan = json.loads(capsys.readouterr().out)
    assert plan["resource_budget"]["cpu_slots"] == 3
    assert plan["resource_budget"]["memory_budget_mb"] == 4096
    assert "docs-tests" in json.dumps(plan)
    after = (
        set((ROOT / "cache/reports/test-suites").glob("*"))
        if (ROOT / "cache/reports/test-suites").exists()
        else set()
    )
    assert before == after


@pytest.mark.parametrize("timeout", ["0", "-1", "nan", "inf"])
def test_invalid_timeout_rejected(timeout):
    with pytest.raises(SystemExit) as result:
        load_cli().main(["run", "--suite", "docs", "--timeout", timeout])
    assert result.value.code == 2


def test_failed_step_stops_later_steps_and_writes_report(tmp_path, monkeypatch):
    import json
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(
        cli, "binding", lambda root: {"head": "fixture", "workspace_sha256": "same"}
    )

    def suites(root, names, options, out):
        return [
            dict(
                id="docs",
                errors=[],
                steps=[
                    Step(
                        "fail", [sys.executable, "-c", "raise SystemExit(7)"], tmp_path
                    ),
                    Step(
                        "never", [sys.executable, "-c", "raise SystemExit(0)"], tmp_path
                    ),
                ],
            )
        ]

    monkeypatch.setattr(cli, "make_suites", suites)
    assert cli.main(["run", "--suite", "docs"]) == 1
    report = json.loads(
        next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text(
            encoding="utf-8"
        )
    )
    assert [row["status"] for row in report["steps"]] == ["failed", "not_run"]
    assert report["steps"][0]["raw_exit_code"] == 7


def test_source_change_invalidates_success(tmp_path, monkeypatch):
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    snapshots = iter([{"head": "a"}, {"head": "b"}])
    monkeypatch.setattr(cli, "binding", lambda root: next(snapshots))
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(
                id="docs",
                errors=[],
                steps=[Step("ok", [sys.executable, "-c", "pass"], tmp_path)],
            )
        ],
    )
    assert cli.main(["run", "--suite", "docs"]) == 2


@pytest.mark.parametrize(
    "text",
    [
        "packageManager: pnpm@10.33.0\n",
        'packageManager: "pnpm@10.33.0"\n',
        '{"packageManager": "pnpm@10.33.0"}',
    ],
)
def test_pnpm_metadata_json_and_yaml(text):
    from testing.catalog import installed_pnpm

    assert installed_pnpm(text) == "pnpm@10.33.0"


def test_contrib_modules_directory_is_valid_without_root_cmake(tmp_path, monkeypatch):
    import testing.catalog as catalog

    java = tmp_path / "jdk"
    (java / "bin").mkdir(parents=True)
    (java / "bin/java.exe").touch()
    sdk = tmp_path / "sdk"
    (sdk / "platforms").mkdir(parents=True)
    cv = tmp_path / "opencv"
    cv.mkdir()
    (cv / "CMakeLists.txt").touch()
    contrib = tmp_path / "contrib"
    (contrib / "modules").mkdir(parents=True)
    ncnn = tmp_path / "ncnn"
    for abi in ("armeabi-v7a", "arm64-v8a"):
        path = ncnn / abi / "lib/cmake/ncnn/ncnnConfig.cmake"
        path.parent.mkdir(parents=True)
        path.touch()
    monkeypatch.setattr(
        catalog.subprocess,
        "run",
        lambda *a, **k: SimpleNamespace(
            returncode=0, stdout="", stderr='openjdk version "21.0.1"'
        ),
    )
    options = SimpleNamespace(
        java_home=java,
        android_sdk="sdk",
        opencv_root=cv,
        opencv_contrib_root=contrib,
        android_ncnn_dir=ncnn,
    )
    monkeypatch.chdir(tmp_path)
    suite = make_suites(ROOT, ["android-real"], options, tmp_path / "out")[0]
    assert suite["errors"] == []
    assert suite["steps"][0].env["ANDROID_HOME"] == str(sdk.resolve())


def test_structured_hash_not_misclassified_as_phone(tmp_path):
    import json

    cli = load_cli()
    digest = "a13800138000" + "b" * 52
    report = dict(
        status="passed",
        run_id="fixture",
        selected=["docs"],
        not_selected=[],
        steps=[],
        head=digest,
    )
    cli.save(tmp_path, report)
    assert (
        json.loads((tmp_path / "summary.json").read_text(encoding="utf-8"))["head"]
        == digest
    )


def test_lock_busy_has_explicit_reason_and_unrun_steps(tmp_path, monkeypatch):
    import json
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(cli, "binding", lambda root: {"head": "fixture"})
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(
                id="docs",
                errors=[],
                steps=[Step("never", [sys.executable, "-c", "pass"], tmp_path)],
            )
        ],
    )

    def busy(root):
        raise RuntimeError("RESOURCE_BUSY")

    monkeypatch.setattr(cli, "RepoLock", busy)
    assert cli.main(["run", "--suite", "docs"]) == 2
    report = json.loads(
        next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text(
            encoding="utf-8"
        )
    )
    assert report["reason"] == "RESOURCE_BUSY"
    assert report["steps"][0]["status"] == "not_run"


@pytest.mark.parametrize(
    "relative", ["scripts/new.py", "app/src/main/New.java", "web/src/new.css"]
)
def test_binding_ignores_generated_and_binary_untracked_files(
    tmp_path, monkeypatch, relative
):
    cli = load_cli()
    source = tmp_path / relative
    source.parent.mkdir(parents=True)
    source.write_text("print(1)", encoding="utf-8")
    private = tmp_path / "private.jpg"
    private.write_bytes(b"synthetic-media")
    generated = tmp_path / "node_modules/.vite/results.json"
    generated.parent.mkdir(parents=True)
    generated.write_text("{}", encoding="utf-8")

    def git(command, **kwargs):
        if command[1] == "rev-parse":
            return b"a" * 40
        if command[1] == "diff":
            return b""
        return (
            "private.jpg\0" + relative + "\0node_modules/.vite/results.json\0"
        ).encode()

    monkeypatch.setattr(cli.subprocess, "check_output", git)
    original = Path.read_bytes

    def guarded(path):
        assert path not in (private, generated), "读取了无关媒体或生成缓存"
        return original(path)

    monkeypatch.setattr(Path, "read_bytes", guarded)
    before = cli.binding(tmp_path)
    source.write_text("print(2)", encoding="utf-8")
    assert cli.binding(tmp_path) != before


@pytest.mark.parametrize("same_root", [True, False])
def test_windows_source_root_uses_path_semantics(tmp_path, same_root):
    root = tmp_path / "ProjectRoot"
    build = root / "build"
    build.mkdir(parents=True)
    source = str(root).swapcase() if same_root else str(tmp_path / "other")
    (build / "CMakeCache.txt").write_text(
        "CMAKE_HOME_DIRECTORY:INTERNAL=" + source + "\n", encoding="utf-8"
    )
    suite = make_suites(
        root, ["windows"], SimpleNamespace(windows_build_dir=build), tmp_path / "out"
    )[0]
    assert ("WINDOWS_BUILD_SOURCE_MISMATCH" in suite["errors"]) != same_root


@pytest.mark.parametrize(
    "error,code,state",
    [
        (KeyboardInterrupt, 130, "cancelled"),
        (OSError, 2, "blocked"),
        (RuntimeError, 2, "blocked"),
        (ValueError, 2, "blocked"),
    ],
)
def test_boundary_exception_closes_running_step(
    tmp_path, monkeypatch, error, code, state
):
    import json
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(cli, "binding", lambda root: {"head": "fixture"})
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(
                id="docs",
                errors=[],
                steps=[
                    Step("boundary", [sys.executable, "-c", "pass"], tmp_path),
                    Step("never", [sys.executable, "-c", "pass"], tmp_path),
                ],
            )
        ],
    )

    def fail(*args):
        raise error()

    monkeypatch.setattr(cli, "execute_step", fail)
    assert cli.main(["run", "--suite", "docs"]) == code
    report = json.loads(
        next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text(
            encoding="utf-8"
        )
    )
    assert [row["status"] for row in report["steps"]] == [state, "not_run"]
    assert report["steps"][0]["raw_exit_code"] is None


def test_source_binding_failure_still_saves_completed_result(tmp_path, monkeypatch):
    import json
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    calls = []

    def binding(root):
        calls.append(1)
        if len(calls) == 2:
            raise OSError()
        return {"head": "fixture"}

    monkeypatch.setattr(cli, "binding", binding)
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(
                id="docs",
                errors=[],
                steps=[Step("done", [sys.executable, "-c", "pass"], tmp_path)],
            )
        ],
    )
    assert cli.main(["run", "--suite", "docs"]) == 2
    report = json.loads(
        next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text(
            encoding="utf-8"
        )
    )
    assert report["steps"][0]["status"] == "passed"
    assert report["source_binding_error"] == "SOURCE_BINDING_UNAVAILABLE"


def test_explicit_node_controls_child_path(tmp_path):
    suite = make_suites(
        ROOT, ["docs"], SimpleNamespace(node=sys.executable), tmp_path / "out"
    )[0]
    assert (
        Path(suite["steps"][0].env["PATH"].split(os.pathsep)[0])
        == Path(sys.executable).parent
    )


def test_android_workers_follow_shared_resource_budget(tmp_path, monkeypatch):
    java = tmp_path / "jdk"
    (java / "bin").mkdir(parents=True)
    (java / "bin/java.exe").touch()
    sdk = tmp_path / "sdk"
    (sdk / "platforms").mkdir(parents=True)
    cv = tmp_path / "opencv"
    cv.mkdir()
    (cv / "CMakeLists.txt").touch()
    contrib = tmp_path / "contrib"
    (contrib / "modules").mkdir(parents=True)
    ncnn = tmp_path / "ncnn"
    for abi in ("armeabi-v7a", "arm64-v8a"):
        path = ncnn / abi / "lib/cmake/ncnn/ncnnConfig.cmake"
        path.parent.mkdir(parents=True)
        path.touch()
    monkeypatch.setattr(
        "testing.catalog.subprocess.run",
        lambda *a, **k: SimpleNamespace(
            returncode=0, stdout="", stderr='openjdk version "21.0.1"'
        ),
    )
    options = SimpleNamespace(
        java_home=java,
        android_sdk=sdk,
        opencv_root=cv,
        opencv_contrib_root=contrib,
        android_ncnn_dir=ncnn,
        cpu_budget=3,
        memory_budget_mb=4096,
    )
    suite = make_suites(ROOT, ["android-real"], options, tmp_path / "out")[0]
    step = suite["steps"][0]
    assert "--max-workers=3" in step.command
    assert step.cpu_slots == 3
    assert step.memory_budget_mb == 4096


def test_suite_coordinator_rejects_overbudget_before_child(tmp_path, monkeypatch):
    import json
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    monkeypatch.setattr(cli, "binding", lambda root: {"head": "fixture"})
    monkeypatch.setattr(cli, "collect_versions", lambda *args: None)
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(
                id="docs",
                errors=[],
                steps=[Step("too-wide", [sys.executable, "-c", "pass"], tmp_path, cpu_slots=2)],
            )
        ],
    )
    monkeypatch.setattr(cli, "execute_step", lambda *args: pytest.fail("overbudget child started"))
    assert cli.main(["run", "--suite", "docs", "--cpu-budget", "1"]) == 2
    report = json.loads(next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text())
    assert report["steps"][0]["status"] == "blocked"
    assert report["reason"] == "CPU_BUDGET_EXCEEDED"


def test_cancel_retains_130_if_source_changes_during_interrupt(tmp_path, monkeypatch):
    from testing.runner import Step

    cli = load_cli()
    monkeypatch.setattr(cli, "ROOT", tmp_path)
    snapshots = iter([{"head": "a"}, {"head": "b"}])
    monkeypatch.setattr(cli, "binding", lambda root: next(snapshots))
    monkeypatch.setattr(
        cli,
        "make_suites",
        lambda *args: [
            dict(id="docs", errors=[], steps=[Step("cancel", [], tmp_path)])
        ],
    )

    def cancel(*args):
        raise KeyboardInterrupt

    monkeypatch.setattr(cli, "execute_step", cancel)
    assert cli.main(["run", "--suite", "docs"]) == 130
