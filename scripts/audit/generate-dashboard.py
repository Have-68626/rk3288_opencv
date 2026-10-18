"""无外部资源和脚本的审计摘要；只展示输入，不认证历史结论。"""
import html
import json
from pathlib import Path

from _common import Parser, load_csv, load_json, redact, run_cli, write_text


def escaped(value):
    return html.escape(redact(value), quote=True)


def table(headers, rows):
    return '<table><thead><tr>' + ''.join('<th>' + escaped(x) + '</th>' for x in headers) + '</tr></thead><tbody>' + ''.join(
        '<tr>' + ''.join('<td>' + escaped(cell) + '</td>' for cell in row) + '</tr>' for row in rows) + '</tbody></table>'


def main():
    parser = Parser(description=__doc__)
    for name in ['scorecard', 'findings', 'coverage', 'output']:
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    score = load_json(args.scorecard)
    dimensions, overall = score['dimensions'], score['overall']
    findings = [json.loads(line) for line in Path(args.findings).read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    coverage = load_csv(args.coverage, ['path', 'review_state', 'evidence_ids'])
    # 所有输入字符串经脱敏和 HTML 转义；没有内联脚本、外部字体或网络请求。
    content = '<h1>审计摘要</h1><p>基线：' + escaped(score['baseline_sha']) + '；规则：' + escaped(score['rubric_version']) + '</p>'
    content += '<p>历史输入快照，非当前验收证明。审查清单为 heuristic 启发式分派，assigned 不表示已审查。评分区间及证据覆盖比例不等于测试覆盖率。</p>'
    content += '<h2>评分</h2>' + table(['下界', '上界', '证据覆盖比例', '已知项折算分'], [[overall['lower'], overall['upper'], overall['coverage'], overall['verified_score']]])
    content += table(['维度', '权重', '下界', '上界', '证据覆盖比例'], [[d['id'], d['weight'], d['lower'], d['upper'], d['coverage']] for d in dimensions])
    content += '<h2>发现</h2>' + table(['编号', '优先级', '状态', '标题', '影响'], [[f['id'], f['severity'], f['status'], f['title'], f.get('impact', '')] for f in findings])
    content += '<h2>审查分派</h2>' + table(['路径', '状态', '证据编号'], [[row['path'], row['review_state'], row['evidence_ids']] for row in coverage])
    page = '''<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>审计摘要</title>
<style>body{font:16px system-ui;max-width:1200px;margin:32px auto;padding:0 20px;color:#182634}table{border-collapse:collapse;width:100%;margin:16px 0}th,td{border:1px solid #b8c5cf;padding:8px;text-align:left;overflow-wrap:anywhere}th{background:#eef3f7}p{line-height:1.6}</style>
</head><body>''' + content + '</body></html>\n'
    write_text(args.output, page)
    print('AUDIT_DASHBOARD_WRITTEN historical_snapshot=true')


if __name__ == '__main__':
    raise SystemExit(run_cli(main))
