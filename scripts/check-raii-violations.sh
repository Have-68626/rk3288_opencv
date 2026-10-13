#!/bin/bash
# RAII 违规检查
# 检查所有裸 lock/unlock 调用（未在 RAII 类中封装）
# 遵守架构契约 #1
# 用法: ./scripts/check-raii-violations.sh
#
# 判定方式（块级结构化，非逐行文本匹配）：
#   以「花括号平衡」定位定义 ScopedWindowLock / ScopedBitmapLock 的类体区间，
#   落在该块内部的裸调用视为合法封装而豁免；块之外的裸调用才计违规。
#   判定前先剔除注释/字符串/字符字面量/raw 字符串，避免其内 {} 与关键字干扰结构计算。
#
# 已知边界（如实登记；本脚本是文本/正则工具，**非** C++ 语义解析器）：
#   ① 多行 **raw 字符串**若内部含不平衡的花括号（最小复现：
#        class ScopedWindowLock {
#            const char* k = R"(
#        {{
#        )";
#        };
#        ANativeWindow_lock(w, &b, nullptr);   ← 真违规，却曾被整体静默豁免
#      ）：旧实现的 strip() **逐行**处理、字符串状态不跨行 ⇒ raw 串内部行被当作代码，depth 永远
#      回不到 wrapper_depth ⇒ wrapper_active 永久为真 ⇒ 该文件此后所有真违规全部静默漏报。
#      **已修**：strip() 的 raw string 状态（in_raw / raw_delim）**跨行保持**，raw 串内部不参与
#      花括号计数（见 strip/detect_raw）。仍可能漏的同类形态见 ②。
#   ② `};` 与紧随的 `{`（或 `} void g(){`）落在**同一行**时，该行花括号净变化为 0 ⇒
#      **该行之后新开块的区间内**豁免边界失效，**该区间内的裸调用漏报**；区间结束后检查恢复正常。
#      ⚠️ 精确措辞（2026-09-26 二次审计实测更正，勿写成「整文件误免检」）：若豁免区因此**一直不闭合**，
#      ③ 的安全网会在 EOF 报 WARNING 并**计违规**（合成件 `}; {` 实测 rc=4 ⇒ 脚本判 FAIL，不是静默放行）。
#      **未修**（需更完整的作用域模型，超出本文本工具的射程）。
#   ③ ⚠️ 安全网：若因 ② 或任何未知原因，豁免区在**文件末尾仍未闭合**，本脚本会输出
#      「RAII 豁免区未闭合」告警并计入违规（退出码非 0）。这把「静默整文件免检」变成**可见信号** ——
#      即便根因未除尽，漏检也不会再静默通过。触发条件：扫描到 EOF 时 wrapper_active 仍为真。
#   ④ ⚠️ 裸调用与封装类的闭合 `};` **同行**时（合成输入：`};\nclass Foo {};\n` 之外再加
#      `}; ANativeWindow_lock(a,&b,0);`）⇒ 该行被判为**仍在豁免区内** ⇒ 真违规**漏报**；
#      且 wrapper_active 随后正常退出 ⇒ ③ 的 EOF 安全网**也不触发** ⇒ 属**静默**漏报。
#      **未修**（2026-09-26 二次审计实测复现）：判定发生在 `depth += opens - closes` **之前**，
#      要修需把判据改成「命中字符位置是否落在豁免区间内」，与 ② 同属作用域模型重写。

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

# awk 程序（结构化判定）。单引号内的字符字面量 39 用 SQ=sprintf("%c",39) 规避。
# 传入：-v pat=<裸调用模式>；-v report_eof=1 仅在**每个文件的第一个模式**调用时为 1
#       （避免同一文件的两个模式各报一次「未闭合」）。
RAII_AWK='
function nbraces(s, ch,   i, n, k) {
    k = 0; n = length(s)
    for (i = 1; i <= n; i++) if (substr(s, i, 1) == ch) k++
    return k
}
# 跳过普通字符串（s 中位于 i 的 " 起），返回结束后的位置。C++ 普通串不跨行，故无需跨行状态。
function skip_dq(s, i, n,   d) {
    i++
    while (i <= n) {
        d = substr(s, i, 1)
        if (d == "\\") { i += 2; continue }
        if (d == "\"") { i++; break }
        i++
    }
    return i
}
# 跳过字符字面量（s 中位于 i 的 SQ 起），返回结束后的位置。
function skip_sq(s, i, n,   d) {
    i++
    while (i <= n) {
        d = substr(s, i, 1)
        if (d == "\\") { i += 2; continue }
        if (d == SQ) { i++; break }
        i++
    }
    return i
}
# 若 s 的 i 处是 raw 串起始 R"delim(，则设置跨行状态 in_raw/raw_delim 并返回 "(" 之后的位置；
# 否则返回 0（不是 raw 串起始）。delim 规则：最长 16 字符，不含空白/)/反斜杠。
function detect_raw(s, i, n,   j, c, delim) {
    if (substr(s, i, 2) != "R\"") return 0
    delim = ""; j = i + 2
    while (j <= n) {
        c = substr(s, j, 1)
        if (c == "(") { raw_delim = delim; in_raw = 1; return j + 1 }
        if (c == ")" || c == "\\" || c == " " || c == "\t" || length(delim) >= 16) return 0
        delim = delim c
        j++
    }
    return 0
}
# 剔除块注释 / 行注释 / 普通串 / 字符 / raw 串，返回"纯代码"。
# in_block 与 in_raw/raw_delim 均为**全局** ⇒ 状态**跨行保持**（块注释与 raw 串可跨多行）。
function strip(s,   i, n, c, needle, k) {
    out = ""; n = length(s); i = 1
    while (i <= n) {
        c = substr(s, i, 1)
        if (in_block) {
            if (c == "*" && substr(s, i + 1, 1) == "/") { in_block = 0; i += 2; continue }
            i++; continue
        }
        if (in_raw) {
            needle = ")" raw_delim "\""
            if (substr(s, i, length(needle)) == needle) { i += length(needle); in_raw = 0; continue }
            i++; continue
        }
        if (c == "/" && substr(s, i + 1, 1) == "*") { in_block = 1; i += 2; continue }
        if (c == "/" && substr(s, i + 1, 1) == "/") break
        if (c == "\"") { i = skip_dq(s, i, n); continue }
        if (c == SQ) { i = skip_sq(s, i, n); continue }
        if (c == "R" && substr(s, i + 1, 1) == "\"") {
            k = detect_raw(s, i, n)
            if (k > 0) { i = k; continue }
        }
        out = out c
        i++
    }
    return out
}
BEGIN { SQ = sprintf("%c", 39); in_block = 0; in_raw = 0; raw_delim = ""; depth = 0; wrapper_active = 0; wrapper_depth = -1; pending = 0 }
{
    code = strip($0)
    # 本行是否开启一个 Scoped 封装类的定义
    if (code ~ /(class|struct)[ \t]+Scoped(Window|Bitmap)Lock/) pending = 1
    opens  = nbraces(code, "{")
    closes = nbraces(code, "}")
    # 进入封装类体：记录进入前的花括号深度，作为豁免区间的下界
    if (pending && opens > 0) { pending = 0; wrapper_active = 1; wrapper_depth = depth }
    # 仅有前向声明（以 ; 结束且无花括号）时取消
    if (pending && opens == 0 && code ~ /;/) pending = 0
    # 行首尚未处于封装体内部（或本行刚进入）→ 豁免；否则报告为裸调用
    if (!wrapper_active && index(code, pat) > 0) printf "%d:%s\n", NR, $0
    # 更新深度；封装体闭合（深度回落到进入前水平）后退出豁免
    depth += opens - closes
    if (wrapper_active && depth <= wrapper_depth) wrapper_active = 0
}
END {
    # 安全网（见头部 ③）：豁免区到文件末尾仍未闭合 ⇒ 该文件可能被整体误免检。
    # 以退出码 4 报警；调用侧仅在 report_eof==1（每文件的首个模式）时计一次，避免重复告警。
    if (wrapper_active && report_eof == 1) exit 4
}
'

for file in $(git ls-files '*.cpp' '*.h' 2>/dev/null); do
    if [ ! -f "$file" ]; then continue; fi
    first=1  # 每个文件的第一个模式负责「未闭合」告警，避免两个模式各报一次
    for pattern in "${RAII_VIOLATIONS[@]}"; do
        # 块级判定：仅报告落在 Scoped 封装类体之外的裸调用。首模式调用要求 EOF 未闭合时以退出码 4 报警。
        matches=$(awk -v pat="$pattern" -v report_eof="$first" "$RAII_AWK" "$file" 2>/dev/null)
        rc=$?
        first=0
        if [ "$rc" -eq 4 ]; then
            echo "WARNING: $file 的 RAII 豁免区未闭合——该文件可能被整体误免检（depth 被多行 raw 串或同行 }; { 破坏，wrapper_active 恒为真）"
            violations=$((violations + 1))
        fi
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
