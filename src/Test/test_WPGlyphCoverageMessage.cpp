#include <doctest.h>

#include "WPGlyphCoverageMessage.hpp"

using namespace wallpaper;

// BuildGlyphCoverageMessage backs the rate-limited LOG_INFO in
// WPTextRenderer's raster loop. The bug it fixes: the CJK fallback face
// resolving a real glyph still left the log claiming a .notdef box had
// been drawn, because the miss counter that fed the message didn't
// distinguish "no glyph anywhere" from "the fallback face had it".
TEST_SUITE("WPGlyphCoverageMessage.BuildGlyphCoverageMessage") {
    TEST_CASE("no misses, no fallback -> empty (nothing to log)") {
        CHECK(BuildGlyphCoverageMessage(0, 0, "hello").empty());
    }

    TEST_CASE("notdef only -> reports .notdef emission, no fallback wording") {
        const std::string msg = BuildGlyphCoverageMessage(4, 0, "by SYKM/\xE5\xB1\xB1\xE9\x9B\xA8");
        CHECK_FALSE(msg.empty());
        CHECK(msg.find("4 codepoint") != std::string::npos);
        CHECK(msg.find(".notdef") != std::string::npos);
        CHECK(msg.find("fallback") == std::string::npos);
    }

    TEST_CASE("fallback only -> reports resolution via fallback, no .notdef claim") {
        const std::string msg = BuildGlyphCoverageMessage(0, 4, "by SYKM/\xE5\xB1\xB1\xE9\x9B\xA8");
        CHECK_FALSE(msg.empty());
        CHECK(msg.find("4 codepoint") != std::string::npos);
        CHECK(msg.find("fallback") != std::string::npos);
        // The whole point of the fix: don't say .notdef was emitted when it wasn't.
        CHECK(msg.find(".notdef") == std::string::npos);
    }

    TEST_CASE("both notdef and fallback -> message reports both counts") {
        const std::string msg = BuildGlyphCoverageMessage(2, 3, "mixed text");
        CHECK_FALSE(msg.empty());
        CHECK(msg.find("2 codepoint") != std::string::npos);
        CHECK(msg.find(".notdef") != std::string::npos);
        CHECK(msg.find("3") != std::string::npos);
        CHECK(msg.find("fallback") != std::string::npos);
    }

    TEST_CASE("sample text is truncated the same way the old message truncated it") {
        const std::string longText(64, 'x');
        const std::string msg = BuildGlyphCoverageMessage(1, 0, longText);
        // %.32s semantics: at most 32 bytes of the sample text survive.
        CHECK(msg.find(std::string(33, 'x')) == std::string::npos);
        CHECK(msg.find(std::string(32, 'x')) != std::string::npos);
    }
}
