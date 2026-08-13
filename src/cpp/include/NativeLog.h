#pragma once

#include <string>

namespace rklog {
void setLogDirs(const std::string& internalDir, const std::string& externalDir, const std::string& filename = "rk3288.log");
void logDebug(const char* module, const char* function, const std::string& msg);
void logInfo(const char* module, const char* function, const std::string& msg);
void logWarn(const char* module, const char* function, const std::string& msg);
void logError(const char* module, const char* function, const std::string& msg);

// 通用日志脱敏（S-SEC-B07-001）：将消息中敏感 KV（password=/token=/api_key= 等）的值
// 掩码为 ***。纯函数，可独立单测（core_unit_tests 的可测 seam）。若消息不含任何敏感键
// 则原样返回（零分配、无正则开销）。
std::string maskSensitiveData(const std::string& msg);
}

#define RKLOG_ENTER(MODULE) rklog::logDebug((MODULE), __func__, "ENTER")
