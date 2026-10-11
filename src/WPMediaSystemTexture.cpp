#include "WPMediaSystemTexture.hpp"

#include <cstring>

#include "Image.hpp"
#include "SpecTexs.hpp"
#include "WPTexImageParser.hpp"
#include "Scene/include/Scene/Scene.h"
#include "Utils/Logging.h"

namespace wallpaper
{

std::shared_ptr<Image> CopyImageMip0(const Image& src, const std::string& newKey) {
    auto img    = std::make_shared<Image>();
    img->key    = newKey;
    img->header = src.header;

    if (src.slots.empty() || src.slots[0].mipmaps.empty()) return img;
    const auto& srcSlot = src.slots[0];
    const auto& srcMip  = srcSlot.mipmaps[0];

    Image::Slot slot;
    slot.width  = srcSlot.width;
    slot.height = srcSlot.height;

    ImageData mip;
    mip.width  = srcMip.width;
    mip.height = srcMip.height;
    mip.size   = srcMip.size;
    if (srcMip.size > 0 && srcMip.data) {
        auto* buf = new uint8_t[(usize)srcMip.size];
        std::memcpy(buf, srcMip.data.get(), (usize)srcMip.size);
        mip.data = ImageDataPtr(buf, [](uint8_t* p) {
            delete[] p;
        });
    }
    slot.mipmaps.push_back(std::move(mip));
    img->slots.push_back(std::move(slot));
    return img;
}

std::shared_ptr<Image> MakeTransparentMediaPlaceholder(const std::string& key, i32 size) {
    std::vector<uint8_t> pixels((usize)size * (usize)size * 4, 0);
    return BuildImageFromRgba8888(pixels.data(), (isize)pixels.size(), size, size, key, {});
}

std::string ResolveMediaSystemTexture(const std::string& systemName,
                                      const std::string& originalTexName, Scene* pScene) {
    if (! IsKnownMediaSystemTexture(systemName)) return originalTexName;

    // Idempotent per scene load: LoadMaterial runs once per pass, and several
    // passes across the scene can reference the same system slot.
    for (const auto& slot : pScene->mediaTextureSlots)
        if (slot.textureKey == systemName) return systemName;

    auto* imgParser = dynamic_cast<WPTexImageParser*>(pScene->imageParser.get());
    if (! imgParser) {
        // No known IImageParser implementation other than WPTexImageParser
        // exists today; this is a defensive bail-out, not a reachable path.
        LOG_ERROR("media system texture '%s': image parser has no RegisterImage, "
                  "leaving the pass on its authored texture",
                  systemName.c_str());
        return originalTexName;
    }

    std::shared_ptr<Image> fallback =
        originalTexName.empty() ? nullptr : pScene->imageParser->Parse(originalTexName);

    std::shared_ptr<Image> registered;
    i32                    w, h;
    TextureSample          sample;
    if (fallback && ! fallback->slots.empty()) {
        registered = CopyImageMip0(*fallback, systemName);
        w          = fallback->slots[0].width;
        h          = fallback->slots[0].height;
        sample     = fallback->header.sample;
    } else {
        // Nothing authored to preserve the look of. Registering the
        // renderer's ordinary 1x1 "dangling texture" fallback here (as
        // opposed to everywhere else it's used) would crush every real
        // thumbnail arriving later down to a single pixel forever, since
        // this slot's own size becomes the resize target for all of them --
        // use a fixed, non-degenerate, transparent placeholder instead.
        LOG_INFO("media system texture '%s': fallback texture '%s' unavailable, "
                 "using a transparent %dx%d placeholder",
                 systemName.c_str(),
                 originalTexName.c_str(),
                 kMediaSystemTextureFallbackSize,
                 kMediaSystemTextureFallbackSize);
        w          = kMediaSystemTextureFallbackSize;
        h          = kMediaSystemTextureFallbackSize;
        registered = MakeTransparentMediaPlaceholder(systemName, w);
        sample     = registered->header.sample;
    }

    // The pass gets its own copy: TextureCache::CreateTex frees the CPU pixels
    // of whatever it uploads, and the slot's image is the "no art" restore
    // source for the life of the scene.
    imgParser->RegisterImage(systemName, CopyImageMip0(*registered, systemName));
    pScene->mediaTextureSlots.push_back(MediaTextureSlot { systemName, w, h, sample, registered });
    return systemName;
}

std::shared_ptr<Image> BuildImageFromRgba8888(const uint8_t* bytes, isize byteCount, i32 width,
                                              i32 height, const std::string& key,
                                              const TextureSample& sample) {
    auto img              = std::make_shared<Image>();
    img->key              = key;
    img->header.width     = width;
    img->header.height    = height;
    img->header.mapWidth  = width;
    img->header.mapHeight = height;
    img->header.format    = TextureFormat::RGBA8;
    img->header.count     = 1;
    img->header.sample    = sample;

    Image::Slot slot;
    slot.width  = width;
    slot.height = height;

    ImageData mip;
    mip.width  = width;
    mip.height = height;
    mip.size   = byteCount;
    if (byteCount > 0 && bytes) {
        auto* buf = new uint8_t[(usize)byteCount];
        std::memcpy(buf, bytes, (usize)byteCount);
        mip.data = ImageDataPtr(buf, [](uint8_t* p) {
            delete[] p;
        });
    }
    slot.mipmaps.push_back(std::move(mip));
    img->slots.push_back(std::move(slot));
    return img;
}

ImageShapeForTesting InspectImageForTesting(const std::shared_ptr<Image>& img) {
    ImageShapeForTesting out;
    if (! img || img->slots.empty() || img->slots[0].mipmaps.empty()) return out;
    out.ok          = true;
    out.width       = img->slots[0].width;
    out.height      = img->slots[0].height;
    const auto& mip = img->slots[0].mipmaps[0];
    if (mip.data && mip.size >= 4) {
        const uint8_t* p = mip.data.get();
        out.r            = p[0];
        out.g            = p[1];
        out.b            = p[2];
        out.a            = p[3];
    }
    return out;
}

} // namespace wallpaper
