#pragma once
#include <string>
#include "JsonLite.h"

namespace rk_win {

// JSON Schema 校验器 — 提取自 WinJsonConfig
//
// 实现 JSON Schema 子集（type/required/properties/additionalProperties/minimum/maximum）
// 用于 settings doc 和加密字段的校验。
class JsonSchemaValidator {
public:
    // 校验 doc 是否符合 schema
    // - 返回 false + errorOut 包含错误信息（多条错误以 "; " 连接）
    static bool validate(const JsonValue& schema, const JsonValue& doc, std::string* errorOut = nullptr);

    // 构建 settingsDoc 的标准 schema
    static JsonValue buildSettingsDocSchema();

    // 构建加密字符串对象的 schema（用于 poster.postUrl 等敏感字段）
    static JsonValue buildEncryptedStringSchema();
};

} // namespace rk_win
