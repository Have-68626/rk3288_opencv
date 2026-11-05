#include "rk_win/WinCrypto.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef BCRYPT_AUTHENTICATED_CIPHER_MODE_CHAIN_CALL_FLAG
#define BCRYPT_AUTHENTICATED_CIPHER_MODE_CHAIN_CALL_FLAG 0x00000001
#endif
#endif

#include <algorithm>

#ifndef _WIN32
// Linux/Android：OpenSSL libcrypto 实现（决策 D2）。不得据此宣称任何硬件加速。
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace rk_win {

#if defined(RK_ENABLE_TEST_SEAMS)
namespace {
// 仅测试接缝：固定 nonce 让 Windows(BCrypt) 与 Linux(OpenSSL) 两套实现对同一向量比对。
// 生产构建不编译本段（RK_ENABLE_TEST_SEAMS 仅由测试目标定义），nonce 始终随机。
std::vector<std::uint8_t> g_testNonce;
}  // namespace
#endif

bool base64Encode(const std::vector<std::uint8_t>& bin, std::string& outB64) {
#ifdef _WIN32
    if (bin.empty()) {
        outB64.clear();
        return true;
    }
    DWORD outChars = 0;
    if (!CryptBinaryToStringA(bin.data(), static_cast<DWORD>(bin.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outChars)) {
        return false;
    }
    std::string s(outChars, '\0');
    if (!CryptBinaryToStringA(bin.data(), static_cast<DWORD>(bin.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &outChars)) {
        return false;
    }
    while (!s.empty() && (s.back() == '\0' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    outB64 = std::move(s);
    return true;
#else
    // 标准 Base64、无换行（与 Windows 侧 CRYPT_STRING_BASE64|NOCRLF 等价）。
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    outB64.clear();
    outB64.reserve((bin.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= bin.size()) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bin[i]) << 16) |
                                (static_cast<std::uint32_t>(bin[i + 1]) << 8) |
                                static_cast<std::uint32_t>(bin[i + 2]);
        outB64.push_back(kTable[(v >> 18) & 0x3F]);
        outB64.push_back(kTable[(v >> 12) & 0x3F]);
        outB64.push_back(kTable[(v >> 6) & 0x3F]);
        outB64.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    const std::size_t rest = bin.size() - i;
    if (rest == 1) {
        const std::uint32_t v = static_cast<std::uint32_t>(bin[i]) << 16;
        outB64.push_back(kTable[(v >> 18) & 0x3F]);
        outB64.push_back(kTable[(v >> 12) & 0x3F]);
        outB64 += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (static_cast<std::uint32_t>(bin[i]) << 16) |
                                (static_cast<std::uint32_t>(bin[i + 1]) << 8);
        outB64.push_back(kTable[(v >> 18) & 0x3F]);
        outB64.push_back(kTable[(v >> 12) & 0x3F]);
        outB64.push_back(kTable[(v >> 6) & 0x3F]);
        outB64.push_back('=');
    }
    return true;
#endif
}

bool base64Decode(std::string_view b64, std::vector<std::uint8_t>& outBin) {
#ifdef _WIN32
    outBin.clear();
    if (b64.empty()) return true;
    DWORD outBytes = 0;
    if (!CryptStringToBinaryA(std::string(b64).c_str(), 0, CRYPT_STRING_BASE64, nullptr, &outBytes, nullptr, nullptr)) {
        return false;
    }
    std::vector<std::uint8_t> buf(outBytes);
    if (!CryptStringToBinaryA(std::string(b64).c_str(), 0, CRYPT_STRING_BASE64, buf.data(), &outBytes, nullptr, nullptr)) {
        return false;
    }
    buf.resize(outBytes);
    outBin = std::move(buf);
    return true;
#else
    // 容忍空白字符；非法字符或长度不合法返回 false。
    outBin.clear();
    std::int32_t acc = 0;
    int bits = 0;
    for (const char ch : b64) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (ch == '=') break;
        std::int32_t v = -1;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        if (v < 0) return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            outBin.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return true;
#endif
}

bool dpapiProtect(const std::vector<std::uint8_t>& plain, std::vector<std::uint8_t>& protectedOut, std::string& errOut) {
#ifdef _WIN32
    errOut.clear();
    protectedOut.clear();
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"rk_win_config_key", nullptr, nullptr, nullptr, 0, &out)) {
        errOut = "CryptProtectData failed";
        return false;
    }
    protectedOut.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    // 决策 D1：Linux 无 DPAPI 等价物，密钥以 0600 权限文件（base64）保存，
    // 由调用方在写盘后强制权限；本函数只做恒等变换。
    // 威胁差异：同机同用户的其他进程可读取密钥文件；Windows 的 DPAPI 由系统密钥库托管。
    (void)errOut;
    protectedOut = plain;
    errOut.clear();
    return true;
#endif
}

bool dpapiUnprotect(const std::vector<std::uint8_t>& protectedIn, std::vector<std::uint8_t>& plainOut, std::string& errOut) {
#ifdef _WIN32
    errOut.clear();
    plainOut.clear();
    DATA_BLOB in{};
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(protectedIn.data()));
    in.cbData = static_cast<DWORD>(protectedIn.size());
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        errOut = "CryptUnprotectData failed";
        return false;
    }
    plainOut.assign(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return true;
#else
    // 与 dpapiProtect 对称：文件权限由调用方强制（0600）。
    (void)errOut;
    plainOut = protectedIn;
    errOut.clear();
    return true;
#endif
}

bool randomBytes(std::size_t n, std::vector<std::uint8_t>& out, std::string& errOut) {
#ifdef _WIN32
    errOut.clear();
    out.assign(n, 0);
    if (n == 0) return true;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(out.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        errOut = "BCryptGenRandom failed";
        return false;
    }
    return true;
#else
    errOut.clear();
    out.assign(n, 0);
    if (n == 0) return true;
    if (RAND_bytes(reinterpret_cast<unsigned char*>(out.data()), static_cast<int>(out.size())) != 1) {
        errOut = "RAND_bytes failed";
        return false;
    }
    return true;
#endif
}

bool aes256gcmEncrypt(const std::vector<std::uint8_t>& key32,
                      const std::vector<std::uint8_t>& plain,
                      AesGcmCiphertext& out,
                      std::string& errOut,
                      std::string_view aad) {
#ifdef _WIN32
    errOut.clear();
    out = {};
    if (key32.size() != 32) {
        errOut = "invalid key length (need 32 bytes)";
        return false;
    }

#if defined(RK_ENABLE_TEST_SEAMS)
    if (g_testNonce.size() == 12) {
        out.nonce = g_testNonce;
    } else
#endif
    if (!randomBytes(12, out.nonce, errOut)) return false;
    out.tag.assign(16, 0);
    out.ciphertext.assign(plain.size(), 0);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<std::uint8_t> keyObj;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (st != 0) {
        errOut = "BCryptOpenAlgorithmProvider(AES) failed";
        return false;
    }

    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                           reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                           static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
    if (st != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptSetProperty(GCM) failed";
        return false;
    }

    DWORD objLen = 0, cb = 0;
    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
    if (st != 0 || objLen == 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptGetProperty(OBJECT_LENGTH) failed";
        return false;
    }
    keyObj.assign(objLen, 0);

    st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), objLen,
                                    const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key32.data())),
                                    static_cast<ULONG>(key32.size()), 0);
    if (st != 0) {
        if (!keyObj.empty()) SecureZeroMemory(keyObj.data(), keyObj.size());
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptGenerateSymmetricKey failed";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = out.nonce.data();
    info.cbNonce = static_cast<ULONG>(out.nonce.size());
    info.pbTag = out.tag.data();
    info.cbTag = static_cast<ULONG>(out.tag.size());
    // AAD 必须通过 pbAuthData/cbAuthData 绑定才能计入 tag。旧实现用“链式调用 + 入参=AAD”
    // 并不会认证 AAD（2026-11-05 向量回归实测：带/不带 AAD 的 tag 相同）；旧密文由存储层
    // 按空 AAD 回退解密，并在下次保存时按新语义重写。
    info.pbAuthData = aad.empty() ? nullptr
                                  : const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());

    ULONG outBytes = 0;

    st = BCryptEncrypt(hKey,
                       const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(plain.data())),
                       static_cast<ULONG>(plain.size()),
                       &info,
                       nullptr,
                       0,
                       reinterpret_cast<PUCHAR>(out.ciphertext.data()),
                       static_cast<ULONG>(out.ciphertext.size()),
                       &outBytes,
                       0);

    BCryptDestroyKey(hKey);
    if (!keyObj.empty()) {
        SecureZeroMemory(keyObj.data(), keyObj.size());
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (st != 0) {
        errOut = "BCryptEncrypt failed";
        return false;
    }
    out.ciphertext.resize(outBytes);
    return true;
#else
    errOut.clear();
    out = AesGcmCiphertext{};
    if (key32.size() != 32) {
        errOut = "invalid key length (need 32 bytes)";
        return false;
    }
#if defined(RK_ENABLE_TEST_SEAMS)
    if (g_testNonce.size() == 12) {
        out.nonce = g_testNonce;
    } else
#endif
    {
        out.nonce.resize(12);
        if (RAND_bytes(out.nonce.data(), static_cast<int>(out.nonce.size())) != 1) {
            errOut = "RAND_bytes(nonce) failed";
            return false;
        }
    }
    out.ciphertext.assign(plain.size(), 0);
    out.tag.assign(16, 0);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        errOut = "EVP_CIPHER_CTX_new failed";
        return false;
    }
    int len = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(out.nonce.size()), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr, key32.data(), out.nonce.data()) == 1;
    // AAD 先于明文；与 Windows 实现使用相同 AAD 字节即可保持密文互通（决策 D3）。
    if (ok && !aad.empty()) {
        ok = EVP_EncryptUpdate(ctx, nullptr, &len,
                               reinterpret_cast<const unsigned char*>(aad.data()),
                               static_cast<int>(aad.size())) == 1;
    }
    if (ok && !plain.empty()) {
        ok = EVP_EncryptUpdate(ctx, out.ciphertext.data(), &len, plain.data(),
                               static_cast<int>(plain.size())) == 1;
        out.ciphertext.resize(static_cast<std::size_t>(len));
    }
    if (ok) {
        int finalLen = 0;
        ok = EVP_EncryptFinal_ex(ctx, out.ciphertext.data() + out.ciphertext.size(), &finalLen) == 1;
        out.ciphertext.resize(out.ciphertext.size() + static_cast<std::size_t>(finalLen));
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                                 static_cast<int>(out.tag.size()), out.tag.data()) == 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        errOut = "OpenSSL EVP aes-256-gcm encrypt failed";
        out = AesGcmCiphertext{};
        return false;
    }
    return true;
#endif
}

bool aes256gcmDecrypt(const std::vector<std::uint8_t>& key32,
                      const AesGcmCiphertext& in,
                      std::vector<std::uint8_t>& plainOut,
                      std::string& errOut,
                      std::string_view aad) {
#ifdef _WIN32
    errOut.clear();
    plainOut.clear();
    if (key32.size() != 32) {
        errOut = "invalid key length (need 32 bytes)";
        return false;
    }
    if (in.nonce.empty() || in.tag.empty()) {
        errOut = "missing nonce/tag";
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    std::vector<std::uint8_t> keyObj;

    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0);
    if (st != 0) {
        errOut = "BCryptOpenAlgorithmProvider(AES) failed";
        return false;
    }
    st = BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                           reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                           static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0);
    if (st != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptSetProperty(GCM) failed";
        return false;
    }
    DWORD objLen = 0, cb = 0;
    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
    if (st != 0 || objLen == 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptGetProperty(OBJECT_LENGTH) failed";
        return false;
    }
    keyObj.assign(objLen, 0);

    st = BCryptGenerateSymmetricKey(hAlg, &hKey, keyObj.data(), objLen,
                                    const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(key32.data())),
                                    static_cast<ULONG>(key32.size()), 0);
    if (st != 0) {
        if (!keyObj.empty()) SecureZeroMemory(keyObj.data(), keyObj.size());
        BCryptCloseAlgorithmProvider(hAlg, 0);
        errOut = "BCryptGenerateSymmetricKey failed";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(in.nonce.data()));
    info.cbNonce = static_cast<ULONG>(in.nonce.size());
    info.pbTag = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(in.tag.data()));
    info.cbTag = static_cast<ULONG>(in.tag.size());
    // 与加密侧一致：AAD 通过 pbAuthData 认证；旧密文由存储层以空 AAD 回退解密。
    info.pbAuthData = aad.empty() ? nullptr
                                  : const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(aad.data()));
    info.cbAuthData = static_cast<ULONG>(aad.size());

    ULONG outBytes = 0;

    plainOut.assign(in.ciphertext.size(), 0);
    outBytes = 0;
    st = BCryptDecrypt(hKey,
                       const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(in.ciphertext.data())),
                       static_cast<ULONG>(in.ciphertext.size()),
                       &info,
                       nullptr,
                       0,
                       reinterpret_cast<PUCHAR>(plainOut.data()),
                       static_cast<ULONG>(plainOut.size()),
                       &outBytes,
                       0);

    BCryptDestroyKey(hKey);
    if (!keyObj.empty()) {
        SecureZeroMemory(keyObj.data(), keyObj.size());
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (st != 0) {
        errOut = "BCryptDecrypt failed (bad key/nonce/tag or corrupted data)";
        plainOut.clear();
        return false;
    }
    plainOut.resize(outBytes);
    return true;
#else
    errOut.clear();
    plainOut.clear();
    if (key32.size() != 32) {
        errOut = "invalid key length (need 32 bytes)";
        return false;
    }
    if (in.nonce.size() != 12 || in.tag.size() != 16) {
        errOut = "invalid nonce/tag size";
        return false;
    }
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        errOut = "EVP_CIPHER_CTX_new failed";
        return false;
    }
    int len = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                                  static_cast<int>(in.nonce.size()), nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr, key32.data(), in.nonce.data()) == 1;
    if (ok && !aad.empty()) {
        ok = EVP_DecryptUpdate(ctx, nullptr, &len,
                               reinterpret_cast<const unsigned char*>(aad.data()),
                               static_cast<int>(aad.size())) == 1;
    }
    plainOut.assign(in.ciphertext.size(), 0);
    if (ok && !in.ciphertext.empty()) {
        ok = EVP_DecryptUpdate(ctx, plainOut.data(), &len, in.ciphertext.data(),
                               static_cast<int>(in.ciphertext.size())) == 1;
        plainOut.resize(static_cast<std::size_t>(len));
    }
    if (ok) {
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                                 static_cast<int>(in.tag.size()),
                                 const_cast<std::uint8_t*>(in.tag.data())) == 1;
    }
    if (ok) {
        int finalLen = 0;
        ok = EVP_DecryptFinal_ex(ctx, plainOut.data() + plainOut.size(), &finalLen) == 1;
        plainOut.resize(plainOut.size() + static_cast<std::size_t>(finalLen));
    }
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        errOut = "OpenSSL EVP aes-256-gcm decrypt failed (bad key/nonce/tag or corrupted data)";
        plainOut.clear();
        return false;
    }
    return true;
#endif
}

#if defined(RK_ENABLE_TEST_SEAMS)
bool aes256gcmSetNonceForTesting(const std::vector<std::uint8_t>& nonce) {
    if (nonce.size() != 12) return false;
    g_testNonce = nonce;
    return true;
}

void aes256gcmClearNonceForTesting() {
    g_testNonce.clear();
}
#endif

}  // namespace rk_win

