#include "rk_win/JsonLite.h"

#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace rk_win {
namespace {

constexpr int kMaxParseDepth = 64;

struct Parser {
    const char* base = nullptr;
    const char* p = nullptr;
    const char* end = nullptr;
    std::string err;

    bool eof() const { return p >= end; }
    char peek() const { return eof() ? '\0' : *p; }
    char get() { return eof() ? '\0' : *p++; }

    void skipWs() {
        while (!eof() && std::isspace(static_cast<unsigned char>(*p))) p++;
    }

    std::string fail(const char* msg) {
        std::ostringstream os;
        const std::size_t off = (base && p) ? static_cast<std::size_t>(p - base) : 0;
        os << msg << " at offset " << off;
        return os.str();
    }

    std::string failAt(std::size_t offset, const char* msg) {
        std::ostringstream os;
        os << msg << " at offset " << offset;
        return os.str();
    }

    bool consume(char c) {
        skipWs();
        if (peek() != c) return false;
        p++;
        return true;
    }

    bool expect(char c, const char* msg) {
        skipWs();
        if (peek() != c) {
            err = fail(msg);
            return false;
        }
        p++;
        return true;
    }

    bool parseValue(JsonValue& out, int depth = 0) {
        if (depth > kMaxParseDepth) {
            err = fail("max parse depth exceeded");
            return false;
        }
        skipWs();
        if (eof()) {
            err = "unexpected EOF";
            return false;
        }

        const char c = peek();
        if (c == '{') return parseObject(out, depth);
        if (c == '[') return parseArray(out, depth);
        if (c == '"') return parseString(out);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        if (c == 't' || c == 'f') return parseBool(out);
        if (c == 'n') return parseNull(out);

        err = fail("unexpected character");
        return false;
    }

    bool parseNull(JsonValue& out) {
        skipWs();
        const std::string_view lit(p, static_cast<std::size_t>(end - p));
        if (lit.rfind("null", 0) == 0) {
            p += 4;
            out = JsonValue::makeNull();
            return true;
        }
        err = fail("expected null");
        return false;
    }

    bool parseBool(JsonValue& out) {
        skipWs();
        const std::string_view lit(p, static_cast<std::size_t>(end - p));
        if (lit.rfind("true", 0) == 0) {
            p += 4;
            out = JsonValue::makeBool(true);
            return true;
        }
        if (lit.rfind("false", 0) == 0) {
            p += 5;
            out = JsonValue::makeBool(false);
            return true;
        }
        err = fail("expected boolean");
        return false;
    }

    bool parseNumber(JsonValue& out) {
        skipWs();
        const char* start = p;

        // JSON number: -?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?
        if (peek() == '-') p++;
        if (peek() == '0') {
            p++;
        } else {
            if (!(peek() >= '1' && peek() <= '9')) {
                err = fail("invalid number");
                return false;
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) p++;
        }
        if (peek() == '.') {
            p++;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                err = fail("invalid fractional part");
                return false;
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) p++;
        }
        if (peek() == 'e' || peek() == 'E') {
            p++;
            if (peek() == '+' || peek() == '-') p++;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) {
                err = fail("invalid exponent");
                return false;
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) p++;
        }

        const std::string_view token(start, static_cast<std::size_t>(p - start));
        char* endp = nullptr;
        const double v = std::strtod(std::string(token).c_str(), &endp);
        if (!std::isfinite(v)) {
            err = fail("number out of range");
            return false;
        }
        out = JsonValue::makeNumber(v);
        return true;
    }

    static bool appendUtf8FromCodepoint(std::string& out, std::uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
            return true;
        }
        if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return true;
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return true;
        }
        if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return true;
        }
        return false;
    }

    static int hexNibble(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    }

    bool parseString(JsonValue& out) {
        skipWs();
        if (!expect('"', "expected string")) return false;
        std::string s;
        while (!eof()) {
            const char c = get();
            if (c == '"') {
                out = JsonValue::makeString(std::move(s));
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                err = fail("control character in string");
                return false;
            }
            if (c != '\\') {
                s.push_back(c);
                continue;
            }

            if (eof()) {
                err = "unexpected EOF in string escape";
                return false;
            }
            const char e = get();
            switch (e) {
                case '"': s.push_back('"'); break;
                case '\\': s.push_back('\\'); break;
                case '/': s.push_back('/'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'u': {
                    // \uXXXX（仅实现 BMP；若要完整支持代理对，需要额外逻辑。当前配置不依赖此特性）
                    if (end - p < 4) {
                        err = "incomplete \\u escape";
                        return false;
                    }
                    std::uint32_t cp = 0;
                    for (int i = 0; i < 4; i++) {
                        const int hn = hexNibble(p[i]);
                        if (hn < 0) {
                            err = fail("invalid hex in \\u escape");
                            return false;
                        }
                        cp = (cp << 4) | static_cast<std::uint32_t>(hn);
                    }
                    p += 4;
                    if (!appendUtf8FromCodepoint(s, cp)) {
                        err = fail("invalid unicode codepoint");
                        return false;
                    }
                    break;
                }
                default:
                    err = fail("invalid escape");
                    return false;
            }
        }
        err = "unexpected EOF in string";
        return false;
    }

    bool parseArray(JsonValue& out, int depth) {
        skipWs();
        if (!expect('[', "expected array")) return false;
        JsonValue arr = JsonValue::makeArray();
        skipWs();
        if (consume(']')) {
            out = std::move(arr);
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parseValue(elem, depth + 1)) return false;
            arr.a.push_back(std::move(elem));
            skipWs();
            if (consume(']')) break;
            if (!expect(',', "expected ',' or ']'")) return false;
        }
        out = std::move(arr);
        return true;
    }

    bool parseObject(JsonValue& out, int depth) {
        skipWs();
        if (!expect('{', "expected object")) return false;
        JsonValue obj = JsonValue::makeObject();
        skipWs();
        if (consume('}')) {
            out = std::move(obj);
            return true;
        }
        while (true) {
            JsonValue key;
            if (!parseString(key)) return false;
            if (!expect(':', "expected ':'")) return false;
            JsonValue val;
            if (!parseValue(val, depth + 1)) return false;
            obj.o[std::move(key.s)] = std::move(val);
            skipWs();
            if (consume('}')) break;
            if (!expect(',', "expected ',' or '}'")) return false;
        }
        out = std::move(obj);
        return true;
    }
};

static void writeEscapedString(std::string& os, const std::string& s) {
    os += '"';
    for (unsigned char uc : s) {
        const char c = static_cast<char>(uc);
        switch (c) {
            case '"': os += "\\\""; break;
            case '\\': os += "\\\\"; break;
            case '\b': os += "\\b"; break;
            case '\f': os += "\\f"; break;
            case '\n': os += "\\n"; break;
            case '\r': os += "\\r"; break;
            case '\t': os += "\\t"; break;
            default:
                if (uc < 0x20) {
                    os += "\\u00";
                    static const char* hex = "0123456789ABCDEF";
                    os += hex[(uc >> 4) & 0xF];
                    os += hex[uc & 0xF];
                } else {
                    os += c;
                }
                break;
        }
    }
    os += '"';
}

static void dump(const JsonValue& v, std::string& os, bool pretty, int indentSize, int indent) {
    auto nl = [&]() {
        if (!pretty) return;
        os += '\n';
        os.append(indent, ' ');
    };
    auto nlChild = [&]() {
        if (!pretty) return;
        os += '\n';
        os.append(indent + indentSize, ' ');
    };

    switch (v.type) {
        case JsonValue::Type::Null: os += "null"; return;
        case JsonValue::Type::Bool: os += (v.b ? "true" : "false"); return;
        case JsonValue::Type::Number: {
            // 输出稳定：整数按整数输出；小数用 fixed + 去尾零，避免科学计数法导致 diff 噪音。
            if (std::fabs(v.n - std::round(v.n)) < 1e-9 && std::fabs(v.n) < 9.22e18) {
                os += std::to_string(static_cast<long long>(std::llround(v.n)));
                return;
            }
            char buf[512];
            int len = std::snprintf(buf, sizeof(buf), "%.6f", v.n);
            if (len > 0 && len < static_cast<int>(sizeof(buf))) {
                std::string s(buf, static_cast<std::size_t>(len));
                // 去尾零与多余小数点
                while (s.size() > 1 && s.find('.') != std::string::npos && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
                os += s;
            }
            return;
        }
        case JsonValue::Type::String:
            writeEscapedString(os, v.s);
            return;
        case JsonValue::Type::Array: {
            os += '[';
            if (v.a.empty()) {
                os += ']';
                return;
            }
            for (std::size_t i = 0; i < v.a.size(); i++) {
                if (i == 0) {
                    nlChild();
                } else {
                    os += ',';
                    nlChild();
                }
                dump(v.a[i], os, pretty, indentSize, indent + indentSize);
            }
            nl();
            os += ']';
            return;
        }
        case JsonValue::Type::Object: {
            os += '{';
            if (v.o.empty()) {
                os += '}';
                return;
            }
            std::size_t i = 0;
            for (const auto& kv : v.o) {
                if (i == 0) {
                    nlChild();
                } else {
                    os += ',';
                    nlChild();
                }
                writeEscapedString(os, kv.first);
                os += (pretty ? ": " : ":");
                dump(kv.second, os, pretty, indentSize, indent + indentSize);
                i++;
            }
            nl();
            os += '}';
            return;
        }
    }
}

}  // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
    if (!isObject()) return nullptr;
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

JsonValue* JsonValue::find(const std::string& key) {
    if (!isObject()) return nullptr;
    auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

bool parseJson(std::string_view text, JsonValue& out, std::string& errOut) {
    constexpr std::size_t kMaxJsonInputBytes = 10 * 1024 * 1024;
    if (text.size() > kMaxJsonInputBytes) {
        errOut = "JSON input too large (" + std::to_string(text.size()) + " bytes, max " + std::to_string(kMaxJsonInputBytes) + ")";
        return false;
    }
    Parser ps;
    ps.base = text.data();
    ps.p = text.data();
    ps.end = text.data() + text.size();
    JsonValue v;
    if (!ps.parseValue(v)) {
        errOut = ps.err.empty() ? "parse error" : ps.err;
        return false;
    }
    ps.skipWs();
    if (!ps.eof()) {
        errOut = "trailing characters after JSON";
        return false;
    }
    out = std::move(v);
    errOut.clear();
    return true;
}

std::string toJsonString(const JsonValue& v, bool pretty, int indentSize) {
    std::string os;
    os.reserve(4096);
    dump(v, os, pretty, indentSize, 0);
    if (pretty) os += "\n";
    return os;
}

void deepMergeObject(JsonValue& baseObj, const JsonValue& patchObj) {
    if (!baseObj.isObject() || !patchObj.isObject()) return;
    for (const auto& kv : patchObj.o) {
        const auto& k = kv.first;
        const auto& pv = kv.second;
        JsonValue* bv = baseObj.find(k);
        if (bv && bv->isObject() && pv.isObject()) {
            deepMergeObject(*bv, pv);
        } else {
            baseObj.o[k] = pv;
        }
    }
}

}  // namespace rk_win

