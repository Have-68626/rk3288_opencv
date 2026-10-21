"""复现信息只能保存明确输入，不采集环境全集。"""

from pathlib import Path
from types import SimpleNamespace
import sys
import json
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))


def test_plan_keeps_actual_command_and_hashes_declared_configuration(tmp_path):
    from testing.metadata import plan_snapshot
    from testing.runner import Step

    web = tmp_path / "web"
    web.mkdir()
    lock = web / "pnpm-lock.yaml"
    lock.write_text("first", encoding="utf-8")
    suites = [
        dict(
            id="web",
            errors=[],
            tools={},
            steps=[
                Step(
                    "test",
                    [sys.executable, "--version"],
                    web,
                    timeout=17,
                    env={"PATH": "synthetic-private-path", "TOKEN": "synthetic-value"},
                )
            ],
        )
    ]
    first = plan_snapshot(tmp_path, suites, SimpleNamespace())
    assert first["suites"][0]["steps"][0]["timeout"] == 17
    assert first["suites"][0]["steps"][0]["cwd"] == str(web)
    encoded = json.dumps(first)
    assert "synthetic-private-path" not in encoded and "synthetic-value" not in encoded
    lock.write_text("second", encoding="utf-8")
    assert (
        first["inputs"] != plan_snapshot(tmp_path, suites, SimpleNamespace())["inputs"]
    )


def test_effective_java_path_is_recorded_without_dumping_environment(tmp_path):
    from testing.metadata import plan_snapshot
    from testing.runner import Step

    suites = [
        dict(
            id="android-mock",
            errors=[],
            steps=[
                Step(
                    "android",
                    ["gradlew.bat"],
                    tmp_path,
                    env={
                        "JAVA_HOME": str(tmp_path / "jdk"),
                        "PASSWORD": "synthetic-value",
                    },
                )
            ],
        )
    ]
    plan = plan_snapshot(tmp_path, suites, SimpleNamespace())
    assert plan["inputs"]["java_home"]["path"] == str(tmp_path / "jdk")
    assert "synthetic-value" not in json.dumps(plan)


def test_versions_use_owned_executor_and_preserve_evidence(tmp_path):
    from testing.metadata import collect_versions
    from testing.runner import Step

    suites = [
        dict(
            id="docs",
            errors=[],
            tools={"node": sys.executable},
            steps=[Step("docs", [], tmp_path)],
        )
    ]
    records = {}
    collect_versions(suites, tmp_path / "versions", records)
    assert records["python"]["version"]
    node = records["node"]
    assert node["status"] == "passed" and "Python" in node["version"]
    assert Path(node["evidence"]["path"]).is_file()


@pytest.mark.parametrize(
    "state,error", [("failed", RuntimeError), ("cancelled", KeyboardInterrupt)]
)
def test_version_failure_or_cancel_blocks_execution(
    tmp_path, monkeypatch, state, error
):
    import testing.metadata as metadata
    from testing.runner import Step

    monkeypatch.setattr(
        metadata,
        "execute_step",
        lambda *args, **kwargs: dict(status=state, raw_exit_code=7, reason="fixture"),
    )
    suites = [
        dict(
            id="docs",
            errors=[],
            tools={"node": sys.executable},
            steps=[Step("docs", [], tmp_path)],
        )
    ]
    records = {}
    with pytest.raises(error):
        metadata.collect_versions(suites, tmp_path / "versions", records)
    assert records["node"]["raw_exit_code"] == 7


def test_cli_run_archives_plan_and_versions(tmp_path, monkeypatch):
    from test_suite_catalog import load_cli
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
                steps=[Step("done", [sys.executable, "-c", "pass"], tmp_path)],
            )
        ],
    )
    assert cli.main(["run", "--suite", "docs"]) == 0
    report = json.loads(
        next((tmp_path / "cache/reports/test-suites").glob("*/summary.json")).read_text(
            encoding="utf-8"
        )
    )
    assert Path(report["execution_plan"]["path"]).is_file()
    assert report["tools"]["python"]["version"]


def test_missing_version_log_cannot_remain_passed(tmp_path, monkeypatch):
    import testing.metadata as metadata
    from testing.runner import Step

    def passed(step, output):
        return dict(status="passed", raw_exit_code=0)

    monkeypatch.setattr(metadata, "execute_step", passed)
    records = {}
    with pytest.raises(RuntimeError):
        metadata.collect_versions(
            [
                dict(
                    id="docs",
                    errors=[],
                    tools={"node": sys.executable},
                    steps=[Step("docs", [], tmp_path)],
                )
            ],
            tmp_path / "versions",
            records,
        )
    assert records["node"]["status"] == "unverified"


def test_version_probe_disables_corepack_network(tmp_path, monkeypatch):
    import testing.metadata as metadata
    from testing.runner import Step

    seen = []

    def fail(step, output):
        seen.append(step.env)
        return dict(status="failed", raw_exit_code=7)

    monkeypatch.setattr(metadata, "execute_step", fail)
    with pytest.raises(RuntimeError):
        metadata.collect_versions(
            [
                dict(
                    id="docs",
                    errors=[],
                    tools={"pnpm": sys.executable},
                    steps=[Step("docs", [], tmp_path)],
                )
            ],
            tmp_path / "versions",
            {},
        )
    assert seen[0]["COREPACK_ENABLE_NETWORK"] == "0"
