"""逐字段脱敏后编码，避免把JSON语法交给日志替换规则。"""
import json
import re
from audit._common import redact


def safe_json(value):
    def clean(item,key=''):
        if re.fullmatch(r'password|token|secret|api[_-]?key',key,re.I):
            return '[已脱敏]'
        if isinstance(item,dict):return {k:clean(v,k) for k,v in item.items()}
        if isinstance(item,list):return [clean(v) for v in item]
        if isinstance(item,str):
            if key.endswith('sha256') and re.fullmatch('[a-f0-9]{64}',item):return item
            return redact(item)
        return item
    return json.dumps(clean(value),ensure_ascii=False,indent=2,allow_nan=False)
