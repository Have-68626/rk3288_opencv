#!/usr/bin/env python3
"""固定配置的本地构建与验证，不执行任意命令或远程发布。"""
from datetime import datetime, timezone
from dataclasses import asdict
import sys
from pathlib import Path
import uuid

from audit._common import Parser
from building.profiles import PROFILES, load_inputs, make_plan, validate_build_time
from building.coordinator import record_json, run, command_steps
from testing.resources import add_budget_arguments, budget_from_options

ROOT = Path(__file__).resolve().parents[1]


def parser():
    value = Parser(description=__doc__)
    value.add_argument('action', choices=('list', 'plan', 'configure', 'build', 'verify'))
    value.add_argument('--profile', choices=PROFILES)
    value.add_argument('--inputs')
    value.add_argument('--report')
    value.add_argument('--build-time', help='Android诊断时间，格式为 YYYYMMDD-HHmm')
    add_budget_arguments(value)
    return value


def main(argv=None):
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    argument_parser = parser()
    options = argument_parser.parse_args(argv)
    try:
        budget = budget_from_options(options)
    except ValueError as error:
        argument_parser.error(str(error))
    if options.action == 'list':
        print('\n'.join(PROFILES))
        return 0
    if not options.profile or not options.inputs or (options.action == 'verify' and not options.report):
        argument_parser.error('MISSING_ARGUMENTS')
    if options.build_time is not None:
        if options.profile not in ('android-mock', 'android-real'):
            print('BUILD_TIME_ANDROID_APP_ONLY', file=sys.stderr)
            argument_parser.error('INVALID_ARGUMENTS')
        try:
            validate_build_time(options.build_time)
        except ValueError as error:
            print(str(error), file=sys.stderr)
            argument_parser.error('INVALID_ARGUMENTS')
    try:
        inputs = load_inputs(options.inputs)
        run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:6]
        out = ROOT / 'cache/reports/build-coordination' / run_id
        plan = make_plan(ROOT, options.profile, inputs, out, build_time=options.build_time)
        plan['resource_budget'] = dict(
            cpu_slots=budget.cpu_slots,
            memory_budget_mb=budget.memory_budget_mb,
            max_parallel_steps=budget.max_parallel_steps,
        )
        if options.action == 'plan':
            plan['steps'] = []
            if not plan['errors']:
                for phase, step in command_steps('build', plan):
                    value = {key: item for key, item in asdict(step).items() if key != 'env'}
                    value.update(phase=phase, environment_keys=sorted(step.env))
                    plan['steps'].append(value)
            print(record_json(plan))
            return 2 if plan['errors'] else 0
        result = run(options.action, plan, options.report)
        print('REPORT=' + str(out / 'summary.md'))
        return result['exit_code']
    except KeyboardInterrupt:
        print('CANCELLED', file=sys.stderr)
        return 130
    except Exception:
        print('BUILD_COORDINATION_FAILED', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
