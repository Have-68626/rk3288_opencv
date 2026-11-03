#!/bin/bash
# 统一词法实现，避免awk与Python的作用域规则再次漂移。
set -e
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python}" "$SCRIPT_DIR/check-raii-violations.py" "$@"
