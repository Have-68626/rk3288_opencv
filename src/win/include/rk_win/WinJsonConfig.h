#pragma once

#include "WinConfig.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rk_win {

// Windows JSON 配置存储（落盘 + 校验 + 热重载 + 回滚）
//
// 目标路径：%APPDATA%\<ProductName>\config.json
// - 为什么放到 APPDATA：可写、按用户隔离、不需要管理员权限；
// - 坑：多实例同时写会冲突；因此写入必须“先校验再原子替换”，并保留 .bak 用于回滚。
class WinJsonConfigStore {
public:
    struct UpdateResult {
        bool ok = false;
        int httpStatus = 500;  // 用于 API 返回（400/409/500 等）
        std::string code;      // 机器可读错误码
        std::string message;   // 人类可读说明
        std::vector<std::string> details;
    };

    WinJsonConfigStore();
    ~WinJsonConfigStore();

    // 初始化：从 config.json 读取；不存在则尝试从旧 ini 迁移；失败则使用默认值并落盘。
    bool initialize(std::string& warnOut);

    // 当前生效配置（线程安全复制）
    AppConfig current() const;

    // 供 API 输出：返回脱敏后的 settings JSON（不包含密钥/敏感明文）
    std::string currentRedactedJsonPretty() const;

    // PUT settings：body 为 JSON 对象（局部更新）。成功时会落盘并更新内存。
    UpdateResult updateFromJsonBody(const std::string& bodyUtf8);

    // 敏感字段密钥轮换：生成新 AES 主密钥（DPAPI 保护）并重新加密敏感字段落盘。
    UpdateResult rotateKeyAndReencrypt();

    // 热重载：启动后台线程监控 config.json 变更；变更后自动 reload+校验；失败时回滚。
    // - outApplied=true 代表确实更新了内存配置（调用方可据此决定是否重启模块）
    bool startWatching();
    void stopWatching();
    bool pollReloadOnce(bool& outApplied, std::string& outErr);  // 便于测试/无线程环境

    std::filesystem::path configPath() const { return configPath_; }

private:
    bool reloadFromDisk(bool& outApplied, std::string& outErr);
    bool writeAtomicallyWithBackup(const std::string& jsonPretty, std::string& outErr);

    // 加解密相关：仅用于“敏感字段”落盘形态（cipher object），内存中仍为明文（便于业务使用）。
    bool ensureOrLoadKey(std::vector<std::uint8_t>& keyOut, std::string& outErr) const;

    // JSON <-> AppConfig
    bool parseAndValidateSettingsDoc(const std::string& jsonText, AppConfig& cfgOut, std::string& outErr) const;
    std::string buildSettingsJson(const AppConfig& cfg, bool redacted, bool encryptSensitive) const;

    static std::filesystem::path defaultConfigPath();
    static std::filesystem::path defaultKeyPath(const std::filesystem::path& cfgPath);
    static std::filesystem::path defaultBakPath(const std::filesystem::path& cfgPath);

    // watch state
    std::atomic<bool> watching_{false};
    std::thread watchThread_;

    std::filesystem::path configPath_;
    std::filesystem::path keyPath_;
    std::filesystem::path bakPath_;
    std::filesystem::file_time_type lastWriteTime_{};

    mutable std::mutex mu_;
    AppConfig cfg_{};
    std::string lastGoodJsonPretty_;  // 用于回滚/调试输出（不含密钥）
};

}  // namespace rk_win

