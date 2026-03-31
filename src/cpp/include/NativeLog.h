#pragma once

#include <string>

namespace rklog {
void setLogDirs(const std::string& internalDir, const std::string& externalDir, const std::string& filename = "rk3288.log");
void logDebug(const char* module, const char* function, const std::string& msg);
void logInfo(const char* module, const char* function, const std::string& msg);
void logWarn(const char* module, const char* function, const std::string& msg);
void logError(const char* module, const char* function, const std::string& msg);
}

#define RKLOG_ENTER(MODULE) rklog::logDebug((MODULE), __func__, "ENTER")
