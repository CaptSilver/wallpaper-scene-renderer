#include "WPTexImageParser.hpp"

#include "Type.hpp"
#include "WPTexImageHelpers.h"
#include "WPCommon.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <lz4.h>

#include "SpriteAnimation.hpp"
#include "Utils/Algorism.h"
#include "Utils/SceneProfiler.h"
#include "Fs/VFS.h"
#include "Utils/BitFlags.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

using namespace wallpaper;

enum class WPTexFlagEnum : uint32_t
{
    // true for no bilinear
    noInterpolation = 0,
    // true for no repeat
    clampUVs = 1,
    sprite   = 2,

    compo1 = 20,
    compo2 = 21,
    compo3 = 22
};
using WPTexFlags = BitFlags<WPTexFlagEnum>;

namespace
{

using namespace wallpaper::teximage_helpers;

// Hostile-input bounds for Parse().  Wallpaper Engine ships textures up to 4K
// per side with ≤13 mips and ≤6 slots (cube faces) in practice; the three
// plausibility caps right below (image count, mipmap count, per-axis mip
// dimension) sit well above any real .tex while keeping fuzz inputs from
// declaring 80GB image_count, INT32_MAX dimensions, or thousands of mip
// levels. The cumulative byte budget further down is a different kind of
// cap -- see the comment above it -- since a real texture can get close to
// that one.
//
// Surfaced by fuzz_WPTexImageParser pathologies — a 105-byte hostile .tex
// embedding a TGA with declared dims 10000×10000 routes through
// stbi_load_from_memory and allocates ~716MB before failing on truncated input;
// a many-mip header with each mip declaring decompressed_size near the 256MB
// per-allocation cap accumulates total RSS across slot.mipmaps[].data without
// bound.
//
// Each LOG_INFO names the field and the rejected value so journal forensics on
// a refused texture localises the bad header field.
constexpr usize kMaxImageCount   = 16;
constexpr usize kMaxMipmapCount  = 24;
constexpr i32   kMaxMipmapDim    = 16384;
// Cumulative texture budget: the host-heap bytes one .tex may keep resident
// in slot.mipmaps[].data while Parse() decodes it from untrusted workshop
// content. Unlike the plausibility caps above, this bounds accumulation
// across many mips rather than rejecting one bad field outright -- it's a
// fuzz-derived DoS margin, not an estimate of how big a real texture gets.
//
// Default is 2 GiB: large multi-frame sprites exist (e.g. Hackercore's
// 7680x8000 sheet), and the old hard 1 GiB cap dropped them to a 1x1
// fallback (a blank wallpaper) instead of just costing more memory. The
// 256 MB per-allocation cap below still bounds any single mip, but a
// texture built from many mips near that cap can still run the cumulative
// total up to this budget -- accepted, since rejecting it would mean
// blanking a real wallpaper.
//
// Two independent ways to change it, matching how every other WEKDE_*/WEK_*
// knob in this codebase splits runtime vs. build time: WEKDE_MAX_TEX_BYTES
// overrides it per run (DefaultMaxTexBytes(), read fresh whenever a
// WPTexImageParser is constructed without an explicit budget);
// WEK_MAX_TEX_BYTES overrides the compiled-in default for packagers who want
// to ship a different one.
#ifndef WEK_MAX_TEX_BYTES
#  define WEK_MAX_TEX_BYTES (2ll * 1024 * 1024 * 1024)
#endif
constexpr i64 kMaxTotalBytes = WEK_MAX_TEX_BYTES;
constexpr i32   kMaxEmbeddedDim  = 16384;

std::vector<char> Lz4Decompress(const char* src, int size, int decompressed_size) {
    std::vector<char> dst((usize)decompressed_size);
    int               load_size = LZ4_decompress_safe(src, dst.data(), size, decompressed_size);
    if (load_size < decompressed_size) {
        LOG_ERROR("lz4 decompress failed");
        return {};
    }
    return dst;
}
void LoadHeader(fs::IBinaryStream& file, ImageHeader& header) {
    header.extraHeader["texv"].val = ReadTexVesion(file);
    header.extraHeader["texi"].val = ReadTexVesion(file);

    header.format = ToTexFormate(file.ReadInt32());
    WPTexFlags flags(file.ReadUint32());
    {
        header.isSprite     = flags[WPTexFlagEnum::sprite];
        header.sample.wrapS = header.sample.wrapT =
            flags[WPTexFlagEnum::clampUVs] ? TextureWrap::CLAMP_TO_EDGE : TextureWrap::REPEAT;
        header.sample.minFilter = header.sample.magFilter =
            flags[WPTexFlagEnum::noInterpolation] ? TextureFilter::NEAREST : TextureFilter::LINEAR;
        header.extraHeader["compo1"].val = flags[WPTexFlagEnum::compo1];
        header.extraHeader["compo2"].val = flags[WPTexFlagEnum::compo2];
        header.extraHeader["compo3"].val = flags[WPTexFlagEnum::compo3];
    }

    /*
        picture:
        width, height --> pow of 2 (tex size)
        mapw, maph    --> pic size
        mips
        mipw,miph     --> pow of 2

        sprites:
        width, height --> piece of sprite sheet
        mapw, maph    --> same
        1 mip
        mipw,mimp     --> tex size
    */

    header.width  = file.ReadInt32();
    header.height = file.ReadInt32();
    // in sprite this mean one pic
    header.mapWidth  = file.ReadInt32();
    header.mapHeight = file.ReadInt32();

    file.ReadInt32(); // unknown

    header.extraHeader["texb"].val = ReadTexVesion(file);

    header.count = file.ReadInt32();

    if (header.extraHeader["texb"].val >= 3) header.type = static_cast<ImageType>(file.ReadInt32());
    if (header.extraHeader["texb"].val >= 4) file.ReadInt32(); // isVideoMp4 flag, skip
}

inline ImageHeader MakeFallbackHeader() {
    ImageHeader header;
    header.width     = 1;
    header.height    = 1;
    header.mapWidth  = 1;
    header.mapHeight = 1;
    header.format    = TextureFormat::RGBA8;
    header.count     = 1;
    return header;
}

inline std::shared_ptr<Image> MakeFallbackImage(const std::string& name) {
    auto  img_ptr = std::make_shared<Image>();
    auto& img     = *img_ptr;
    img.key       = name;
    img.header    = MakeFallbackHeader();

    Image::Slot slot;
    slot.width  = 1;
    slot.height = 1;

    ImageData mipmap;
    mipmap.width  = 1;
    mipmap.height = 1;
    mipmap.size   = 4;
    mipmap.data   = ImageDataPtr(new uint8_t[4] { 255, 255, 255, 255 }, [](uint8_t* p) {
        delete[] p;
    });
    slot.mipmaps.push_back(std::move(mipmap));
    img.slots.push_back(std::move(slot));
    return img_ptr;
}

} // namespace

i64 wallpaper::DefaultMaxTexBytes() {
    // Per-call getenv(), no caching -- matches WEKDE_MSAA / WEKDE_DEBUG_FRAMETIME:
    // Parse() isn't hot enough for a getenv() to matter, and this way a test
    // (or an operator re-running with the var set) sees the new value without
    // a process restart.
    const char* env = std::getenv("WEKDE_MAX_TEX_BYTES");
    if (! env) return kMaxTotalBytes;
    char* end = nullptr;
    errno     = 0;
    long long v = std::strtoll(env, &end, 10);
    if (end == env || *end != '\0' || errno == ERANGE || v <= 0) {
        LOG_ERROR("WEKDE_MAX_TEX_BYTES=\"%s\" is not a positive byte count, "
                  "keeping the %lld-byte compile-time default",
                  env, (long long)kMaxTotalBytes);
        return kMaxTotalBytes;
    }
    return (i64)v;
}

void WPTexImageParser::RegisterImage(const std::string& key, std::shared_ptr<Image> img) {
    m_registered[key] = std::move(img);
}

std::shared_ptr<Image> WPTexImageParser::Parse(const std::string& name) {
    WEK_PROFILE_SCOPE("WPTexImageParser::Parse");
    if (auto it = m_registered.find(name); it != m_registered.end()) {
        return it->second;
    }
    std::string path = "/assets/materials/" + name + ".tex";
    if (IsAliasTexture(name) && ! m_vfs->Contains(path)) {
        LOG_INFO("using fallback 1x1 white texture for \"%s\"", name.c_str());
        return MakeFallbackImage(name);
    }
    std::shared_ptr<Image> img_ptr = std::make_shared<Image>();
    auto&                  img     = *img_ptr;
    img.key                        = name;
    auto pfile                     = m_vfs->Open(path);
    if (! pfile) return nullptr;
    auto& file     = *pfile;
    auto  startpos = file.Tell();
    LoadHeader(file, img.header);

    {
        LOG_INFO("tex '%s': %dx%d fmt=%s sprite=%d count=%d",
                 name.c_str(),
                 img.header.width,
                 img.header.height,
                 ToString(img.header.format).c_str(),
                 (int)img.header.isSprite,
                 img.header.count);
    }

    // image
    i32 _image_count = img.header.count;
    if (_image_count < 0) return nullptr;
    usize image_count = (usize)_image_count;

    if (image_count > kMaxImageCount) {
        LOG_INFO("tex '%s': image_count %zu exceeds plausibility cap %zu",
                 name.c_str(), image_count, kMaxImageCount);
        return nullptr;
    }
    if (! CountFitsStream(file, image_count)) {
        LOG_ERROR("tex '%s': image_count %zu exceeds stream", name.c_str(), image_count);
        return nullptr;
    }
    img.slots.resize(image_count);

    // Cumulative byte budget across every mip in every slot.  Caps the total
    // RSS a single Parse() can request — a per-allocation cap alone leaves the
    // door open to N slots × M mips × cap accumulating without bound.
    i64 total_bytes = 0;
    for (usize i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        usize mipmap_count = (usize)std::max<i32>(file.ReadInt32(), 0);
        if (mipmap_count > kMaxMipmapCount) {
            LOG_INFO("tex '%s' slot[%zu]: mipmap_count %zu exceeds plausibility cap %zu",
                     name.c_str(), i_image, mipmap_count, kMaxMipmapCount);
            return nullptr;
        }
        if (! CountFitsStream(file, mipmap_count)) {
            LOG_ERROR("tex '%s': mipmap_count %zu exceeds stream", name.c_str(), mipmap_count);
            return nullptr;
        }
        mipmaps.resize(mipmap_count);
        // load image
        for (usize i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
            auto& mipmap  = mipmaps.at(i_mipmap);
            mipmap.width  = file.ReadInt32();
            mipmap.height = file.ReadInt32();
            if (i_mipmap == 0) {
                img_slot.width  = mipmap.width;
                img_slot.height = mipmap.height;
                SetHeaderPow2(img.header, mipmap.width, mipmap.height);
            }

            bool    LZ4_compressed    = false;
            int32_t decompressed_size = 0;
            // check compress
            if (img.header.extraHeader["texb"].val > 1) {
                LZ4_compressed    = file.ReadInt32() == 1;
                decompressed_size = file.ReadInt32();
            }

            i32 src_size = file.ReadInt32();
            if (src_size <= 0 || mipmap.width <= 0 || mipmap.height <= 0 || decompressed_size < 0)
                return nullptr;

            // Per-axis plausibility cap.  Real WP textures top out at 4K per
            // side; 16K leaves room for future engine bumps while rejecting
            // the INT32_MAX class of fuzz inputs.
            if (mipmap.width > kMaxMipmapDim || mipmap.height > kMaxMipmapDim) {
                LOG_INFO("tex '%s' mip[%zu]: dimensions %dx%d exceed plausibility cap %d",
                         name.c_str(), i_mipmap,
                         mipmap.width, mipmap.height, kMaxMipmapDim);
                return nullptr;
            }

            // Cap any single-allocation byte count parsed from the .tex
            // header — both decompressed_size below (Lz4Decompress) and
            // raw_size in the MP4-placeholder path further down call
            // std::vector / std::make_unique with this value, and
            // libFuzzer's -malloc_limit_mb=512 trips on multi-hundred-MB
            // allocations.  256 MB sits well above any legitimate single
            // mip (8K × 8K × RGBA32F = 256 MB) and below the fuzz limit.
            // Surfaced by fuzz_WPTexImageParser OOM artifacts:
            //   oom-f282830d880e0ebc5ae123ee52c5fe2a20c1d163  (LZ4 path)
            //   oom-aa47057a0c3b665efa41381957e7a441c40a2fda  (MP4-fallback path)
            constexpr i64 kMaxTexAllocBytes = 256 * 1024 * 1024;
            if ((i64)decompressed_size > kMaxTexAllocBytes) {
                LOG_ERROR("tex '%s': decompressed_size %d exceeds %lld-byte cap",
                          name.c_str(),
                          decompressed_size,
                          (long long)kMaxTexAllocBytes);
                return nullptr;
            }

            if (! CountFitsStream(file, (usize)src_size)) {
                LOG_ERROR("tex '%s': src_size %d exceeds stream", name.c_str(), src_size);
                return nullptr;
            }
            std::vector<char> result((usize)src_size);
            file.Read(result.data(), (usize)src_size);

            // is LZ4 compress
            if (LZ4_compressed) {
                auto decompressed = Lz4Decompress(result.data(), src_size, decompressed_size);
                if (! decompressed.empty()) {
                    src_size = decompressed_size;
                    result   = std::move(decompressed);
                } else {
                    LOG_ERROR("lz4 decompress failed");
                    return nullptr;
                }
            }

            // `result` now holds exactly what this mip keeps if it takes the
            // raw-copy branch below: unchanged src_size if it wasn't
            // LZ4-compressed, or the reassigned src_size (== decompressed_size)
            // if it was -- the compressed buffer itself is gone, freed by the
            // std::move above. The embedded-image branch further down retains
            // stbi's own output buffer instead, so it gates on a different
            // value; the cumulative-budget check happens per-branch below
            // rather than once here, so a mip that's about to be re-decoded
            // into a smaller buffer doesn't get rejected for the container
            // bytes it's about to discard.
            i64 mip_bytes = (i64)src_size;

            // Detect MP4 video data misidentified as raw texture (isVideoMp4 flag not set).
            // MP4 containers start with a size field + "ftyp" signature.
            if (src_size > 8 && result.size() >= 8 &&
                std::memcmp(result.data() + 4, "ftyp", 4) == 0) {
                // Extract MP4 to temp file for video decoder
                std::string cacheDir = m_cachePath.empty()
                                           ? std::filesystem::temp_directory_path().string()
                                           : m_cachePath;
                std::string videoDir = cacheDir + "/video_tex";
                std::filesystem::create_directories(videoDir);
                // Sanitize name for filename
                std::string safeName = name;
                for (char& c : safeName) c = SanitizePathSeparatorChar(c);
                std::string videoPath = videoDir + "/" + safeName + ".mp4";
                {
                    std::ofstream f(videoPath, std::ios::binary);
                    if (f) {
                        f.write(result.data(), src_size);
                        LOG_INFO("tex '%s' mip[%zu]: extracted MP4 video (%d bytes) to %s",
                                 name.c_str(),
                                 i_mipmap,
                                 src_size,
                                 videoPath.c_str());
                    } else {
                        LOG_ERROR("tex '%s': failed to write video to %s",
                                  name.c_str(),
                                  videoPath.c_str());
                    }
                }
                img.header.isVideoTexture = true;
                img.header.videoFilePath  = videoPath;

                // Use black placeholder for initial texture upload
                i64 raw_size = Rgba8ByteSize(mipmap.width, mipmap.height);
                if (raw_size < 0) {
                    LOG_ERROR("tex '%s': mipmap dimensions %dx%d unsupported",
                              name.c_str(), mipmap.width, mipmap.height);
                    return nullptr;
                }
                if (raw_size > kMaxTexAllocBytes) {
                    LOG_ERROR("tex '%s': mp4 placeholder raw_size %lld (%dx%d) exceeds %lld-byte cap",
                              name.c_str(),
                              (long long)raw_size,
                              mipmap.width, mipmap.height,
                              (long long)kMaxTexAllocBytes);
                    return nullptr;
                }
                // The placeholder buffer is retained in mipmap.data exactly
                // like any other mip's, so it counts against the cumulative
                // budget the same way the raw-copy and stbi branches do below.
                if (total_bytes + raw_size > m_maxTotalBytes) {
                    LOG_INFO("tex '%s' mip[%zu]: cumulative retained bytes %lld + %lld "
                             "exceeds %lld-byte cap",
                             name.c_str(), i_mipmap,
                             (long long)total_bytes, (long long)raw_size,
                             (long long)m_maxTotalBytes);
                    return nullptr;
                }
                total_bytes += raw_size;

                auto buf = std::make_unique<uint8_t[]>((usize)raw_size);
                std::memset(buf.get(), 0, (usize)raw_size);
                mipmap.data = ImageDataPtr(buf.release(), [](uint8_t* p) {
                    delete[] p;
                });
                mipmap.size = raw_size;
                continue;
            }
            // is image container
            if (img.header.extraHeader["texb"].val >= 3 && img.header.type != ImageType::UNKNOWN) {
                // Pre-validate the embedded image's declared dimensions before
                // calling stbi_load_from_memory.  stbi allocates its output
                // buffer up front from the decoded image header (TGA/PNG/JPEG);
                // a hostile 10K×10K TGA inside an otherwise-empty .tex causes
                // ~716MB RSS + 7s of CPU before stbi errors on truncated
                // pixels.  Reject at the parser boundary instead.
                //
                // Well-formed WP textures store the embedded image at the same
                // dimensions as the mip container declared (mipmap.width/height)
                // — the parser later overwrites mipmap.size with w*h*4 from
                // stbi's output.  Reject if the embedded dims exceed the mip
                // container's, since that's the upper bound the parser would
                // honour anyway, or if either side exceeds the plausibility cap.
                int32_t info_w = 0, info_h = 0, info_n = 0;
                if (! stbi_info_from_memory((const unsigned char*)result.data(),
                                            src_size, &info_w, &info_h, &info_n) ||
                    info_w <= 0 || info_h <= 0 ||
                    info_w > kMaxEmbeddedDim || info_h > kMaxEmbeddedDim ||
                    info_w > mipmap.width || info_h > mipmap.height) {
                    LOG_INFO("tex '%s' mip[%zu]: embedded image dims %dx%d "
                             "outside mip container %dx%d / plausibility cap %d",
                             name.c_str(), i_mipmap,
                             info_w, info_h,
                             mipmap.width, mipmap.height,
                             kMaxEmbeddedDim);
                    return nullptr;
                }
                // This mip keeps stbi's own decoded buffer below, not
                // `result` (the container bytes get freed once stbi has its
                // own copy) -- gate on stbi's output size, not the container
                // size in mip_bytes, so a small image inside a padded/oversized
                // container isn't charged for bytes it doesn't keep.
                i64 stbi_out = (i64)info_w * info_h * 4;
                if (total_bytes + stbi_out > m_maxTotalBytes) {
                    LOG_INFO("tex '%s' mip[%zu]: stbi output %lld for %dx%d "
                             "exceeds cumulative cap %lld",
                             name.c_str(), i_mipmap,
                             (long long)stbi_out, info_w, info_h,
                             (long long)m_maxTotalBytes);
                    return nullptr;
                }
                total_bytes += stbi_out;

                int32_t w = 0, h = 0, n = 0;
                auto*   data = stbi_load_from_memory(
                    (const unsigned char*)result.data(), src_size, &w, &h, &n, 4);
                // stbi_info above only parses the container header, so it accepts
                // an image whose pixel stream is truncated or corrupt -- exactly
                // what a partially-downloaded workshop item carries.  The load can
                // still fail, and on failure stbi leaves w/h/n untouched, so
                // discarding this NULL stored a mip with no data and a size read
                // from uninitialised stack.  The Vulkan staging memcpy dereferenced
                // it later, far from the cause.
                if (data == nullptr) {
                    LOG_ERROR("tex '%s' mip[%zu]: embedded image failed to decode: %s",
                              name.c_str(),
                              i_mipmap,
                              stbi_failure_reason());
                    return nullptr;
                }
                mipmap.data = ImageDataPtr((uint8_t*)data, [](uint8_t* data) {
                    stbi_image_free((unsigned char*)data);
                });
                src_size    = w * h * 4;
            } else {
                // Plain raw-copy path: `result` (mip_bytes) is exactly what
                // gets retained in mipmap.data, so gate on it directly here,
                // before the allocation below.
                if (total_bytes + mip_bytes > m_maxTotalBytes) {
                    LOG_INFO("tex '%s' mip[%zu]: cumulative retained bytes %lld + %lld "
                             "exceeds %lld-byte cap",
                             name.c_str(), i_mipmap,
                             (long long)total_bytes, (long long)mip_bytes,
                             (long long)m_maxTotalBytes);
                    return nullptr;
                }
                total_bytes += mip_bytes;

                auto buf = std::make_unique<uint8_t[]>((usize)src_size);
                std::copy(result.data(), result.data() + src_size, buf.get());
                mipmap.data = ImageDataPtr(buf.release(), [](uint8_t* p) {
                    delete[] p;
                });
            }
            mipmap.size = src_size;
        }
    }
    // Self-cache the parsed image.  Without this every subsequent Parse(name)
    // re-opens the .tex file, re-decompresses all mipmaps, and (worst case
    // for video textures) re-writes the full MP4 payload to /tmp/video_tex/.
    // On Nightingale 3276911872 the same 19MB MP4 was being dumped 9+ times
    // during init, blocking the render thread long enough that the compositor
    // gave up and showed a black desktop.
    m_registered[name] = img_ptr;
    return img_ptr;
}

ImageHeader WPTexImageParser::ParseHeader(const std::string& name) {
    if (auto it = m_registered.find(name); it != m_registered.end()) {
        return it->second->header;
    }
    // Header-only cache: serves repeated ParseHeader calls on the same instance
    // without re-opening the .tex file (e.g. multiple WPSceneParser sites
    // querying the same autosize texture during one scene load).
    if (auto it = m_headerCache.find(name); it != m_headerCache.end()) {
        return it->second;
    }
    ImageHeader header;
    std::string path = "/assets/materials/" + name + ".tex";
    if (IsAliasTexture(name) && ! m_vfs->Contains(path)) {
        return MakeFallbackHeader();
    }
    auto pfile = m_vfs->Open(path);
    if (! pfile) return header;
    auto& file = *pfile;

    LoadHeader(file, header);
    if (header.count < 0) return header;

    usize image_count = (usize)header.count;

    // load sprite info
    if (header.isSprite) {
        // bypass image data, store width and height
        std::vector<std::vector<float>> imageDatas(image_count);
        for (usize i_image = 0; i_image < image_count; i_image++) {
            int mipmap_count = file.ReadInt32();
            for (int32_t i_mipmap = 0; i_mipmap < mipmap_count; i_mipmap++) {
                int32_t width  = file.ReadInt32();
                int32_t height = file.ReadInt32();
                if (i_mipmap == 0) {
                    imageDatas.at(i_image) = { (float)width, (float)height };
                    header.mipmap_pow2     = algorism::IsPowOfTwo((u32)(width * height));
                    // The full Parse() path calls SetHeaderPow2() per slot, which
                    // also derives mipmap_larger (mip0 area > map area).  The
                    // header-only path here forgot to, leaving mipmap_larger at
                    // its default false for sprite textures.  That matters for
                    // pictures stored as a single sprite frame inside a larger
                    // power-of-two container (mapWidth/mapHeight < tex size):
                    // WPSceneParser keys g_Texture0Resolution off mipmap_larger,
                    // and a false value collapses the image's UV mapRate to
                    // {1,1}, so the content only fills the map/tex fraction of
                    // the quad (Tomb Raider 1179142239: 3500x2188 image in a
                    // 4096x4096 .tex rendered into the top-left ~85%x53%).
                    // Real animated sprite sheets store map == atlas, so this
                    // stays false for them — only padded pictures flip true.
                    header.mipmap_larger = static_cast<i64>(width) * height >
                                           static_cast<i64>(header.mapWidth) * header.mapHeight;
                }
                if (header.extraHeader["texb"].val > 1) {
                    int32_t LZ4_compressed    = file.ReadInt32();
                    int32_t decompressed_size = file.ReadInt32();
                    (void)LZ4_compressed;
                    (void)decompressed_size;
                }
                long src_size = file.ReadInt32();
                file.SeekCur(src_size);
            }
        }
        // sprite pos
        int32_t texs       = ReadTexVesion(file);
        int32_t framecount = file.ReadInt32();
        if (texs > 3) {
            LOG_ERROR("Unkown texs version");
        }
        if (texs == 3) {
            i32 width  = file.ReadInt32();
            i32 height = file.ReadInt32();
            (void)width;
            (void)height;
        }

        for (int32_t i = 0; i < framecount; i++) {
            SpriteFrame sf;
            sf.imageId = file.ReadInt32();
            // The imageId range check alone is not enough: a slot whose declared
            // mipmap_count was 0 never had its {width,height} pair written, so it
            // is still an empty vector even though its index is in range.  Reading
            // [0]/[1] off it is an out-of-bounds access on a valid-looking id.
            if (sf.imageId < 0 || (usize)sf.imageId >= imageDatas.size() ||
                imageDatas.at((usize)sf.imageId).size() < 2) {
                LOG_ERROR("invalid sprite imageId %d (image_count=%zu, slot has %zu dims), "
                          "skipping sprite",
                          sf.imageId,
                          imageDatas.size(),
                          (usize)sf.imageId < imageDatas.size()
                              ? imageDatas.at((usize)sf.imageId).size()
                              : (usize)0);
                header.isSprite = false;
                break;
            }
            float spriteWidth  = imageDatas.at((usize)sf.imageId)[0];
            float spriteHeight = imageDatas.at((usize)sf.imageId)[1];

            sf.frametime = file.ReadFloat();
            if (texs == 1) {
                sf.x        = (float)file.ReadInt32() / spriteWidth;
                sf.y        = (float)file.ReadInt32() / spriteHeight;
                sf.xAxis[0] = (float)file.ReadInt32();
                sf.xAxis[1] = (float)file.ReadInt32();
                sf.yAxis[0] = (float)file.ReadInt32();
                sf.yAxis[1] = (float)file.ReadInt32();
            } else {
                sf.x        = file.ReadFloat() / spriteWidth;
                sf.y        = file.ReadFloat() / spriteHeight;
                sf.xAxis[0] = file.ReadFloat();
                sf.xAxis[1] = file.ReadFloat();
                sf.yAxis[0] = file.ReadFloat();
                sf.yAxis[1] = file.ReadFloat();
            }
            sf.width  = (float)std::sqrt(std::pow(sf.xAxis[0], 2) + std::pow(sf.xAxis[1], 2));
            sf.height = (float)std::sqrt(std::pow(sf.yAxis[0], 2) + std::pow(sf.yAxis[1], 2));
            sf.xAxis[0] /= spriteWidth;
            sf.xAxis[1] /= spriteWidth;
            sf.yAxis[0] /= spriteHeight;
            sf.yAxis[1] /= spriteHeight;
            sf.rate = sf.height / sf.width;
            header.spriteAnim.AppendFrame(sf);
        }
    } else {
        i32 mipmap_count = file.ReadInt32();
        (void)mipmap_count;
        i32 width  = file.ReadInt32();
        i32 height = file.ReadInt32();
        SetHeaderPow2(header, width, height);
    }
    // Diagnostic: surfaces atlas/content mismatches (mipmap_larger) and sprite
    // vs picture classification — the inputs to the g_Texture0Resolution /
    // image UV mapRate decision in WPSceneParser.  Kept for future audits of
    // padded-picture textures (see Tomb Raider 1179142239 background fix).
    LOG_INFO("TEXHDR '%s' sprite=%d tex=%dx%d map=%dx%d mipmap_larger=%d frames=%zu",
             name.c_str(),
             (int)header.isSprite,
             header.width,
             header.height,
             header.mapWidth,
             header.mapHeight,
             (int)header.mipmap_larger,
             header.spriteAnim.numFrames());
    // Cache the successfully-parsed header so subsequent ParseHeader calls for
    // the same name on this instance skip the file open + parse.
    m_headerCache[name] = header;
    return header;
}
