"""仅共享审计输入、脱敏和独占输出，不执行历史工作区命令。"""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import sys


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(2, 'INVALID_ARGUMENTS\n')


def load_json(path):
    def reject_constant(value):
        raise ValueError('INVALID_JSON')
    return json.loads(Path(path).read_text(encoding='utf-8-sig'), parse_constant=reject_constant)


def load_csv(path, required):
    with Path(path).open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream, strict=True)
        if not reader.fieldnames or len(set(reader.fieldnames)) != len(reader.fieldnames) or not set(required).issubset(reader.fieldnames):
            raise ValueError('INVALID_HEADERS')
        rows = list(reader)
        if any(None in row or any(row[key] is None for key in required) for row in rows):
            raise ValueError('INVALID_CSV')
        # 旧准则表省略末尾未使用的 finding_ids，必需评分字段仍严格拒绝缺失。
        return [{key: value if value is not None else '' for key, value in row.items()} for row in rows]


def redact(value):
    text = str(value)
    text = re.sub(r'(?i)(https?://)[^\s/@]+@', r'\1[已脱敏]@', text)
    text = re.sub(r'(?i)\bBearer\s+[^\s;,]+', 'Bearer [已脱敏]', text)
    text = re.sub(r'''(?i)(["']?(?:password|token|secret|api[_-]?key)["']?\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^\s,;]+)''',
                  r'\1[已脱敏]', text)
    text = re.sub(r'[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}', '[已脱敏邮箱]', text)
    text = re.sub(r'(?<!\d)(?:\+?86[- ]?)?1[3-9]\d{9}(?!\d)', '[已脱敏手机号]', text)
    return text


def write_text(path, text):
    if os.path.lexists(path):
        raise ValueError('OUTPUT_EXISTS')
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    # x 模式使输入同址、现有输出和并发创建均不能被覆写。
    with output.open('x', encoding='utf-8', newline='') as stream:
        stream.write(text)


def run_cli(main):
    try:
        main()
        return 0
    except KeyboardInterrupt:
        print('CANCELLED', file=sys.stderr)
        return 130
    except Exception:
        # 输入和异常正文可能含凭据，仅返回固定诊断。
        print('INVALID_INPUT_OR_OUTPUT', file=sys.stderr)
        return 2
