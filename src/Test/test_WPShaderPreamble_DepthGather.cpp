#include <doctest.h>

#include "WPShaderPreamble.hpp"

#include <string>
#include <string_view>

using wallpaper::kPreShaderCodeFrag;

// Pin the existence and shape of weSampleSceneDepthMinGather in the frag
// preamble.  Volumetric and future depth-aware effects depend on this helper
// being available unconditionally inside any fragment shader compiled through
// WPShaderParser.  A preamble refactor that drops the helper would silently
// regress those passes; this test catches it.
TEST_SUITE("WPShaderPreamble::weSampleSceneDepthMinGather") {
    TEST_CASE("helper signature present in frag preamble") {
        std::string_view preamble = kPreShaderCodeFrag;
        CHECK(preamble.find("float weSampleSceneDepthMinGather(") != std::string::npos);
        CHECK(preamble.find("sampler2D") != std::string::npos);
        CHECK(preamble.find("vec2") != std::string::npos);
    }
    TEST_CASE("uses textureLod with explicit lod=0") {
        std::string_view preamble = kPreShaderCodeFrag;
        CHECK(preamble.find("textureLod") != std::string::npos);
        CHECK(preamble.find(", 0.0)") != std::string::npos);
    }
    TEST_CASE("min-gather flips to max under REVERSEDEPTH=1") {
        std::string_view preamble = kPreShaderCodeFrag;
        auto if_pos    = preamble.find("#if REVERSEDEPTH");
        REQUIRE(if_pos != std::string::npos);
        auto else_pos  = preamble.find("#else", if_pos);
        REQUIRE(else_pos != std::string::npos);
        auto endif_pos = preamble.find("#endif", else_pos);
        REQUIRE(endif_pos != std::string::npos);

        std::string_view reverse_branch = preamble.substr(if_pos, else_pos - if_pos);
        std::string_view regular_branch = preamble.substr(else_pos, endif_pos - else_pos);

        CHECK(reverse_branch.find("max") != std::string::npos);
        CHECK(regular_branch.find("min") != std::string::npos);
    }
    TEST_CASE("samples four texels (2x2 gather)") {
        std::string_view preamble = kPreShaderCodeFrag;
        size_t count = 0;
        size_t pos   = preamble.find("weSampleSceneDepthMinGather");
        REQUIRE(pos != std::string::npos);
        size_t body_end = preamble.find("}", pos);
        REQUIRE(body_end != std::string::npos);
        std::string_view body = preamble.substr(pos, body_end - pos);
        size_t lod_pos = 0;
        while ((lod_pos = body.find("textureLod", lod_pos)) != std::string::npos) {
            count++;
            lod_pos++;
        }
        CHECK(count == 4);
    }
}
