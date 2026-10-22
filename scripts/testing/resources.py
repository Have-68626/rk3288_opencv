"""本地测试与构建协调器共享的串行资源契约。"""

from contextlib import contextmanager
from dataclasses import dataclass
import threading


def add_budget_arguments(parser):
    parser.add_argument("--cpu-budget", type=int, default=2)
    parser.add_argument("--memory-budget-mb", type=int)
    return parser


def budget_from_options(options):
    return ResourceBudget(
        cpu_slots=getattr(options, "cpu_budget", 2),
        memory_budget_mb=getattr(options, "memory_budget_mb", None),
    )


def _positive_integer(value, reason):
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(reason)


@dataclass(frozen=True)
class ResourceBudget:
    cpu_slots: int = 2
    memory_budget_mb: int | None = None
    max_parallel_steps: int = 1

    def __post_init__(self):
        _positive_integer(self.cpu_slots, "INVALID_CPU_BUDGET")
        if self.memory_budget_mb is not None:
            _positive_integer(self.memory_budget_mb, "INVALID_MEMORY_BUDGET")
        if type(self.max_parallel_steps) is not int:
            raise ValueError("INVALID_PARALLEL_LIMIT")
        if self.max_parallel_steps != 1:
            raise ValueError("PARALLEL_STEPS_UNSUPPORTED")


@dataclass(frozen=True)
class ResourceClaim:
    cpu_slots: int = 1
    memory_budget_mb: int | None = None
    exclusive: tuple[str, ...] = ()


class ResourceCoordinator:
    """校验单步预算并保护串行执行中的独占资源键。"""

    def __init__(self, budget=None):
        self.budget = budget or ResourceBudget()
        self._active_steps = 0
        self._active_keys = set()
        self._guard = threading.Lock()

    def validate(self, claim):
        _positive_integer(claim.cpu_slots, "INVALID_CPU_CLAIM")
        if claim.cpu_slots > self.budget.cpu_slots:
            raise ValueError("CPU_BUDGET_EXCEEDED")
        if claim.memory_budget_mb is not None:
            _positive_integer(claim.memory_budget_mb, "INVALID_MEMORY_CLAIM")
            if (
                self.budget.memory_budget_mb is not None
                and claim.memory_budget_mb > self.budget.memory_budget_mb
            ):
                raise ValueError("MEMORY_BUDGET_EXCEEDED")
        keys = claim.exclusive
        if not isinstance(keys, tuple) or any(
            not isinstance(key, str) or not key.strip() for key in keys
        ):
            raise ValueError("INVALID_RESOURCE_KEY")
        if len(set(keys)) != len(keys):
            raise ValueError("DUPLICATE_RESOURCE_KEY")

    @contextmanager
    def acquire(self, claim):
        self.validate(claim)
        with self._guard:
            if self._active_steps >= self.budget.max_parallel_steps or any(
                key in self._active_keys for key in claim.exclusive
            ):
                raise RuntimeError("RESOURCE_BUSY")
            self._active_steps += 1
            self._active_keys.update(claim.exclusive)
        try:
            yield
        finally:
            with self._guard:
                self._active_steps -= 1
                self._active_keys.difference_update(claim.exclusive)
