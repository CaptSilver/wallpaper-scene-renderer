#include <doctest.h>

#include "WPTextRenderer.hpp"

#include <cstring>
#include <string>

using namespace wallpaper;

// Helper: feed bytes through WPTextRenderer::DecodeUtf8 and collect the
// decoded codepoints.  The decoder advances `p` byte-by-byte so a malformed
// run can produce multiple REPLACEMENT characters per input string.
static std::vector<uint32_t> decode_all(const char* data, size_t len) {
    std::vector<uint32_t> out;
    const char*           p   = data;
    const char*           end = data + len;
    while (p < end) {
        out.push_back(WPTextRenderer::DecodeUtf8(p, end));
    }
    return out;
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — well-formed UTF-8") {
    TEST_CASE("ASCII passes through unchanged") {
        const char  s[] = "abc";
        const char* p   = s;
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('a'));
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('b'));
        CHECK(WPTextRenderer::DecodeUtf8(p, s + 3) == uint32_t('c'));
        CHECK(p == s + 3);
    }

    TEST_CASE("2-byte sequence decodes (U+00E9 'é' = C3 A9)") {
        const char  bytes[] = { (char)0xC3, (char)0xA9 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == 0x00E9u);
    }

    TEST_CASE("3-byte sequence decodes (U+4E2D '中' = E4 B8 AD)") {
        const char  bytes[] = { (char)0xE4, (char)0xB8, (char)0xAD };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == 0x4E2Du);
    }

    TEST_CASE("4-byte sequence decodes (U+1F600 '😀' = F0 9F 98 80)") {
        const char  bytes[] = { (char)0xF0, (char)0x9F, (char)0x98, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 4) == 0x1F600u);
    }
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — malformed UTF-8 → U+FFFD") {
    TEST_CASE("overlong 2-byte: C0 80 (encodes U+0000 in 2 bytes) rejected") {
        // Overlong forms have historically been used to smuggle ASCII past
        // validators (e.g., null byte as `C0 80`).  Must NOT decode to U+0000.
        const char  bytes[] = { (char)0xC0, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("overlong 3-byte: E0 80 80 (encodes U+0000 in 3 bytes) rejected") {
        const char  bytes[] = { (char)0xE0, (char)0x80, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("UTF-16 surrogate half: ED A0 80 (U+D800) rejected") {
        // U+D800–U+DFFF are not valid scalar values; some fonts can crash
        // FT_Load_Char on them.
        const char  bytes[] = { (char)0xED, (char)0xA0, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("codepoint above Unicode max: F4 90 80 80 (U+110000) rejected") {
        // 4-byte form can carry up to U+1FFFFF; Unicode caps at U+10FFFF.
        const char  bytes[] = { (char)0xF4, (char)0x90, (char)0x80, (char)0x80 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 4) == WPTextRenderer::kReplacementChar);
    }

    TEST_CASE("non-continuation trailing byte: C3 41 ('A' after a 2-byte start)") {
        // Trailing byte must be 10xxxxxx.  An ASCII byte in the trailing
        // slot signals truncation — decoder returns FFFD and leaves `p` on
        // the bad byte so the next call resyncs at that ASCII.
        const char  bytes[] = { (char)0xC3, (char)0x41 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == WPTextRenderer::kReplacementChar);
        // Resync: next call decodes the 'A' as plain ASCII.
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 2) == uint32_t('A'));
    }

    TEST_CASE("truncated 4-byte sequence (only 3 bytes available)") {
        const char  bytes[] = { (char)0xF0, (char)0x9F, (char)0x98 };
        const char* p       = bytes;
        CHECK(WPTextRenderer::DecodeUtf8(p, bytes + 3) == WPTextRenderer::kReplacementChar);
    }
}

TEST_SUITE("WPTextRenderer::DecodeUtf8 — stream behavior") {
    TEST_CASE("end-of-buffer returns 0 (terminator), not FFFD") {
        const char* p = "";
        CHECK(WPTextRenderer::DecodeUtf8(p, p) == 0u);
    }

    TEST_CASE("mixed valid+invalid stream decodes each correctly") {
        // "a" + overlong-null + "b" + lone-surrogate + "c"
        std::string s;
        s.push_back('a');
        s.push_back((char)0xC0);
        s.push_back((char)0x80);
        s.push_back('b');
        s.push_back((char)0xED);
        s.push_back((char)0xA0);
        s.push_back((char)0x80);
        s.push_back('c');
        auto cps = decode_all(s.data(), s.size());
        REQUIRE(cps.size() == 5);
        CHECK(cps[0] == uint32_t('a'));
        CHECK(cps[1] == WPTextRenderer::kReplacementChar);
        CHECK(cps[2] == uint32_t('b'));
        CHECK(cps[3] == WPTextRenderer::kReplacementChar);
        CHECK(cps[4] == uint32_t('c'));
    }
}
