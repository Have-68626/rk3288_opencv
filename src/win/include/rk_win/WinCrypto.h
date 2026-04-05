#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rk_win {

// Base64（使用 Windows Crypt32 实现）
bool base64Encode(const std::vector<std::uint8_t>& bin, std::string& outB64);
bool base64Decode(std::string_view b64, std::vector<std::uint8_t>& outBin);

// DPAPI：用于保护 AES 主密钥（避免密钥明文落盘）
// - 选择 DPAPI 的原因：无需自带证书/密钥管理，默认绑定到当前用户或本机；
// - 坑：同一配置文件复制到另一台机器/用户会无法解密，需要在产品层面明确迁移策略。
bool dpapiProtect(const std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& protectedOut, std::string& errOut);
bool dpapiUnprotect(const std::vector<std::uint8_t>& protectedIn, std::vector<std::uint8_t>& plainOut, std::string& errOut);

// AES-256-GCM（CNG/BCrypt）
struct AesGcmCiphertext {
    std::vector<std::uint8_t> nonce;       // 建议 12 bytes
    std::vector<std::uint8_t> ciphertext;  // 与明文等长
    std::vector<std::uint8_t> tag;         // 16 bytes
};

bool aes256gcmEncrypt(const std::vector<std::uint8_t>& key32,
                      const std::vector<std::uint8_t>& plain,
                      AesGcmCiphertext& out,
                      std::string& errOut);

bool aes256gcmDecrypt(const std::vector<std::uint8_t>& key32,
                      const AesGcmCiphertext& in,
                      std::vector<std::uint8_t>& plainOut,
                      std::string& errOut);

// 安全随机数
bool randomBytes(std::size_t n, std::vector<std::uint8_t>& out, std::string& errOut);

}  // namespace rk_win

