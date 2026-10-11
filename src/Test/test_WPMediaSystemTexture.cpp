#include <doctest.h>

#include "WPMediaSystemTexture.hpp"
#include "Image.hpp"
#include "SpecTexs.hpp"
#include "WPTexImageParser.hpp"
#include "ImagePayload.h"
#include "Scene/include/Scene/Scene.h"
#include "Fs/VFS.h"
#include "Fs/MemBinaryStream.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace wallpaper;

namespace
{

// Minimal in-memory Fs -- same shape as test_WPSceneParse.cpp's MemFs.
class MemFs : public fs::Fs {
public:
    void add(std::string path, std::vector<uint8_t> data) {
        m_files[std::move(path)] = std::move(data);
    }
    bool Contains(std::string_view path) const override {
        return m_files.count(std::string(path)) > 0;
    }
    std::shared_ptr<fs::IBinaryStream> Open(std::string_view path) override {
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        auto copy = it->second;
        return std::make_shared<fs::MemBinaryStream>(std::move(copy));
    }
    std::shared_ptr<fs::IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::unordered_map<std::string, std::vector<uint8_t>> m_files;
};

void append(std::vector<uint8_t>& buf, const void* data, size_t n) {
    auto p = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), p, p + n);
}
void appendInt32(std::vector<uint8_t>& buf, int32_t v) { append(buf, &v, 4); }
void appendUint32(std::vector<uint8_t>& buf, uint32_t v) { append(buf, &v, 4); }
void appendTexVersion(std::vector<uint8_t>& buf, int ver) {
    char hdr[9] {};
    std::snprintf(hdr, sizeof(hdr), "%.4s%.4d", "TEX", ver);
    append(buf, hdr, 9);
}

// Minimal single-mip RGBA8 .tex payload (texb=1, no LZ4, no sprite) -- same
// shape as test_WPTexImageParser.cpp's makeSimpleRGBA8Tex, duplicated locally
// since that helper isn't header-exported. `flags=2` sets WPTexFlagEnum::
// clampUVs so the decoded header.sample differs from a default-constructed
// TextureSample in both wrap and filter, proving a real copy happened rather
// than a value that merely matches the struct's defaults by coincidence.
std::vector<uint8_t> makeRGBA8Tex(int32_t w, int32_t h) {
    std::vector<uint8_t> buf;
    appendTexVersion(buf, 1); // TEXV
    appendTexVersion(buf, 1); // TEXI
    appendInt32(buf, 0);      // type = RGBA8
    appendUint32(buf, 2);     // flags: clampUVs set, noInterpolation unset
    appendInt32(buf, w);
    appendInt32(buf, h);
    appendInt32(buf, w);      // mapWidth
    appendInt32(buf, h);      // mapHeight
    appendInt32(buf, 0);      // unknown
    appendTexVersion(buf, 1); // TEXB
    appendInt32(buf, 1);      // image count
    appendInt32(buf, 1);      // mipmap count
    appendInt32(buf, w);
    appendInt32(buf, h);
    std::vector<uint8_t> pixels((size_t)(w * h * 4), 0xAB);
    appendInt32(buf, (int32_t)pixels.size());
    append(buf, pixels.data(), pixels.size());
    return buf;
}

// Builds a VFS + WPTexImageParser pair with one real, non-square fallback
// texture mounted under `fallbackName`. Non-square catches a width/height
// swap that a square fixture couldn't.
struct Fixture {
    fs::VFS                           vfs;
    std::unique_ptr<WPTexImageParser> parser;

    explicit Fixture(const std::string& fallbackName, int32_t w = 64, int32_t h = 32) {
        auto memfs = std::make_unique<MemFs>();
        memfs->add("/materials/" + fallbackName + ".tex", makeRGBA8Tex(w, h));
        REQUIRE(vfs.Mount("/assets", std::move(memfs)));
        parser = std::make_unique<WPTexImageParser>(&vfs);
    }
};

} // namespace

TEST_SUITE("IsKnownMediaSystemTexture") {
    TEST_CASE("both real names are recognized") {
        CHECK(IsKnownMediaSystemTexture("$mediaThumbnail"));
        CHECK(IsKnownMediaSystemTexture("$mediaPreviousThumbnail"));
    }
    TEST_CASE("unrelated names are not") {
        CHECK_FALSE(IsKnownMediaSystemTexture("usertexture2"));
        CHECK_FALSE(IsKnownMediaSystemTexture(""));
        CHECK_FALSE(IsKnownMediaSystemTexture("$mediaSomethingElse"));
    }
}

TEST_SUITE("CopyImageMip0") {
    TEST_CASE("copies bytes and key under a new key, without aliasing the source") {
        Image src;
        src.key           = "SS";
        src.header.sample = { TextureWrap::CLAMP_TO_EDGE,
                              TextureWrap::CLAMP_TO_EDGE,
                              TextureFilter::LINEAR,
                              TextureFilter::LINEAR };
        Image::Slot slot;
        slot.width  = 2;
        slot.height = 2;
        ImageData mip;
        mip.width  = 2;
        mip.height = 2;
        mip.size   = 16;
        auto* raw  = new uint8_t[16];
        for (int i = 0; i < 16; i++) raw[i] = (uint8_t)i;
        mip.data = ImageDataPtr(raw, [](uint8_t* p) {
            delete[] p;
        });
        slot.mipmaps.push_back(std::move(mip));
        src.slots.push_back(std::move(slot));

        auto copy = CopyImageMip0(src, "$mediaThumbnail");
        REQUIRE(copy);
        CHECK(copy->key == "$mediaThumbnail");
        REQUIRE(copy->slots.size() == 1);
        REQUIRE(copy->slots[0].mipmaps.size() == 1);
        CHECK(copy->slots[0].mipmaps[0].size == 16);
        CHECK(std::memcmp(copy->slots[0].mipmaps[0].data.get(),
                          src.slots[0].mipmaps[0].data.get(),
                          16) == 0);
        // header.sample must be copied -- this is the field TextureCache::CreateTex
        // actually reads, not SceneTexture.sample.
        CHECK(copy->header.sample.wrapS == TextureWrap::CLAMP_TO_EDGE);
        CHECK(copy->header.sample.magFilter == TextureFilter::LINEAR);

        // Mutating the copy must not touch the source (regression guard
        // against aliasing the same underlying buffer).
        copy->slots[0].mipmaps[0].data.get()[0] = 0xFF;
        CHECK(src.slots[0].mipmaps[0].data.get()[0] == 0);
    }
}

TEST_SUITE("ResolveMediaSystemTexture") {
    TEST_CASE("unrecognized name returns the original unchanged, no registration") {
        Fixture fixture("SS");
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());
        auto out = ResolveMediaSystemTexture("usertexture2", "SS", &scene);
        CHECK(out == "SS");
        CHECK(scene.mediaTextureSlots.empty());
    }

    TEST_CASE("recognized name resolves to the system key and records real fallback dimensions") {
        Fixture fixture("SS", 64, 32);
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        auto out = ResolveMediaSystemTexture("$mediaThumbnail", "SS", &scene);
        CHECK(out == "$mediaThumbnail");

        REQUIRE(scene.mediaTextureSlots.size() == 1);
        const auto& slot = scene.mediaTextureSlots[0];
        CHECK(slot.textureKey == "$mediaThumbnail");
        CHECK(slot.width == 64);
        CHECK(slot.height == 32);
        CHECK(slot.sample.wrapS == TextureWrap::CLAMP_TO_EDGE);
        CHECK(slot.sample.magFilter == TextureFilter::LINEAR);
        REQUIRE(slot.placeholder);
        CHECK(slot.placeholder->key == "$mediaThumbnail");

        // Load-bearing: Parse(systemName) afterward must return the copy
        // registered under the SYSTEM key, not the fallback's own "SS" image.
        auto parsed = scene.imageParser->Parse("$mediaThumbnail");
        REQUIRE(parsed);
        CHECK(parsed->key == "$mediaThumbnail");
        REQUIRE(! parsed->slots.empty());
        CHECK(parsed->slots[0].width == 64);
        CHECK(parsed->slots[0].height == 32);

        // "SS" itself must still resolve to its own real texture (a second,
        // unrelated non-system reference to the same fallback name exists on
        // this wallpaper -- it must not get silently rebound).
        auto ss = scene.imageParser->Parse("SS");
        REQUIRE(ss);
        CHECK(ss->key == "SS");
    }

    // No authored appearance to preserve when the wallpaper's own fallback
    // texture is itself missing/undecodable. MediaTextureSlot::width/height
    // doubles as the resize target for every real thumbnail that arrives
    // later (SceneBackend.cpp's BuildResizedSystemImage) -- registering the
    // renderer's ordinary 1x1 "dangling texture" fallback here would crush
    // every future real thumbnail down to a single pixel forever, not just
    // this "nothing to show yet" placeholder. Use a fixed, non-degenerate
    // size instead, filled transparent so there's nothing to look at until
    // real art arrives.
    TEST_CASE("empty fallback name falls back to a non-degenerate transparent placeholder") {
        Fixture fixture("SS");
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        auto out = ResolveMediaSystemTexture("$mediaThumbnail", "", &scene);
        CHECK(out == "$mediaThumbnail");
        REQUIRE(scene.mediaTextureSlots.size() == 1);
        CHECK(scene.mediaTextureSlots[0].width == kMediaSystemTextureFallbackSize);
        CHECK(scene.mediaTextureSlots[0].height == kMediaSystemTextureFallbackSize);
        auto shape = InspectImageForTesting(scene.mediaTextureSlots[0].placeholder);
        REQUIRE(shape.ok);
        CHECK(shape.width == kMediaSystemTextureFallbackSize);
        CHECK(shape.height == kMediaSystemTextureFallbackSize);
        CHECK(shape.a == 0);

        // Load-bearing: a real thumbnail arriving later resizes to THIS
        // slot's size, so it must not have been crushed to 1x1.
        auto parsed = scene.imageParser->Parse("$mediaThumbnail");
        REQUIRE(parsed);
        REQUIRE(! parsed->slots.empty());
        CHECK(parsed->slots[0].width == kMediaSystemTextureFallbackSize);
        CHECK(parsed->slots[0].height == kMediaSystemTextureFallbackSize);
    }

    TEST_CASE("a fallback name that fails to decode falls back to a non-degenerate transparent "
              "placeholder") {
        Fixture fixture("SS");
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        auto out = ResolveMediaSystemTexture("$mediaThumbnail", "does-not-exist", &scene);
        CHECK(out == "$mediaThumbnail");
        REQUIRE(scene.mediaTextureSlots.size() == 1);
        CHECK(scene.mediaTextureSlots[0].width == kMediaSystemTextureFallbackSize);
        CHECK(scene.mediaTextureSlots[0].height == kMediaSystemTextureFallbackSize);
    }

    // TextureCache::CreateTex frees a texture's CPU pixels once they are on the
    // GPU. The slot's placeholder is what a track with no art restores later,
    // so it must survive that: on 3616389236 every artless track tried to
    // upload a 0-byte image (VK_ERROR_INITIALIZATION_FAILED on the staging
    // buffer) because the placeholder WAS the registered image.
    TEST_CASE("the texture cache freeing the registered image leaves the placeholder's pixels") {
        Fixture fixture("SS", 64, 32);
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        ResolveMediaSystemTexture("$mediaThumbnail", "SS", &scene);
        REQUIRE(scene.mediaTextureSlots.size() == 1);

        // What CreateTex does with the image LoadMaterial parses for the pass.
        auto registered = scene.imageParser->Parse("$mediaThumbnail");
        REQUIRE(registered);
        if (mayReleaseDecodedPayload(*registered)) releaseDecodedPayload(*registered);

        auto shape = InspectImageForTesting(scene.mediaTextureSlots[0].placeholder);
        REQUIRE(shape.ok);
        CHECK(shape.width == 64);
        CHECK(shape.height == 32);
        const auto& mip = scene.mediaTextureSlots[0].placeholder->slots[0].mipmaps[0];
        CHECK(mip.size == 64 * 32 * 4);
        CHECK(mip.data != nullptr);
    }

    TEST_CASE("the transparent fallback placeholder also survives the cache's release") {
        Fixture fixture("SS");
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        ResolveMediaSystemTexture("$mediaThumbnail", "", &scene);
        auto registered = scene.imageParser->Parse("$mediaThumbnail");
        REQUIRE(registered);
        if (mayReleaseDecodedPayload(*registered)) releaseDecodedPayload(*registered);

        const auto& mip = scene.mediaTextureSlots[0].placeholder->slots[0].mipmaps[0];
        CHECK(mip.size ==
              (isize)kMediaSystemTextureFallbackSize * kMediaSystemTextureFallbackSize * 4);
        CHECK(mip.data != nullptr);
    }

    TEST_CASE("calling it twice for the same name is idempotent") {
        Fixture fixture("SS");
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        ResolveMediaSystemTexture("$mediaThumbnail", "SS", &scene);
        ResolveMediaSystemTexture("$mediaThumbnail", "SS", &scene);
        CHECK(scene.mediaTextureSlots.size() == 1);
    }

    TEST_CASE("two different recognized names sharing one fallback register independently") {
        Fixture fixture("SS", 64, 32);
        Scene   scene;
        scene.imageParser.reset(fixture.parser.release());

        ResolveMediaSystemTexture("$mediaThumbnail", "SS", &scene);
        ResolveMediaSystemTexture("$mediaPreviousThumbnail", "SS", &scene);
        REQUIRE(scene.mediaTextureSlots.size() == 2);
        CHECK(scene.mediaTextureSlots[0].textureKey == "$mediaThumbnail");
        CHECK(scene.mediaTextureSlots[1].textureKey == "$mediaPreviousThumbnail");
        CHECK(scene.mediaTextureSlots[0].placeholder != scene.mediaTextureSlots[1].placeholder);
    }
}

TEST_SUITE("BuildImageFromRgba8888 / InspectImageForTesting") {
    TEST_CASE("builds a one-slot, one-mip image with the given key/size/sample/bytes") {
        std::vector<uint8_t> px = { 10, 20, 30, 255, 40, 50, 60, 255 }; // 2x1 RGBA8888
        TextureSample        sample { TextureWrap::CLAMP_TO_EDGE,
                                      TextureWrap::CLAMP_TO_EDGE,
                                      TextureFilter::LINEAR,
                                      TextureFilter::LINEAR };

        auto img =
            BuildImageFromRgba8888(px.data(), (isize)px.size(), 2, 1, "$mediaThumbnail", sample);
        REQUIRE(img);
        CHECK(img->key == "$mediaThumbnail");
        CHECK(img->header.sample.wrapS == TextureWrap::CLAMP_TO_EDGE);
        REQUIRE(img->slots.size() == 1);
        CHECK(img->slots[0].width == 2);
        CHECK(img->slots[0].height == 1);
        REQUIRE(img->slots[0].mipmaps.size() == 1);
        CHECK(img->slots[0].mipmaps[0].size == (isize)px.size());
        CHECK(std::memcmp(img->slots[0].mipmaps[0].data.get(), px.data(), px.size()) == 0);
    }

    TEST_CASE("InspectImageForTesting reports the same shape/first pixel without member access") {
        std::vector<uint8_t> px = { 200, 40, 40, 255, 0, 0, 0, 0 };
        auto img = BuildImageFromRgba8888(px.data(), (isize)px.size(), 2, 1, "k", {});

        auto shape = InspectImageForTesting(img);
        CHECK(shape.ok);
        CHECK(shape.width == 2);
        CHECK(shape.height == 1);
        CHECK(shape.r == 200);
        CHECK(shape.g == 40);
        CHECK(shape.b == 40);
        CHECK(shape.a == 255);
    }

    TEST_CASE("InspectImageForTesting on a null image reports not-ok") {
        auto shape = InspectImageForTesting(nullptr);
        CHECK_FALSE(shape.ok);
    }
}
