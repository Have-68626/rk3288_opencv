"""变异只有真实断言失败才算检出，环境或收集错误必须拒绝。"""

from pathlib import Path
import sys
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))


@pytest.mark.parametrize(
    "kind,message,exit_code,expected",
    [
        ("failure", "AssertionError: mismatch", 1, True),
        ("failure", "Failed: DID NOT RAISE ValueError", 1, True),
        ("error", "collection failure", 1, False),
        ("failure", "ImportError: missing dependency", 1, False),
        ("failure", "SyntaxError: invalid", 1, False),
        ("failure", "AssertionError: mismatch", 0, False),
    ],
)
def test_mutation_detection_rejects_environment_errors(
    tmp_path, kind, message, exit_code, expected
):
    from testing.mutations import is_detected
    from testing.reports import identity

    path = tmp_path / "junit.xml"
    path.write_text(
        f'<testsuite><testcase><{kind} message="{message}"/></testcase></testsuite>',
        encoding="utf-8",
    )
    result = dict(
        status="failed",
        raw_exit_code=exit_code,
        evidence=[dict(path=str(path), **identity(path))],
    )
    assert is_detected(result) == expected


def test_mutation_without_current_evidence_is_not_detected():
    from testing.mutations import is_detected

    assert not is_detected(dict(status="failed", raw_exit_code=1, evidence=[]))


def test_failed_baseline_prevents_all_mutants(tmp_path, monkeypatch):
    import testing.mutations as mutations

    root = Path(__file__).resolve().parents[2]
    calls = []

    def fail(step, output):
        calls.append(step.name)
        return dict(status="failed", raw_exit_code=1, reason="fixture")

    monkeypatch.setattr(mutations, "execute_step", fail)
    report = mutations.run_mutations(root, tmp_path / "result", ["ignore-exit"], 30)
    assert calls == ["control"]
    assert report["exit_code"] != 0 and report["results"][0]["status"] == "not_run"


def test_mutation_copy_preserves_original_sources(tmp_path):
    from testing.mutations import clone_sources, MUTATIONS

    root = Path(__file__).resolve().parents[2]
    source = root / MUTATIONS[0].file
    before = source.read_bytes()
    clone_sources(root, tmp_path / "copy")
    target = tmp_path / "copy" / MUTATIONS[0].file
    target.write_text("synthetic changed copy", encoding="utf-8")
    assert source.read_bytes() == before


def test_non_unique_target_never_executes_mutant(tmp_path, monkeypatch):
    import testing.mutations as mutations

    root = Path(__file__).resolve().parents[2]
    original = mutations.MUTATIONS[0]
    monkeypatch.setattr(
        mutations,
        "MUTATIONS",
        (
            mutations.Mutation(
                original.name,
                original.file,
                "synthetic missing target",
                original.new,
                original.test,
            ),
        ),
    )
    calls = []

    def control(step, output):
        calls.append(step.name)
        return dict(status="passed", raw_exit_code=0, counts=dict(total=1, skipped=0))

    monkeypatch.setattr(mutations, "execute_step", control)
    report = mutations.run_mutations(root, tmp_path / "result", [original.name], 30)
    assert report["exit_code"] == 2 and calls == ["control"]
    assert report["source_unchanged"]
