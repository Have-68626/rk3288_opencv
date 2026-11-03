#!/usr/bin/env python3
"""在独立源码副本中验证测试统筹的六组行为变异。"""

from datetime import datetime, timezone
import math
from pathlib import Path
import sys
import uuid
from audit._common import Parser
from testing.mutations import MUTATIONS, run_mutations


def main(argv=None):
    parser = Parser(description=__doc__)
    parser.add_argument("--list", action="store_true")
    parser.add_argument(
        "--mutation", action="append", choices=[m.name for m in MUTATIONS]
    )
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args(argv)
    if args.list:
        print("\n".join(m.name for m in MUTATIONS))
        return 0
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("INVALID_TIMEOUT")
    root = Path(__file__).resolve().parents[1]
    output = (
        root
        / "cache/reports/test-suite-mutations"
        / (
            datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
            + "-"
            + uuid.uuid4().hex[:6]
        )
    )
    report = run_mutations(root, output, args.mutation, args.timeout)
    print("REPORT=" + str(output / "results.json"))
    return report["exit_code"]


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("CANCELLED", file=sys.stderr)
        raise SystemExit(130)
    except Exception:
        print("MUTATION_TOOL_FAILED", file=sys.stderr)
        raise SystemExit(2)
