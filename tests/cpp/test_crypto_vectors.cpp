#include <gtest/gtest.h>

#include "rk_win/WinCrypto.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> range(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i);
    return v;
}

std::string hex(const std::vector<std::uint8_t>& v) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (const auto b : v) {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 0x0F]);
    }
    return s;
}

// 期望值由独立的 Python cryptography（AESGCM）实现生成：
//   key=00..1f(32B) nonce=00..0b(12B) plain=00..0f(16B)
struct Vector {
    std::string aad;
    std::string ciphertext;
    std::string tag;
};

const Vector kVectors[] = {
    {"", "4703d418c1e0c41c85489d80bde47662", "ed395508276ff660850d12d3e755eba5"},
    {std::string("\x00\x01\x02\x03\x04\x05\x06\x07", 8),
     "4703d418c1e0c41c85489d80bde47662", "6c041535fecc181118957279df4dff1d"},
    {"poster.postUrl", "4703d418c1e0c41c85489d80bde47662",
     "e65e73506b9b5a199f98e42bf36c247e"},
};

}  // namespace

// Windows(BCrypt) 与 Linux(OpenSSL) 必须对同一 (key, nonce, plain, aad) 产出相同密文与 tag；
// 该用例在两侧 CI 都会运行，构成跨平台密文互通的算法级回归。
TEST(CryptoVectorTest, Aes256GcmMatchesReferenceVectors) {
    const auto key = range(32);
    const auto nonce = range(12);
    const auto plain = range(16);
    ASSERT_TRUE(rk_win::aes256gcmSetNonceForTesting(nonce));
    for (const auto& v : kVectors) {
        rk_win::AesGcmCiphertext out;
        std::string err;
        ASSERT_TRUE(rk_win::aes256gcmEncrypt(key, plain, out, err, v.aad)) << err;
        EXPECT_EQ(hex(out.nonce), hex(nonce));
        EXPECT_EQ(hex(out.ciphertext), v.ciphertext) << "aad=" << v.aad;
        EXPECT_EQ(hex(out.tag), v.tag) << "aad=" << v.aad;
    }
    rk_win::aes256gcmClearNonceForTesting();
}

TEST(CryptoVectorTest, DecryptRoundTripsAndRejectsTamperedTagOrAad) {
    const auto key = range(32);
    const auto nonce = range(12);
    const auto plain = range(16);
    ASSERT_TRUE(rk_win::aes256gcmSetNonceForTesting(nonce));
    rk_win::AesGcmCiphertext out;
    std::string err;
    ASSERT_TRUE(rk_win::aes256gcmEncrypt(key, plain, out, err, "poster.postUrl")) << err;
    rk_win::aes256gcmClearNonceForTesting();

    std::vector<std::uint8_t> back;
    ASSERT_TRUE(rk_win::aes256gcmDecrypt(key, out, back, err, "poster.postUrl")) << err;
    EXPECT_EQ(back, plain);

    auto tampered = out;
    tampered.tag[0] ^= 0x01;
    EXPECT_FALSE(rk_win::aes256gcmDecrypt(key, tampered, back, err, "poster.postUrl"));

    EXPECT_FALSE(rk_win::aes256gcmDecrypt(key, out, back, err, "other.aad"));
}
