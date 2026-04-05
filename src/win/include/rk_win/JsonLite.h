#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rk_win {

// 轻量 JSON 值类型（仅依赖 STL）：
// - 目的：在 Windows 侧实现 settings API / 本地配置时，避免引入庞大的第三方 JSON 依赖；
// - 取舍：只覆盖本项目所需能力（对象/数组/字符串/数字/布尔/null），不追求极限性能；
// - 坑：JSON 解析容错会导致“静默接受脏数据”，因此 parse/validate 必须返回明确错误信息。
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array };

    Type type = Type::Null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::map<std::string, JsonValue> o;
    std::vector<JsonValue> a;

    static JsonValue makeNull() { return JsonValue{}; }
    static JsonValue makeBool(bool v) {
        JsonValue j;
        j.type = Type::Bool;
        j.b = v;
        return j;
    }
    static JsonValue makeNumber(double v) {
        JsonValue j;
        j.type = Type::Number;
        j.n = v;
        return j;
    }
    static JsonValue makeString(std::string v) {
        JsonValue j;
        j.type = Type::String;
        j.s = std::move(v);
        return j;
    }
    static JsonValue makeObject() {
        JsonValue j;
        j.type = Type::Object;
        return j;
    }
    static JsonValue makeArray() {
        JsonValue j;
        j.type = Type::Array;
        return j;
    }

    bool isNull() const { return type == Type::Null; }
    bool isBool() const { return type == Type::Bool; }
    bool isNumber() const { return type == Type::Number; }
    bool isString() const { return type == Type::String; }
    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }

    // 便捷访问：仅在确认类型正确时使用；否则请走 validate/json schema 报错路径。
    const JsonValue* find(const std::string& key) const;
    JsonValue* find(const std::string& key);
};

// 解析 JSON 文本为 JsonValue。
// - 失败时返回 false，并在 errOut 给出“最短可定位”的错误信息（含偏移）。
bool parseJson(std::string_view text, JsonValue& out, std::string& errOut);

// 序列化 JsonValue 为 JSON 文本。
// - pretty=true 时输出带缩进的稳定格式（用于落盘配置，便于 diff/审计）。
std::string toJsonString(const JsonValue& v, bool pretty = false, int indentSize = 2);

// 对象深度合并（用于 settings PUT 的局部更新）：
// - 若 patch 与 base 在同一 key 下均为 object：递归合并；
// - 否则：直接用 patch 覆盖 base；
// - 备注：这是“深度覆盖”而非 JSON Patch/JSON Merge Patch 的完整实现，但满足本项目 settings 诉求。
void deepMergeObject(JsonValue& baseObj, const JsonValue& patchObj);

}  // namespace rk_win

