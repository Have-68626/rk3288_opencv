"""只在隔离pytest进程中装配指定副本，禁止意外测试原模块。"""

import importlib.util
import os
from pathlib import Path
import sys


def pytest_sessionstart(session):
    root = Path(os.environ["RK_SUITE_MUTANT"]).resolve()
    sys.path.insert(0, str(root / "scripts"))
    import testing.runner
    import testing.reports
    import testing.catalog

    assert Path(testing.runner.__file__).resolve().is_relative_to(root)


def pytest_collection_modifyitems(items):
    def loader():
        path = Path(os.environ["RK_SUITE_MUTANT"]) / "scripts/run-test-suites.py"
        spec = importlib.util.spec_from_file_location("mutated_cli", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    for item in items:
        if hasattr(item.module, "load_cli"):
            item.module.load_cli = loader
