#include "WPTexImageParser.hpp"

#include "Type.hpp"
#include "WPTexImageHelpers.h"
#include "WPCommon.hpp"
#include <cstdint>
#include <lz4.h>

#include "SpriteAnimation.hpp"
#include "Utils/Algorism.h"
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
    img.key        = name;
    img.header     = MakeFallbackHeader();

    Image::Slot slot;
    slot.width  = 1;
    slot.height = 1;

    ImageData mipmap;
    mipmap.width  = 1;
    mipmap.height = 1;
    mipmap.size   = 4;
    mipmap.data   = ImageDataPtr(new uint8_t[4] { 255, 255, 255, 255 },
                                 [](uint8_t* p) { delete[] p; });
    slot.mipmaps.push_back(std::move(mipmap));
    img.slots.push_back(std::move(slot));
    return img_ptr;
}

} // namespace

void WPTexImageParser::RegisterImage(const std::string& key, std::shared_ptr<Image> img) {
    m_registered[key] = std::move(img);
}

std::shared_ptr<Image> WPTexImageParser::Parse(const std::string& name) {
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
    auto pfile = m_vfs->Open(path);
    if (! pfile) return nullptr;
    auto& file     = *pfile;
    auto  startpos = file.Tell();
    LoadHeader(file, img.header);

    {
        LOG_INFO("tex '%s': %dx%d fmt=%s sprite=%d count=%d",
                 name.c_str(), img.header.width, img.header.height,
                 ToString(img.header.format).c_str(),
                 (int)img.header.isSprite, img.header.count);
    }

    // image
    i32 _image_count = img.header.count;
    if (_image_count < 0) return nullptr;
    usize image_count = (usize)_image_count;

    img.slots.resize(image_count);
    for (usize i_image = 0; i_image < image_count; i_image++) {
        auto& img_slot = img.slots[i_image];
        auto& mipmaps  = img_slot.mipmaps;

        usize mipmap_count = (usize)std::max<i32>(file.ReadInt32(), 0);
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
            // Detect MP4 video data misidentified as raw texture (isVideoMp4 flag not set).
            // MP4 containers start with a size field + "ftyp" signature.
            if (src_size > 8 && result.size() >= 8 &&
                std::memcmp(result.data() + 4, "ftyp", 4) == 0) {
                LOG_INFO("tex '%s' mip[%zu]: detected MP4 video data (%d bytes), "
                         "using solid fallback (video textures not supported)",
                         name.c_str(), i_mipmap, src_size);
                i32 raw_size = mipmap.width * mipmap.height * 4;
                auto buf = std::make_unique<uint8_t[]>((usize)raw_size);
                std::memset(buf.get(), 0, (usize)raw_size);
                mipmap.data = ImageDataPtr(buf.release(), [](uint8_t* p) { delete[] p; });
                mipmap.size = raw_size;
                continue;
            }
            // is image container
            if (img.header.extraHeader["texb"].val >= 3 && img.header.type != ImageType::UNKNOWN) {
                int32_t w, h, n;
                auto*   data =
                    stbi_load_from_memory((const unsigned char*)result.data(), src_size, &w, &h, &n, 4);
                mipmap.data = ImageDataPtr((uint8_t*)data, [](uint8_t* data) {
                    stbi_image_free((unsigned char*)data);
                });
                src_size    = w * h * 4;
            } else {
                auto buf = std::make_unique<uint8_t[]>((usize)src_size);
                std::copy(result.data(), result.data() + src_size, buf.get());
                mipmap.data = ImageDataPtr(buf.release(), [](uint8_t* p) { delete[] p; });
            }
            mipmap.size = src_size;
        }
    }
    return img_ptr;
}

ImageHeader WPTexImageParser::ParseHeader(const std::string& name) {
    if (auto it = m_registered.find(name); it != m_registered.end()) {
        return it->second->header;
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
            if (sf.imageId < 0 || (usize)sf.imageId >= imageDatas.size()) {
                LOG_ERROR("invalid sprite imageId %d (image_count=%zu), skipping sprite",
                          sf.imageId, imageDatas.size());
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
    return header;
}
