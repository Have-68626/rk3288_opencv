#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace rk_win {

std::string urlDecodePath(const std::string& url);
bool isSafeRelativePath(const std::filesystem::path& docRoot, const std::string& urlPath, std::filesystem::path& resolvedOut);

// S-SEC-B09-007：服务启动时固定 webroot 的目录句柄（不共享 DELETE），避免请求间
// 重新按可替换的根路径打开目录。读取时逐层相对该句柄打开并拒绝重解析点，根目录
// 重绑定和 symlink/junction 换出均 fail-closed。
class StaticFileRoot {
public:
    StaticFileRoot() = default;
    ~StaticFileRoot();
    StaticFileRoot(const StaticFileRoot&) = delete;
    StaticFileRoot& operator=(const StaticFileRoot&) = delete;

    bool open(const std::filesystem::path& docRoot);
    void close();
    // 校验 URL 后返回相对根目录的路径；该结果只能传给 readFileWithinRoot。
    bool resolve(const std::string& urlPath, std::filesystem::path& resolvedOut) const;

private:
    std::uintptr_t nativeHandle_ = 0;
    std::filesystem::path fallbackRoot_;

    friend bool readFileWithinRoot(const StaticFileRoot& docRoot,
                                   const std::filesystem::path& path,
                                   std::string& out);
};

// 仅接受 StaticFileRoot::resolve 返回的相对路径；非 Windows 构建 fail-closed。
bool readFileWithinRoot(const StaticFileRoot& docRoot,
                        const std::filesystem::path& path,
                        std::string& out);

} // namespace rk_win
