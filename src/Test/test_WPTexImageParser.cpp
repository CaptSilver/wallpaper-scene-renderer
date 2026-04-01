#include <doctest.h>

#include "WPTexImageParser.hpp"
#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"
#include "Type.hpp"

#include <cstring>
#include <lz4.h>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// ---------------------------------------------------------------------------
// Mock filesystem: serves pre-built binary blobs keyed by VFS-internal path
// ---------------------------------------------------------------------------
class MockFs : public Fs {
public:
    void AddFile(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }

    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }

    std::shared_ptr<IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second; // copy so stream owns its data
        return std::make_shared<MemBinaryStream>(std::move(copy));
    }

    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

// ---------------------------------------------------------------------------
// Binary builder helpers
// ---------------------------------------------------------------------------
namespace
{

// Append raw bytes
void append(std::vector<uint8_t>& buf, const void* data, size_t n) {
    auto p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + n);
}

void appendInt32(std::vector<uint8_t>& buf, int32_t v) { append(buf, &v, 4); }
void appendUint32(std::vector<uint8_t>& buf, uint32_t v) { append(buf, &v, 4); }
void appendFloat(std::vector<uint8_t>& buf, float v) { append(buf, &v, 4); }

// Write a 9-byte TEX version header (matches WriteVersion format)
void appendTexVersion(std::vector<uint8_t>& buf, int ver) {
    char hdr[9] {};
    std::snprintf(hdr, sizeof(hdr), "%.4s%.4d", "TEX", ver);
    append(buf, hdr, 9);
}

// Build the common .tex header portion (TEXV+TEXI+fields+TEXB)
// Returns the buffer so callers can append image data / sprite data after it
std::vector<uint8_t> makeTexHeader(int texvVer, int texiVer, int texbVer, int32_t type,
                                   uint32_t flags, int32_t width, int32_t height,
                                   int32_t mapWidth, int32_t mapHeight, int32_t count) {
    std::vector<uint8_t> buf;
    buf.reserve(256);
    appendTexVersion(buf, texvVer); // TEXV
    appendTexVersion(buf, texiVer); // TEXI
    appendInt32(buf, type);         // texture type
    appendUint32(buf, flags);       // flags
    appendInt32(buf, width);
    appendInt32(buf, height);
    appendInt32(buf, mapWidth);
    appendInt32(buf, mapHeight);
    appendInt32(buf, 0); // unknown, skipped
    appendTexVersion(buf, texbVer); // TEXB
    appendInt32(buf, count);
    return buf;
}

// Append one mipmap's data for texb==1 (no LZ4 fields)
void appendMipmapV1(std::vector<uint8_t>& buf, int32_t w, int32_t h,
                    const std::vector<uint8_t>& pixels) {
    appendInt32(buf, w);
    appendInt32(buf, h);
    appendInt32(buf, static_cast<int32_t>(pixels.size()));
    append(buf, pixels.data(), pixels.size());
}

// Append one mipmap's data for texb==2 (with LZ4 fields)
void appendMipmapV2(std::vector<uint8_t>& buf, int32_t w, int32_t h,
                    const std::vector<uint8_t>& pixels, bool compress = false) {
    appendInt32(buf, w);
    appendInt32(buf, h);
    if (compress) {
        int maxDst = LZ4_compressBound(static_cast<int>(pixels.size()));
        std::vector<char> compressed(static_cast<size_t>(maxDst));
        int compressedSize =
            LZ4_compress_default(reinterpret_cast<const char*>(pixels.data()), compressed.data(),
                                 static_cast<int>(pixels.size()), maxDst);
        appendInt32(buf, 1); // LZ4_compressed = true
        appendInt32(buf, static_cast<int32_t>(pixels.size()));
        appendInt32(buf, compressedSize);
        append(buf, compressed.data(), static_cast<size_t>(compressedSize));
    } else {
        appendInt32(buf, 0); // LZ4_compressed = false
        appendInt32(buf, 0); // decompressed_size (unused when not compressed)
        appendInt32(buf, static_cast<int32_t>(pixels.size()));
        append(buf, pixels.data(), pixels.size());
    }
}

// Build a complete simple RGBA8 .tex file (1 image, 1 mipmap, texb=1, no sprite)
std::vector<uint8_t> makeSimpleRGBA8Tex(int32_t w, int32_t h) {
    auto buf = makeTexHeader(1, 1, 1, /*type=RGBA8*/ 0, /*flags=*/ 0, w, h, w, h, /*count=*/ 1);
    // image 0: 1 mipmap
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(static_cast<size_t>(w * h * 4), 0xAB);
    appendMipmapV1(buf, w, h, pixels);
    return buf;
}

// Mount a .tex blob into VFS at the path WPTexImageParser expects
void mountTex(VFS& vfs, const std::string& name, std::vector<uint8_t> data) {
    // WPTexImageParser opens "/assets/materials/<name>.tex"
    // VFS mount at "/assets" strips to "/materials/<name>.tex"
    auto mockFs = std::make_unique<MockFs>();
    mockFs->AddFile("/materials/" + name + ".tex", std::move(data));
    vfs.Mount("/assets", std::move(mockFs));
}

} // namespace

// ===========================================================================
// Tests
// ===========================================================================

TEST_SUITE("WPTexImageParser") {

TEST_CASE("Valid RGBA8 texture — Parse") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(4, 4);
    mountTex(vfs, "test_rgba8", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_rgba8");

    REQUIRE(img != nullptr);
    CHECK(img->key == "test_rgba8");
    CHECK(img->header.format == TextureFormat::RGBA8);
    CHECK(img->header.width == 4);
    CHECK(img->header.height == 4);
    CHECK(img->header.count == 1);
    CHECK(img->header.isSprite == false);
    REQUIRE(img->slots.size() == 1);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    CHECK(img->slots[0].mipmaps[0].width == 4);
    CHECK(img->slots[0].mipmaps[0].height == 4);
    CHECK(img->slots[0].mipmaps[0].size == 4 * 4 * 4);
    // Verify pixel data
    CHECK(img->slots[0].mipmaps[0].data.get()[0] == 0xAB);
}

TEST_CASE("Valid RGBA8 texture — ParseHeader") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(8, 8);
    mountTex(vfs, "test_hdr", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("test_hdr");

    CHECK(header.format == TextureFormat::RGBA8);
    CHECK(header.width == 8);
    CHECK(header.height == 8);
    CHECK(header.count == 1);
    CHECK(header.isSprite == false);
}

TEST_CASE("BC1 (DXT1) format") {
    // type=7 → BC1
    auto buf = makeTexHeader(1, 1, 1, 7, 0, 4, 4, 4, 4, 1);
    // BC1: 8 bytes per 4x4 block = 8 bytes for a 4x4 texture
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> blockData(8, 0);
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc1", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc1");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC1);
}

TEST_CASE("BC3 (DXT5) format") {
    auto buf = makeTexHeader(1, 1, 1, 4, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> blockData(16, 0); // 16 bytes per 4x4 block
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc3", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc3");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC3);
}

TEST_CASE("BC2 (DXT3) format") {
    auto buf = makeTexHeader(1, 1, 1, 6, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> blockData(16, 0);
    appendMipmapV1(buf, 4, 4, blockData);

    VFS vfs;
    mountTex(vfs, "test_bc2", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_bc2");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::BC2);
}

TEST_CASE("RG8 format") {
    auto buf = makeTexHeader(1, 1, 1, 8, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 2, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_rg8", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_rg8");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::RG8);
}

TEST_CASE("R8 format") {
    auto buf = makeTexHeader(1, 1, 1, 9, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_r8", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_r8");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::R8);
}

TEST_CASE("Unknown format type falls back to RGBA8") {
    auto buf = makeTexHeader(1, 1, 1, 99, 0, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_unk", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_unk");
    REQUIRE(img != nullptr);
    CHECK(img->header.format == TextureFormat::RGBA8);
}

TEST_CASE("Alias texture fallback — missing file") {
    VFS vfs;
    // Mount an empty filesystem so Contains returns false
    auto mockFs = std::make_unique<MockFs>();
    vfs.Mount("/assets", std::move(mockFs));

    WPTexImageParser parser(&vfs);

    SUBCASE("Parse returns 1x1 white fallback") {
        auto img = parser.Parse("_alias_missing");
        REQUIRE(img != nullptr);
        CHECK(img->key == "_alias_missing");
        CHECK(img->header.width == 1);
        CHECK(img->header.height == 1);
        CHECK(img->header.format == TextureFormat::RGBA8);
        REQUIRE(img->slots.size() == 1);
        REQUIRE(img->slots[0].mipmaps.size() == 1);
        auto* px = img->slots[0].mipmaps[0].data.get();
        CHECK(px[0] == 255);
        CHECK(px[1] == 255);
        CHECK(px[2] == 255);
        CHECK(px[3] == 255);
    }

    SUBCASE("ParseHeader returns 1x1 fallback header") {
        auto header = parser.ParseHeader("_alias_missing");
        CHECK(header.width == 1);
        CHECK(header.height == 1);
        CHECK(header.format == TextureFormat::RGBA8);
        CHECK(header.count == 1);
    }
}

TEST_CASE("Alias texture with existing file — parses normally") {
    VFS vfs;
    auto texData = makeSimpleRGBA8Tex(2, 2);
    mountTex(vfs, "_alias_exists", std::move(texData));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("_alias_exists");
    REQUIRE(img != nullptr);
    CHECK(img->header.width == 2);
    CHECK(img->header.height == 2);
}

TEST_CASE("Sprite with out-of-range imageId — disables sprite") {
    // Build a sprite texture: 1 image, sprite flag set
    uint32_t spriteFlag = (1u << 2); // WPTexFlagEnum::sprite = bit 2
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 16, 16, 16, 16, 1);

    // Image 0: 1 mipmap (texb=2, so LZ4 fields present)
    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(16 * 16 * 4, 0);
    appendMipmapV2(buf, 16, 16, pixels);

    // Sprite section
    appendTexVersion(buf, 2); // texs version
    appendInt32(buf, 1);      // framecount = 1

    // Frame with out-of-range imageId (only 1 image, so imageId=1 is OOB)
    appendInt32(buf, 1);      // imageId = 1 (invalid, only index 0 exists)
    appendFloat(buf, 0.1f);   // frametime
    appendFloat(buf, 0.0f);   // x
    appendFloat(buf, 0.0f);   // y
    appendFloat(buf, 16.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_oob", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_oob");
    // Should disable sprite instead of crashing
    CHECK(header.isSprite == false);
}

TEST_CASE("Sprite with negative imageId — disables sprite") {
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 8, 8, 8, 8, 1);

    appendInt32(buf, 1); // mipmap_count
    std::vector<uint8_t> pixels(8 * 8 * 4, 0);
    appendMipmapV2(buf, 8, 8, pixels);

    appendTexVersion(buf, 2); // texs
    appendInt32(buf, 1);      // framecount

    appendInt32(buf, -1);     // imageId = -1 (invalid)
    appendFloat(buf, 0.1f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 8.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 0.0f);
    appendFloat(buf, 8.0f);

    VFS vfs;
    mountTex(vfs, "sprite_neg", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_neg");
    CHECK(header.isSprite == false);
}

TEST_CASE("Valid sprite — parsed correctly") {
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 16, 16, 16, 16, 1);

    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(16 * 16 * 4, 0);
    appendMipmapV2(buf, 16, 16, pixels);

    appendTexVersion(buf, 2); // texs
    appendInt32(buf, 1);      // framecount = 1

    appendInt32(buf, 0);      // imageId = 0 (valid)
    appendFloat(buf, 0.5f);   // frametime
    appendFloat(buf, 0.0f);   // x
    appendFloat(buf, 0.0f);   // y
    appendFloat(buf, 16.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_ok", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_ok");
    CHECK(header.isSprite == true);
    CHECK(header.spriteAnim.numFrames() == 1);
    auto& frame = header.spriteAnim.GetCurFrame();
    CHECK(frame.imageId == 0);
    CHECK(frame.frametime == doctest::Approx(0.5f));
}

TEST_CASE("LZ4 compressed mipmap") {
    auto buf = makeTexHeader(1, 1, 2, 0, 0, 4, 4, 4, 4, 1);

    // Create pixel data and compress it
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = static_cast<uint8_t>(i & 0xFF);
    }

    appendInt32(buf, 1); // mipmap_count
    appendMipmapV2(buf, 4, 4, pixels, /*compress=*/true);

    VFS vfs;
    mountTex(vfs, "test_lz4", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_lz4");
    REQUIRE(img != nullptr);
    REQUIRE(img->slots.size() == 1);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    CHECK(img->slots[0].mipmaps[0].size == static_cast<isize>(pixels.size()));
    // Verify decompressed data matches original
    auto* data = img->slots[0].mipmaps[0].data.get();
    for (size_t i = 0; i < pixels.size(); i++) {
        CHECK(data[i] == pixels[i]);
    }
}

TEST_CASE("Zero src_size returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 1);       // mipmap_count
    appendInt32(buf, 4);       // mip width
    appendInt32(buf, 4);       // mip height
    appendInt32(buf, 0);       // src_size = 0

    VFS vfs;
    mountTex(vfs, "test_zero", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_zero");
    CHECK(img == nullptr);
}

TEST_CASE("Negative width returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, -1, 4, 4, 4, 1);
    appendInt32(buf, 1);
    appendInt32(buf, -1);      // mip width < 0
    appendInt32(buf, 4);       // mip height
    appendInt32(buf, 64);      // src_size
    std::vector<uint8_t> pixels(64, 0);
    append(buf, pixels.data(), pixels.size());

    VFS vfs;
    mountTex(vfs, "test_negw", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_negw");
    CHECK(img == nullptr);
}

TEST_CASE("Negative count returns nullptr") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, -1);

    VFS vfs;
    mountTex(vfs, "test_negcount", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_negcount");
    CHECK(img == nullptr);
}

TEST_CASE("Non-existent file returns nullptr") {
    VFS vfs;
    auto mockFs = std::make_unique<MockFs>();
    vfs.Mount("/assets", std::move(mockFs));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("nonexistent");
    CHECK(img == nullptr);
}

TEST_CASE("Texture flags — clampUVs and noInterpolation") {
    // flags: bit 0 = noInterpolation, bit 1 = clampUVs
    uint32_t flags = (1u << 0) | (1u << 1);
    auto buf = makeTexHeader(1, 1, 1, 0, flags, 2, 2, 2, 2, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(2 * 2 * 4, 0);
    appendMipmapV1(buf, 2, 2, pixels);

    VFS vfs;
    mountTex(vfs, "test_flags", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_flags");
    REQUIRE(img != nullptr);
    CHECK(img->header.sample.wrapS == TextureWrap::CLAMP_TO_EDGE);
    CHECK(img->header.sample.wrapT == TextureWrap::CLAMP_TO_EDGE);
    CHECK(img->header.sample.magFilter == TextureFilter::NEAREST);
    CHECK(img->header.sample.minFilter == TextureFilter::NEAREST);
}

TEST_CASE("Multiple images") {
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 2, 2, 2, 2, 2); // count=2
    for (int i = 0; i < 2; i++) {
        appendInt32(buf, 1); // mipmap_count
        std::vector<uint8_t> pixels(2 * 2 * 4, static_cast<uint8_t>(i + 1));
        appendMipmapV1(buf, 2, 2, pixels);
    }

    VFS vfs;
    mountTex(vfs, "test_multi", std::move(buf));

    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_multi");
    REQUIRE(img != nullptr);
    CHECK(img->header.count == 2);
    REQUIRE(img->slots.size() == 2);
    CHECK(img->slots[0].mipmaps[0].data.get()[0] == 1);
    CHECK(img->slots[1].mipmaps[0].data.get()[0] == 2);
}

// --- Mutation testing: TEXB v3/v4 header version branches ---

// Build texb v3 header (has imageType field after TEXB+count)
std::vector<uint8_t> makeTexHeaderV3(int32_t imageType, int32_t w, int32_t h) {
    auto buf = makeTexHeader(1, 1, 3, 0, 0, w, h, w, h, 1);
    appendInt32(buf, imageType); // imageType field (texb >= 3)
    return buf;
}

// Build texb v4 header (has imageType + isVideoMp4 fields)
std::vector<uint8_t> makeTexHeaderV4(int32_t imageType, int32_t w, int32_t h) {
    auto buf = makeTexHeader(1, 1, 4, 0, 0, w, h, w, h, 1);
    appendInt32(buf, imageType); // imageType (texb >= 3)
    appendInt32(buf, 0);         // isVideoMp4 (texb >= 4)
    return buf;
}

TEST_CASE("TEXB v3 header reads imageType — parses successfully") {
    // texb==3 must read the imageType field; if ge_to_gt mutant fires
    // (>= 3 becomes > 3), the imageType int won't be consumed and
    // the stream will be misaligned, causing a parse failure.
    auto buf = makeTexHeader(1, 1, 3, 0, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 0); // imageType = UNKNOWN (texb >= 3)
    appendInt32(buf, 1); // mipmap count
    std::vector<uint8_t> pixels(4 * 4 * 4, 0xCC);
    appendMipmapV2(buf, 4, 4, pixels);

    VFS vfs;
    mountTex(vfs, "texb3_test", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("texb3_test");
    REQUIRE(img != nullptr);
    CHECK(img->slots[0].width == 4);
    CHECK(img->slots[0].height == 4);
}

TEST_CASE("TEXB v4 header reads imageType and isVideoMp4") {
    auto buf = makeTexHeader(1, 1, 4, 0, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 0); // imageType = UNKNOWN (texb >= 3)
    appendInt32(buf, 0); // isVideoMp4 flag    (texb >= 4)
    appendInt32(buf, 1); // mipmap count
    appendInt32(buf, 4); // w
    appendInt32(buf, 4); // h
    appendInt32(buf, 0); appendInt32(buf, 0); // LZ4
    std::vector<uint8_t> pixels(4 * 4 * 4, 0xDD);
    appendInt32(buf, (int32_t)pixels.size());
    append(buf, pixels.data(), pixels.size());

    VFS vfs;
    mountTex(vfs, "texb4_test", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("texb4_test");
    REQUIRE(img != nullptr);
    CHECK(img->slots[0].width == 4);
    CHECK(img->slots[0].height == 4);
}

TEST_CASE("Mipmap size = width * height * sizeof(uint8_t)") {
    // Kills mul_to_div on line 225: mipmap.size = src_size * sizeof(uint8_t)
    // and line 219: src_size = w * h * 4
    auto buf = makeSimpleRGBA8Tex(8, 4);
    VFS vfs;
    mountTex(vfs, "size_check", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("size_check");
    REQUIRE(img != nullptr);
    CHECK(img->slots[0].mipmaps[0].size == 8 * 4 * 4);
}

// --- Mutation testing: sprite with texs=1 (integer frame data) ---

TEST_CASE("Sprite texs=1 with non-zero coords verifies division") {
    // Kills div_to_mul on lines 299-300: x=(float)ReadInt32()/spriteWidth
    // With x=8, spriteWidth=32: 8/32=0.25, but 8*32=256
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 32, 16, 32, 16, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(32 * 16 * 4, 0);
    appendMipmapV2(buf, 32, 16, pixels);
    appendTexVersion(buf, 1); // texs=1 (integer format)
    appendInt32(buf, 1);      // framecount
    appendInt32(buf, 0);      // imageId
    appendFloat(buf, 0.1f);   // frametime
    appendInt32(buf, 8);      // x (int) → 8/32 = 0.25
    appendInt32(buf, 4);      // y (int) → 4/16 = 0.25
    appendInt32(buf, 16);     // xAxis[0]
    appendInt32(buf, 0);      // xAxis[1]
    appendInt32(buf, 0);      // yAxis[0]
    appendInt32(buf, 8);      // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_v1_div", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_v1_div");
    CHECK(header.isSprite == true);
    auto& frame = header.spriteAnim.GetCurFrame();
    CHECK(frame.x == doctest::Approx(0.25f));
    CHECK(frame.y == doctest::Approx(0.25f));
    // xAxis[0]/spriteWidth = 16/32 = 0.5
    CHECK(frame.xAxis[0] == doctest::Approx(0.5f));
    // yAxis[1]/spriteHeight = 8/16 = 0.5
    CHECK(frame.yAxis[1] == doctest::Approx(0.5f));
}

TEST_CASE("Sprite texs=1 reads integer frame coordinates") {
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 32, 32, 32, 32, 1);
    // Image 0: 1 mipmap
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(32 * 32 * 4, 0);
    appendMipmapV2(buf, 32, 32, pixels);
    // Sprite section with texs=1 (integer format)
    appendTexVersion(buf, 1); // texs=1
    appendInt32(buf, 1);      // framecount
    appendInt32(buf, 0);      // imageId
    appendFloat(buf, 0.1f);   // frametime
    // texs==1: 6 int32 values (x, y, xAxis[0], xAxis[1], yAxis[0], yAxis[1])
    appendInt32(buf, 0);      // x (int)
    appendInt32(buf, 0);      // y (int)
    appendInt32(buf, 16);     // xAxis[0] (int)
    appendInt32(buf, 0);      // xAxis[1] (int)
    appendInt32(buf, 0);      // yAxis[0] (int)
    appendInt32(buf, 16);     // yAxis[1] (int)

    VFS vfs;
    mountTex(vfs, "sprite_v1", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_v1");
    CHECK(header.isSprite == true);
    CHECK(header.spriteAnim.numFrames() == 1);
    auto& frame = header.spriteAnim.GetCurFrame();
    CHECK(frame.imageId == 0);
    CHECK(frame.frametime == doctest::Approx(0.1f));
    // x = 0/32 = 0.0, y = 0/32 = 0.0
    CHECK(frame.x == doctest::Approx(0.0f));
    CHECK(frame.y == doctest::Approx(0.0f));
    // width = sqrt(16^2 + 0^2) = 16
    CHECK(frame.width == doctest::Approx(16.0f));
    // height = sqrt(0^2 + 16^2) = 16
    CHECK(frame.height == doctest::Approx(16.0f));
    // rate = height / width = 1.0
    CHECK(frame.rate == doctest::Approx(1.0f));
}

TEST_CASE("Sprite frame coordinate division by sprite dimensions") {
    // Kills div_to_mul: sf.x = (float)file.ReadInt32() / spriteWidth
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 64, 32, 64, 32, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(64 * 32 * 4, 0);
    appendMipmapV2(buf, 64, 32, pixels);
    appendTexVersion(buf, 2); // texs=2 (float format)
    appendInt32(buf, 1);      // framecount
    appendInt32(buf, 0);      // imageId
    appendFloat(buf, 0.5f);   // frametime
    appendFloat(buf, 16.0f);  // x (float)
    appendFloat(buf, 8.0f);   // y (float)
    appendFloat(buf, 32.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_coords", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_coords");
    CHECK(header.isSprite == true);
    auto& frame = header.spriteAnim.GetCurFrame();
    // x = 16.0/64 = 0.25, y = 8.0/32 = 0.25
    CHECK(frame.x == doctest::Approx(0.25f));
    CHECK(frame.y == doctest::Approx(0.25f));
    // xAxis[0]/spriteWidth = 32/64 = 0.5
    CHECK(frame.xAxis[0] == doctest::Approx(0.5f));
    // yAxis[1]/spriteHeight = 16/32 = 0.5
    CHECK(frame.yAxis[1] == doctest::Approx(0.5f));
    // width = sqrt(32^2 + 0^2) = 32
    CHECK(frame.width == doctest::Approx(32.0f));
    // height = sqrt(0^2 + 16^2) = 16
    CHECK(frame.height == doctest::Approx(16.0f));
    // rate = 16 / 32 = 0.5
    CHECK(frame.rate == doctest::Approx(0.5f));
}

TEST_CASE("Sprite texs=3 reads extra width/height fields") {
    // Kills gt_to_ge and gt_to_le on line 275: texs > 3 → error
    // texs=3 should NOT trigger the error; texs=4 should
    uint32_t spriteFlag = (1u << 2);
    auto buf = makeTexHeader(1, 1, 2, 0, spriteFlag, 16, 16, 16, 16, 1);
    appendInt32(buf, 1);
    std::vector<uint8_t> pixels(16 * 16 * 4, 0);
    appendMipmapV2(buf, 16, 16, pixels);
    appendTexVersion(buf, 3); // texs=3
    appendInt32(buf, 1);      // framecount
    // texs==3 has extra width/height before frames
    appendInt32(buf, 16);     // extra width
    appendInt32(buf, 16);     // extra height
    // Frame data (texs >= 2 → float format)
    appendInt32(buf, 0);      // imageId
    appendFloat(buf, 0.5f);   // frametime
    appendFloat(buf, 0.0f);   // x
    appendFloat(buf, 0.0f);   // y
    appendFloat(buf, 16.0f);  // xAxis[0]
    appendFloat(buf, 0.0f);   // xAxis[1]
    appendFloat(buf, 0.0f);   // yAxis[0]
    appendFloat(buf, 16.0f);  // yAxis[1]

    VFS vfs;
    mountTex(vfs, "sprite_v3", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto header = parser.ParseHeader("sprite_v3");
    CHECK(header.isSprite == true);
    CHECK(header.spriteAnim.numFrames() == 1);
}

TEST_CASE("Count of zero produces valid empty image") {
    // Kills lt_to_le on line 164: count < 0 returns nullptr
    // count=0 should NOT return nullptr — it's valid (empty slots)
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, 0);
    VFS vfs;
    mountTex(vfs, "test_zero_count", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_zero_count");
    REQUIRE(img != nullptr);
    CHECK(img->slots.size() == 0);
}

TEST_CASE("Multiple mipmaps parsed correctly") {
    // Kills post_inc_to_post_dec on loop counter (line 175)
    auto buf = makeTexHeader(1, 1, 1, 0, 0, 4, 4, 4, 4, 1);
    appendInt32(buf, 2); // 2 mipmaps
    // Mipmap 0: 4x4
    std::vector<uint8_t> pix4(4 * 4 * 4, 0xAA);
    appendMipmapV1(buf, 4, 4, pix4);
    // Mipmap 1: 2x2
    std::vector<uint8_t> pix2(2 * 2 * 4, 0xBB);
    appendMipmapV1(buf, 2, 2, pix2);

    VFS vfs;
    mountTex(vfs, "test_2mip", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("test_2mip");
    REQUIRE(img != nullptr);
    REQUIRE(img->slots[0].mipmaps.size() == 2);
    CHECK(img->slots[0].mipmaps[0].width == 4);
    CHECK(img->slots[0].mipmaps[1].width == 2);
}

// -----------------------------------------------------------------------
// stbi container path (TEXB v3+, imageType != UNKNOWN)
// Kills:
//   * src_size = w*h*4 mutation (→ w*h*3): size would be 3, not 4
//   * type != UNKNOWN mutation (→ == UNKNOWN): falls through to raw path, size stays 21
//   * texb >= 3 is already killed by "TEXB v3 header reads imageType" test
// -----------------------------------------------------------------------

// Minimal 1x1 red-pixel TGA (type 2, uncompressed true-color, 24 bpp, BGR order).
// 18-byte header + 3-byte pixel.  stbi decodes to RGBA {255,0,0,255} when
// called with n_req=4.
static const uint8_t kRedPixelTga[] = {
    /* id_len */  0x00,
    /* cmap_t */  0x00,
    /* img_t  */  0x02, // uncompressed true-color
    /* cmap spec: first_entry, length, entry_size */
    0x00, 0x00,  0x00, 0x00,  0x00,
    /* x_origin */ 0x00, 0x00,
    /* y_origin */ 0x00, 0x00,
    /* width   */ 0x01, 0x00,  // 1 px (little-endian)
    /* height  */ 0x01, 0x00,  // 1 px
    /* bpp     */ 0x18,        // 24
    /* img_desc*/ 0x00,
    /* pixel (BGR): blue=0, green=0, red=255 */
    0x00, 0x00, 0xFF
};

TEST_CASE("TEXB v3 stbi path — valid TGA decoded to RGBA") {
    // imageType = TARGA (17) → stbi decodes, src_size = w*h*4 = 4
    auto buf = makeTexHeader(1, 1, 3, /*tex_fmt=*/0, /*flags=*/0, 1, 1, 1, 1, /*count=*/1);
    appendInt32(buf, 17); // imageType = TARGA
    appendInt32(buf, 1);  // mipmap_count
    std::vector<uint8_t> tgaData(kRedPixelTga, kRedPixelTga + sizeof(kRedPixelTga));
    appendMipmapV2(buf, 1, 1, tgaData);

    VFS vfs;
    mountTex(vfs, "stbi_tga_v3", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("stbi_tga_v3");

    REQUIRE(img != nullptr);
    REQUIRE(img->slots.size() == 1);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    auto& mip = img->slots[0].mipmaps[0];
    CHECK(mip.width  == 1);
    CHECK(mip.height == 1);
    // src_size is set to w*h*4 after stbi decode — mutation w*h*3 would give 3
    CHECK(mip.size == 4);
    // stbi fills alpha=255 for opaque RGB sources; pixel order is RGBA
    REQUIRE(mip.data.get() != nullptr);
    CHECK(mip.data.get()[0] == 255); // R
    CHECK(mip.data.get()[1] == 0);   // G
    CHECK(mip.data.get()[2] == 0);   // B
    CHECK(mip.data.get()[3] == 255); // A (stbi adds opaque alpha for 24-bit source)
}

TEST_CASE("TEXB v3 stbi path skipped when imageType is UNKNOWN") {
    // imageType = UNKNOWN (-1) → raw-copy path, size stays at sizeof(TGA)=21
    // Kills the != UNKNOWN → == UNKNOWN mutant on the branch condition.
    auto buf = makeTexHeader(1, 1, 3, /*tex_fmt=*/0, /*flags=*/0, 1, 1, 1, 1, /*count=*/1);
    appendInt32(buf, -1); // imageType = UNKNOWN
    appendInt32(buf, 1);
    std::vector<uint8_t> tgaData(kRedPixelTga, kRedPixelTga + sizeof(kRedPixelTga));
    appendMipmapV2(buf, 1, 1, tgaData);

    VFS vfs;
    mountTex(vfs, "stbi_unknown_v3", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("stbi_unknown_v3");

    REQUIRE(img != nullptr);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    auto& mip = img->slots[0].mipmaps[0];
    // Raw path: size = sizeof(TGA) = 21, not 4
    CHECK(mip.size == (int32_t)sizeof(kRedPixelTga));
}

TEST_CASE("TEXB v2 stbi path never triggered — raw copy always used") {
    // texb=2: condition texb>=3 is false even though imageType field is absent.
    // Raw data kept as-is; size = sizeof(TGA) = 21.
    auto buf = makeTexHeader(1, 1, 2, /*tex_fmt=*/0, /*flags=*/0, 1, 1, 1, 1, /*count=*/1);
    // No imageType field for texb=2
    appendInt32(buf, 1);
    std::vector<uint8_t> tgaData(kRedPixelTga, kRedPixelTga + sizeof(kRedPixelTga));
    appendMipmapV2(buf, 1, 1, tgaData);

    VFS vfs;
    mountTex(vfs, "stbi_v2_raw", std::move(buf));
    WPTexImageParser parser(&vfs);
    auto img = parser.Parse("stbi_v2_raw");

    REQUIRE(img != nullptr);
    REQUIRE(img->slots[0].mipmaps.size() == 1);
    auto& mip = img->slots[0].mipmaps[0];
    // Raw path: size = 21
    CHECK(mip.size == (int32_t)sizeof(kRedPixelTga));
}

} // TEST_SUITE
