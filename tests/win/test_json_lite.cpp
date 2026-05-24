#include "rk_win/JsonLite.h"
#include <iostream>
#include <string>

using namespace rk_win;

bool test_json_lite_surrogate_pairs() {
    std::string err;
    JsonValue out;

    // Test 1: Valid BMP character (\u0041 -> 'A')
    {
        bool ok = parseJson("\"\\u0041\"", out, err);
        if (!ok || !out.isString() || out.s != "A") {
            std::cerr << "FAIL: Test 1 - BMP char. err: " << err << std::endl;
            return false;
        }
    }

    // Test 2: Valid surrogate pair (U+1D11E 𝄞 -> \uD834\uDD1E)
    // UTF-8 sequence for U+1D11E is F0 9D 84 9E
    {
        bool ok = parseJson("\"\\uD834\\uDD1E\"", out, err);
        std::string expected;
        expected.push_back(static_cast<char>(0xF0));
        expected.push_back(static_cast<char>(0x9D));
        expected.push_back(static_cast<char>(0x84));
        expected.push_back(static_cast<char>(0x9E));





        if (!ok || !out.isString() || out.s != expected) {
            std::cerr << "FAIL: Test 2 - Surrogate pair. err: " << err << " out: " << (out.isString() ? out.s : "not string") << std::endl;
            return false;
        }
    }

    // Test 3: Isolated high surrogate (at end of string)
    {
        bool ok = parseJson("\"\\uD834\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 3 - Expected failure for isolated high surrogate at end." << std::endl;
            return false;
        }
    }

    // Test 4: Isolated high surrogate (followed by normal characters)
    {
        bool ok = parseJson("\"\\uD834abc\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 4 - Expected failure for isolated high surrogate followed by chars." << std::endl;
            return false;
        }
    }

    // Test 5: Isolated high surrogate (followed by another escape but not low surrogate)
    {
        bool ok = parseJson("\"\\uD834\\n\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 5 - Expected failure for isolated high surrogate followed by \\n." << std::endl;
            return false;
        }
    }

    // Test 6: High surrogate followed by invalid hex in low surrogate
    {
        bool ok = parseJson("\"\\uD834\\uDD1X\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 6 - Expected failure for invalid hex in low surrogate." << std::endl;
            return false;
        }
    }





    // Test 7: High surrogate followed by another high surrogate
    {
        bool ok = parseJson("\"\\uD834\\uD834\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 7 - Expected failure for high surrogate followed by high surrogate." << std::endl;
            return false;
        }
    }

    // Test 8: Isolated low surrogate
    {
        bool ok = parseJson("\"\\uDD1E\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 8 - Expected failure for isolated low surrogate." << std::endl;
            return false;
        }
    }

    // Test 9: Truncated escape sequence \u followed by EOF
    {
        bool ok = parseJson("\"\\u\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 9 - Expected failure for truncated escape." << std::endl;
            return false;
        }
    }

    // Test 10: Truncated escape sequence after high surrogate
    {
        bool ok = parseJson("\"\\uD834\\u\"", out, err);
        if (ok) {
            std::cerr << "FAIL: Test 10 - Expected failure for truncated low surrogate escape." << std::endl;
            return false;
        }
    }





    return true;
}
