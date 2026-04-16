#include "rk_win/HttpFacesServerPath.h"
#include <cctype>
#include <algorithm>

namespace rk_win {

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

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
                // Threat Model: Malicious actors might append incomplete percent-encoding (e.g., '%')
                // to paths in an attempt to bypass WAFs or canonicalization logic that expects valid sequences.
                // Impact: Could lead to subtle path validation bypasses or interpretation discrepancies.
                // Fix: Strict decoding. Reject any incomplete sequences at the string boundary.
                // Rollback: Revert to treating trailing '%' as a literal by appending it to `out`.
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
    if (decoded.find("..") != std::string::npos) return false;
    if (decoded.find('\\') != std::string::npos) return false;
    if (decoded.find("//") != std::string::npos) return false;

    std::string rel = decoded.substr(1);
    std::filesystem::path target = docRoot / std::filesystem::path(rel).make_preferred();

    std::filesystem::path targetCanon = std::filesystem::weakly_canonical(target, ec);
    if (ec) return false;

    auto it = std::mismatch(baseCanon.begin(), baseCanon.end(), targetCanon.begin(), targetCanon.end());
    if (it.first != baseCanon.end()) return false;

    resolvedOut = std::move(target);
    return true;
}

} // namespace rk_win
