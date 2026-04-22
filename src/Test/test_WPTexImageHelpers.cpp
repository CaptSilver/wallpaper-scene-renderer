#include <doctest.h>

#include "WPTexImageHelpers.h"

using namespace wallpaper;
using namespace wallpaper::teximage_helpers;

// ===========================================================================
// ToTexFormate
// ===========================================================================

TEST_SUITE("ToTexFormate") {
    TEST_CASE("type 0 RGBA8") { CHECK(ToTexFormate(0) == TextureFormat::RGBA8); }

    TEST_CASE("type 4 BC3") { CHECK(ToTexFormate(4) == TextureFormat::BC3); }

    TEST_CASE("type 6 BC2") { CHECK(ToTexFormate(6) == TextureFormat::BC2); }

    TEST_CASE("type 7 BC1") { CHECK(ToTexFormate(7) == TextureFormat::BC1); }

    TEST_CASE("type 8 RG8") { CHECK(ToTexFormate(8) == TextureFormat::RG8); }

    TEST_CASE("type 9 R8") { CHECK(ToTexFormate(9) == TextureFormat::R8); }

    TEST_CASE("type 10 RGBA16F") { CHECK(ToTexFormate(10) == TextureFormat::RGBA16F); }

    TEST_CASE("type 11 RG16F") { CHECK(ToTexFormate(11) == TextureFormat::RG16F); }

    TEST_CASE("type 12 BC7") { CHECK(ToTexFormate(12) == TextureFormat::BC7); }

    TEST_CASE("type 13 R16F") { CHECK(ToTexFormate(13) == TextureFormat::R16F); }

    TEST_CASE("type 14 BC6H") { CHECK(ToTexFormate(14) == TextureFormat::BC6H); }

    TEST_CASE("type 15 RGB565") { CHECK(ToTexFormate(15) == TextureFormat::RGB565); }

    TEST_CASE("type 16 RGBA1010102") { CHECK(ToTexFormate(16) == TextureFormat::RGBA1010102); }

    TEST_CASE("type 99 fallback RGBA8") { CHECK(ToTexFormate(99) == TextureFormat::RGBA8); }

    TEST_CASE("type -1 fallback RGBA8") { CHECK(ToTexFormate(-1) == TextureFormat::RGBA8); }

} // TEST_SUITE

// ===========================================================================
// IsAliasTexture
// ===========================================================================

TEST_SUITE("IsAliasTexture") {
    TEST_CASE("alias prefix returns true") { CHECK(IsAliasTexture("_alias_foo") == true); }

    TEST_CASE("alias prefix bare returns true") { CHECK(IsAliasTexture("_alias_") == true); }

    TEST_CASE("normal texture returns false") { CHECK(IsAliasTexture("normal.tex") == false); }

    TEST_CASE("empty string returns false") { CHECK(IsAliasTexture("") == false); }

    TEST_CASE("partial prefix returns false") { CHECK(IsAliasTexture("_alias") == false); }

} // TEST_SUITE

// ===========================================================================
// SetHeaderPow2
// ===========================================================================

TEST_SUITE("SetHeaderPow2") {
    TEST_CASE("pow2 dimensions set mipmap_pow2 true") {
        ImageHeader header;
        header.mapWidth  = 256;
        header.mapHeight = 256;
        SetHeaderPow2(header, 256, 256);
        CHECK(header.mipmap_pow2 == true);
    }

    TEST_CASE("non-pow2 dimensions set mipmap_pow2 false") {
        ImageHeader header;
        header.mapWidth  = 100;
        header.mapHeight = 100;
        SetHeaderPow2(header, 100, 100);
        CHECK(header.mipmap_pow2 == false);
    }

    TEST_CASE("one pow2 dimension is enough") {
        ImageHeader header;
        header.mapWidth  = 100;
        header.mapHeight = 100;
        SetHeaderPow2(header, 256, 100);
        CHECK(header.mipmap_pow2 == true);
    }

    TEST_CASE("mipmap_larger true when mip > map") {
        ImageHeader header;
        header.mapWidth  = 100;
        header.mapHeight = 100;
        SetHeaderPow2(header, 256, 256);
        // 256*256 = 65536 > 100*100 = 10000
        CHECK(header.mipmap_larger == true);
    }

    TEST_CASE("mipmap_larger false when mip <= map") {
        ImageHeader header;
        header.mapWidth  = 256;
        header.mapHeight = 256;
        SetHeaderPow2(header, 128, 128);
        // 128*128 = 16384 < 256*256 = 65536
        CHECK(header.mipmap_larger == false);
    }

    TEST_CASE("mipmap_larger false when equal") {
        ImageHeader header;
        header.mapWidth  = 256;
        header.mapHeight = 256;
        SetHeaderPow2(header, 256, 256);
        // 256*256 == 256*256, not strictly greater
        CHECK(header.mipmap_larger == false);
    }

} // TEST_SUITE

TEST_SUITE("SanitizePathSeparatorChar") {
    TEST_CASE("forward slash becomes underscore") {
        CHECK(SanitizePathSeparatorChar('/') == '_');
    }
    TEST_CASE("backslash becomes underscore") {
        CHECK(SanitizePathSeparatorChar('\\') == '_');
    }
    TEST_CASE("colon becomes underscore") {
        CHECK(SanitizePathSeparatorChar(':') == '_');
    }
    TEST_CASE("alphanumeric passes through unchanged") {
        CHECK(SanitizePathSeparatorChar('a') == 'a');
        CHECK(SanitizePathSeparatorChar('Z') == 'Z');
        CHECK(SanitizePathSeparatorChar('0') == '0');
        CHECK(SanitizePathSeparatorChar('9') == '9');
    }
    TEST_CASE("adjacent ascii chars pass through") {
        // Boundary neighbors — ensure we aren't over-matching via a flipped
        // comparison (e.g. '.'=46 vs '/'=47; ';'=59 vs ':'=58).
        CHECK(SanitizePathSeparatorChar('.') == '.');
        CHECK(SanitizePathSeparatorChar('0') == '0');
        CHECK(SanitizePathSeparatorChar(';') == ';');
        CHECK(SanitizePathSeparatorChar('9') == '9');
        CHECK(SanitizePathSeparatorChar('[') == '[');
        CHECK(SanitizePathSeparatorChar(']') == ']');
    }
    TEST_CASE("whole-string loop replaces all separators") {
        std::string s = "a/b\\c:d";
        for (char& c : s) c = SanitizePathSeparatorChar(c);
        CHECK(s == "a_b_c_d");
    }
}

TEST_SUITE("Rgba8ByteSize") {
    TEST_CASE("zero dimensions yield zero bytes") {
        CHECK(Rgba8ByteSize(0, 0) == 0);
        CHECK(Rgba8ByteSize(0, 16) == 0);
        CHECK(Rgba8ByteSize(16, 0) == 0);
    }
    TEST_CASE("1x1 RGBA8 is 4 bytes") {
        CHECK(Rgba8ByteSize(1, 1) == 4);
    }
    TEST_CASE("8x8 RGBA8 is 256 bytes") {
        // Distinguishes w*h*4 from w/h*4 (=4) and w*h/4 (=16).
        CHECK(Rgba8ByteSize(8, 8) == 256);
    }
    TEST_CASE("rectangular dimensions multiply") {
        CHECK(Rgba8ByteSize(4, 2) == 32);
        CHECK(Rgba8ByteSize(2, 4) == 32);
        CHECK(Rgba8ByteSize(3, 5) == 60);
    }
    TEST_CASE("power-of-two dims match simd-aligned layout") {
        CHECK(Rgba8ByteSize(1024, 1024) == 4'194'304);
        CHECK(Rgba8ByteSize(256, 256) == 262'144);
    }
}
