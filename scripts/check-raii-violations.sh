#!/bin/bash
# RAII 违规检查
# 检查所有裸 lock/unlock 调用（未在 RAII 类中封装）
# 遵守架构契约 #1
# 用法: ./scripts/check-raii-violations.sh [--ci]

echo "=== RAII 违规检查 ==="

# 找到项目根目录
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || echo .)"
cd "$ROOT" || exit 1

violations=0
# 要检查的裸调用模式
RAII_VIOLATIONS=(
    "ANativeWindow_lock"
    "AndroidBitmap_lockPixels"
)

for file in $(git ls-files '*.cpp' '*.h' 2>/dev/null); do
    if [ ! -f "$file" ]; then continue; fi
    for pattern in "${RAII_VIOLATIONS[@]}"; do
        # 查找裸调用但排除 RAII 类定义内部的行
        matches=$(grep -n "$pattern" "$file" 2>/dev/null | grep -v "class.*Scoped\|//.*Scoped\|ScopedWindowLock\|ScopedBitmapLock" || true)
        if [ -n "$matches" ]; then
            echo "WARNING: $file 中存在裸 $pattern 调用（未在 RAII 类中封装）"
            echo "$matches"
            violations=$((violations + 1))
        fi
    done
done

if [ "$violations" -gt 0 ]; then
    echo ""
    echo "FAIL: $violations 处 RAII 违规"
    echo "修复指南: 为裸 lock/unlock 配对创建 Scoped* RAII 类"
    exit 1
fi

echo "PASS: 未发现 RAII 违规"
exit 0
