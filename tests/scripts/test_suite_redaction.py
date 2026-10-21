"""先脱敏字段再编码，防止日志正则破坏结构化证据。"""
from pathlib import Path
import json
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'scripts'))


def test_sensitive_values_are_redacted_without_corrupting_json():
    from testing.redaction import safe_json
    text=safe_json(dict(source='token=synthetic-private-value',token='synthetic-private-value',
                        sha256='a'*64, nested=['public@example.invalid']))
    data=json.loads(text)
    assert 'synthetic-private-value' not in text
    assert 'public@example.invalid' not in text
    assert data['sha256']=='a'*64
