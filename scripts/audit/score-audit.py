"""显式评分规则与基线；未知项保留区间，不充当已完成验收。"""
from decimal import Decimal
import json
import re

from _common import Parser, load_csv, load_json, run_cli, write_text


def number(value):
    if isinstance(value, bool):
        raise ValueError('INVALID_NUMBER')
    result = Decimal(str(value))
    if not result.is_finite():
        raise ValueError('INVALID_NUMBER')
    return result


def calculate(rubric, records, baseline):
    if not re.fullmatch(r'[0-9a-fA-F]{40}', baseline):
        raise ValueError('INVALID_BASELINE')
    if not isinstance(rubric['version'], str) or not rubric['version'].strip():
        raise ValueError('INVALID_VERSION')
    maximum = number(rubric['max_score'])
    if maximum <= 0 or maximum != maximum.to_integral_value():
        raise ValueError('INVALID_MAX_SCORE')
    dimensions = rubric['dimensions']
    expected, ids = {}, set()
    weight_sum = Decimal(0)
    for dimension in dimensions:
        key = dimension['id']
        criteria = dimension['criteria']
        weight = number(dimension['weight'])
        if not isinstance(key, str) or not key or key in ids or weight <= 0 or not isinstance(criteria, list) or not criteria:
            raise ValueError('INVALID_DIMENSION')
        ids.add(key)
        weight_sum += weight
        for criterion in criteria:
            if not isinstance(criterion, str) or not criterion or criterion in expected:
                raise ValueError('INVALID_CRITERION')
            expected[criterion] = (key, weight / len(criteria))
    if weight_sum != 100:
        raise ValueError('INVALID_WEIGHTS')
    rows = {row['criterion_id']: row for row in records}
    if len(rows) != len(records) or set(rows) != set(expected):
        raise ValueError('INVALID_CRITERIA_SET')
    for key, row in rows.items():
        dimension, weight = expected[key]
        if row['dimension_id'] != dimension or abs(number(row['weight']) - weight) > Decimal('0.000001'):
            raise ValueError('INVALID_CRITERION_WEIGHT')
        raw = row['score'].strip()
        value = None if not raw else number(raw)
        if value is not None and (value < 0 or value > maximum or value != value.to_integral_value() or not row['evidence_ids'].strip()):
            raise ValueError('INVALID_SCORE_OR_EVIDENCE')
        row = row.copy()
        row['_score'] = value
        rows[key] = row
    result, lower, known_weight = [], Decimal(0), Decimal(0)
    for dimension in dimensions:
        values = [rows[key]['_score'] for key in dimension['criteria']]
        known = [value for value in values if value is not None]
        coverage = Decimal(len(known)) / len(values)
        low = 100 * sum(known) / (maximum * len(values))
        weight = number(dimension['weight'])
        lower += weight * low / 100
        known_weight += weight * coverage
        result.append({'id': dimension['id'], 'weight': float(weight),
                       'scores': [None if value is None else int(value) for value in values],
                       'lower': round(float(low), 4),
                       'upper': round(float(low + 100 * (1 - coverage)), 4),
                       'coverage': round(float(coverage), 6)})
    coverage = known_weight / 100
    return {'baseline_sha': baseline.lower(), 'rubric_version': rubric['version'],
            'dimensions': result, 'overall': {
                'lower': round(float(lower), 4), 'upper': round(float(lower + 100 - known_weight), 4),
                'coverage': round(float(coverage), 6),
                'verified_score': round(float(lower / coverage), 4) if coverage else None,
                'provisional': True}}


def main():
    parser = Parser(description=__doc__)
    for name in ['rubric', 'criteria', 'baseline', 'output']:
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    report = calculate(load_json(args.rubric), load_csv(args.criteria,
        ['criterion_id', 'dimension_id', 'weight', 'score', 'evidence_ids']), args.baseline)
    write_text(args.output, json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False) + '\n')
    print('AUDIT_SCORE_WRITTEN provisional=true')


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
