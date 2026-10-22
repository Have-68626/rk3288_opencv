"""共享构建资源预算与互斥契约。"""

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import threading
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from testing.resources import (
    ResourceBudget,
    ResourceClaim,
    ResourceCoordinator,
    add_budget_arguments,
    budget_from_options,
)


def test_budget_is_serial_and_rejects_unsupported_parallelism():
    assert ResourceBudget().max_parallel_steps == 1
    with pytest.raises(ValueError, match="PARALLEL_STEPS_UNSUPPORTED"):
        ResourceBudget(max_parallel_steps=2)
    with pytest.raises(ValueError, match="INVALID_PARALLEL_LIMIT"):
        ResourceBudget(max_parallel_steps=True)
    with pytest.raises(ValueError, match="INVALID_PARALLEL_LIMIT"):
        ResourceBudget(max_parallel_steps=1.0)


def test_claim_over_cpu_budget_is_rejected_before_acquisition():
    coordinator = ResourceCoordinator(ResourceBudget(cpu_slots=1))
    with pytest.raises(ValueError, match="CPU_BUDGET_EXCEEDED"):
        coordinator.validate(ResourceClaim(cpu_slots=2))


def test_claim_over_memory_budget_is_rejected():
    coordinator = ResourceCoordinator(
        ResourceBudget(cpu_slots=2, memory_budget_mb=512)
    )
    with pytest.raises(ValueError, match="MEMORY_BUDGET_EXCEEDED"):
        coordinator.validate(ResourceClaim(memory_budget_mb=513))


def test_duplicate_resource_key_in_one_claim_is_rejected():
    coordinator = ResourceCoordinator(ResourceBudget())
    with pytest.raises(ValueError, match="DUPLICATE_RESOURCE_KEY"):
        coordinator.validate(ResourceClaim(exclusive=("webroot", "webroot")))


def test_same_active_output_resource_is_rejected_and_released():
    coordinator = ResourceCoordinator(ResourceBudget())
    claim = ResourceClaim(exclusive=("output:webroot",))
    entered = threading.Event()
    release = threading.Event()

    def hold():
        with coordinator.acquire(claim):
            entered.set()
            assert release.wait(5)

    with ThreadPoolExecutor(max_workers=2) as pool:
        first = pool.submit(hold)
        assert entered.wait(5)
        with pytest.raises(RuntimeError, match="RESOURCE_BUSY"):
            with coordinator.acquire(claim):
                pass
        release.set()
        first.result(timeout=5)

    with coordinator.acquire(claim):
        pass


def test_different_output_resources_are_reused_sequentially():
    coordinator = ResourceCoordinator(ResourceBudget())
    with coordinator.acquire(ResourceClaim(exclusive=("output:webroot",))):
        pass
    with coordinator.acquire(ResourceClaim(exclusive=("output:apk",))):
        pass


def test_different_resources_still_respect_serial_step_limit():
    coordinator = ResourceCoordinator(ResourceBudget())
    with coordinator.acquire(ResourceClaim(exclusive=("output:webroot",))):
        with pytest.raises(RuntimeError, match="RESOURCE_BUSY"):
            with coordinator.acquire(ResourceClaim(exclusive=("output:apk",))):
                pass


def test_cli_budget_arguments_share_the_same_contract():
    import argparse

    parser = argparse.ArgumentParser()
    add_budget_arguments(parser)
    options = parser.parse_args(["--cpu-budget", "4", "--memory-budget-mb", "8192"])
    assert budget_from_options(options) == ResourceBudget(
        cpu_slots=4, memory_budget_mb=8192, max_parallel_steps=1
    )


def test_cli_budget_defaults_and_invalid_values():
    import argparse

    parser = argparse.ArgumentParser()
    add_budget_arguments(parser)
    options = parser.parse_args([])
    assert budget_from_options(options) == ResourceBudget()
    with pytest.raises(ValueError, match="INVALID_CPU_BUDGET"):
        budget_from_options(parser.parse_args(["--cpu-budget", "0"]))
