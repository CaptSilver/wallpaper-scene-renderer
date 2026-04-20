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
