#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "Core/Literals.hpp"
#include "SpecTexs.hpp"
#include "Type.hpp"

namespace wallpaper
{

// Forward-declared, not #include "Image.hpp": this header is reached from
// SceneBackend.hpp (qml_helper/), and Qt's <QtCore/qobjectdefs.h> #defines
// `slots` away wherever a Qt header has already been included in the same
// translation unit -- Image::slots would then silently stop parsing as a
// member declaration (see memory/qt-slots-keyword-breaks-scene-headers.md).
// Every declaration below only needs Image by pointer/reference/shared_ptr,
// none of which require it complete; only the .cpp (never Qt-included)
// touches its members.
struct Image;
class Scene;

// True for the fixed set of "$..." names a material's usertextures entry can
// use to ask for a live-updated system texture instead of its authored
// static one (today: current/previous track album art). One place to grow
// the allow-list when a third slot shows up, instead of a new special case
// scattered across the parser and the render graph. Inline (like IsSpecTex
// in SpecTexs.hpp) so CustomShaderPass.cpp's mutable-texture check can call
// it without pulling wescene-renderer into wpVulkanRender's link.
inline bool IsKnownMediaSystemTexture(std::string_view name) {
    return name == WE_MEDIA_THUMBNAIL || name == WE_MEDIA_PREVIOUS_THUMBNAIL;
}

// Size used to register a media system-texture slot when there is no real
// fallback texture to preserve the look of (an empty name, or a name that
// fails to decode). MediaTextureSlot::width/height doubles as the resize
// target for every real thumbnail arriving later, so this must be a real,
// non-degenerate size -- registering the renderer's ordinary 1x1 "dangling
// texture" fallback here would crush every subsequent real thumbnail down to
// a single pixel forever, not just this "nothing to show yet" placeholder.
constexpr i32 kMediaSystemTextureFallbackSize = 256;

// A `size x size` fully transparent image, built the same way a resized
// real thumbnail would be (BuildImageFromRgba8888), so its dimensions are
// real pixel data rather than a rescaled label on a smaller buffer.
std::shared_ptr<Image> MakeTransparentMediaPlaceholder(const std::string& key, i32 size);

// Deep-copy `src`'s first slot's mip 0 under `newKey`. Image is NoCopy/NoMove
// and TextureCache keys its cache by Image::key, not by whatever name Parse()
// was called with -- aliasing the fallback's own Image object would make the
// media slot silently share a VkImage with the fallback texture (or fail
// ReuploadTex outright, since the fallback's own key never matches the new
// one). Copies header.sample too: that's the field TextureCache::CreateTex
// actually reads for the GPU sampler, not SceneTexture.sample.
std::shared_ptr<Image> CopyImageMip0(const Image& src, const std::string& newKey);

// Called from LoadMaterial's texture loop when a pass's usertextures entry
// names a system slot. `originalTexName` is the fallback texture the slot
// would otherwise use (the pass's own `textures[i]`, e.g. the wallpaper's
// static "SS" cover art). Registers a copy of that fallback under
// `systemName` on first sight (idempotent per scene load -- a repeat call
// for the same name is a cache hit against Scene::mediaTextureSlots, not a
// re-decode) and records its size and sample settings there, so the runtime
// resize path knows what to scale incoming album art to.
//
// Returns `systemName` when resolved (the caller substitutes it for the
// slot's texture reference), or `originalTexName` unchanged when
// `systemName` isn't a recognized media slot -- the pass keeps its authored
// texture exactly as before.
std::string ResolveMediaSystemTexture(const std::string& systemName,
                                      const std::string& originalTexName, Scene* pScene);

// Build a one-slot, one-mip Image from already-resized/converted RGBA8888
// pixel bytes. No Qt dependency, unlike the QImage resize/convert step that
// produces those bytes (SceneBackend.cpp owns that half and hands the raw
// buffer here) -- this is the boundary that keeps QImage and Image::slots
// out of the same translation unit.
std::shared_ptr<Image> BuildImageFromRgba8888(const uint8_t* bytes, isize byteCount, i32 width,
                                              i32 height, const std::string& key,
                                              const TextureSample& sample);

// Test-only: a Qt-linked test binary can't touch Image::slots directly (same
// reason as above), so it reads a media-texture upload's shape/pixel through
// this instead of member access.
struct ImageShapeForTesting {
    bool    ok { false };
    i32     width { 0 };
    i32     height { 0 };
    uint8_t r { 0 }, g { 0 }, b { 0 }, a { 0 }; // first pixel, RGBA8888
};
ImageShapeForTesting InspectImageForTesting(const std::shared_ptr<Image>& img);

} // namespace wallpaper
