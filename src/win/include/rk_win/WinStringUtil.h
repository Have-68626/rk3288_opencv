#pragma once

#include <cstdio>
#include <string>

namespace rk_win {

// JSON 字符串转义（处理常见控制字符 + \uXXXX 编码）
// 统一实现，替代 EventLogger/StructuredLogger/RenderMetricsLogger/HttpFacesServer 中的副本
inline std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 4);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

}  // namespace rk_win
