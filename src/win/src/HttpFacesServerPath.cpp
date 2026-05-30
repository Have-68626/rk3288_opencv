#include "rk_win/HttpFacesServerPath.h"
#include <cctype>
#include <algorithm>
#include <vector>

namespace rk_win {

namespace {

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool isPathWithin(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    auto baseIt = base.begin();
    auto candIt = candidate.begin();
    for (; baseIt != base.end() && candIt != candidate.end(); ++baseIt, ++candIt) {
        if (*baseIt != *candIt) return false;
    }
    return baseIt == base.end();
}

static bool isSafeRelativeSegments(const std::filesystem::path& relPath) {
    if (relPath.empty()) return false;
    if (relPath.has_root_name() || relPath.has_root_directory()) return false;
    for (const auto& segment : relPath) {
        const std::string value = segment.string();
        if (value.empty() || value == ".") continue;
        if (value == "..") return false;
    }
    return true;
}

static bool resolveWithExistingAncestor(
    const std::filesystem::path& baseCanon,
    const std::filesystem::path& targetLex,
    std::filesystem::path& resolvedOut) {
    std::error_code ec;
    std::filesystem::path existing = targetLex;
    std::vector<std::filesystem::path> missingSuffix;

    while (!std::filesystem::exists(existing, ec)) {
        if (ec) return false;
        const std::filesystem::path parent = existing.parent_path();
        if (parent.empty() || parent == existing) return false;
        missingSuffix.push_back(existing.filename());
        existing = parent;
    }

    std::filesystem::path existingCanon = std::filesystem::weakly_canonical(existing, ec);
    if (ec) return false;
    if (!isPathWithin(baseCanon, existingCanon)) return false;

    std::filesystem::path rebuilt = existingCanon;
    for (auto it = missingSuffix.rbegin(); it != missingSuffix.rend(); ++it) {
        rebuilt /= *it;
    }
    resolvedOut = rebuilt.lexically_normal();
    return true;
}

}  // namespace

std::string urlDecodePath(const std::string& url) {
    std::string out;
    out.reserve(url.size());
    for (std::size_t i = 0; i < url.size(); ++i) {
        if (url[i] == '%') {
            if (i + 2 < url.size()) {
                int h1 = hexValue(url[i + 1]);
                int h2 = hexValue(url[i + 2]);
                if (h1 >= 0 && h2 >= 0) {
                    out += static_cast<char>((h1 << 4) | h2);
                    i += 2;
                } else {
                    // Invalid hex encoding, fail validation.
                    return "";
                }
            } else {
                // Incomplete percent-encoding sequence at the end of the string.
                return "";
            }
        } else {
            out += url[i];
        }
    }
    return out;
}

bool isSafeRelativePath(const std::filesystem::path& docRoot, const std::string& urlPath, std::filesystem::path& resolvedOut) {
    std::error_code ec;
    // Base dir must exist and be accessible
    if (!std::filesystem::exists(docRoot, ec) || ec) return false;

    std::filesystem::path baseCanon = std::filesystem::weakly_canonical(docRoot, ec);
    if (ec) return false;

    // Decode URL path
    std::string decoded = urlDecodePath(urlPath);
    if (decoded.empty() || decoded[0] != '/') return false;

    // Reject obvious bad sequences before filesystem operations
    if (decoded.find(':') != std::string::npos) return false;
    if (decoded.find('\\') != std::string::npos) return false;
    if (decoded.find("//") != std::string::npos) return false;

    std::string rel = decoded.substr(1);
    std::filesystem::path rawRelPath(rel);
    if (!isSafeRelativeSegments(rawRelPath)) return false;

    std::filesystem::path relPath = rawRelPath.lexically_normal();
    if (relPath.empty()) return false;

    std::filesystem::path targetLex = (baseCanon / relPath).lexically_normal();
    return resolveWithExistingAncestor(baseCanon, targetLex, resolvedOut);
}

} // namespace rk_win
