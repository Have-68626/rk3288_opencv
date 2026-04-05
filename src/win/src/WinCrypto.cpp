#include "rk_win/WinCrypto.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#include <algorithm>

namespace rk_win {

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
    (void)bin;
    (void)outB64;
    return false;
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
    (void)b64;
    (void)outBin;
    return false;
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
    (void)plain;
    (void)protectedOut;
    errOut = "DPAPI not available";
    return false;
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
    (void)protectedIn;
    (void)plainOut;
    errOut = "DPAPI not available";
    return false;
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
    (void)n;
    (void)out;
    errOut = "BCrypt not available";
    return false;
#endif
}

bool aes256gcmEncrypt(const std::vector<std::uint8_t>& key32,
                      const std::vector<std::uint8_t>& plain,
                      AesGcmCiphertext& out,
                      std::string& errOut) {
#ifdef _WIN32
    errOut.clear();
    out = {};
    if (key32.size() != 32) {
        errOut = "invalid key length (need 32 bytes)";
        return false;
    }

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
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (st != 0) {
        errOut = "BCryptEncrypt failed";
        return false;
    }
    out.ciphertext.resize(outBytes);
    return true;
#else
    (void)key32;
    (void)plain;
    (void)out;
    errOut = "BCrypt not available";
    return false;
#endif
}

bool aes256gcmDecrypt(const std::vector<std::uint8_t>& key32,
                      const AesGcmCiphertext& in,
                      std::vector<std::uint8_t>& plainOut,
                      std::string& errOut) {
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

    plainOut.assign(in.ciphertext.size(), 0);
    ULONG outBytes = 0;
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
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (st != 0) {
        errOut = "BCryptDecrypt failed (bad key/nonce/tag or corrupted data)";
        plainOut.clear();
        return false;
    }
    plainOut.resize(outBytes);
    return true;
#else
    (void)key32;
    (void)in;
    (void)plainOut;
    errOut = "BCrypt not available";
    return false;
#endif
}

}  // namespace rk_win

