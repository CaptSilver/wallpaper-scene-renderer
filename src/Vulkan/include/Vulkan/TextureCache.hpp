#pragma once

#include "Parameters.hpp"
#include "Type.hpp"
// Provides the inline ToVkType(TextureFormat) definition.  Must be included
// here (not just in TextureCache.cpp) so EVERY caller that includes
// TextureCache.hpp — e.g. VulkanRender/CustomShaderPass.cpp — sees the inline
// body and doesn't emit an unresolved external reference.  Otherwise the
// plugin .so fails to dlopen with "undefined symbol: ...ToVkType...".
#include "TexFormatVk.hpp"
#include "Core/NoCopyMove.hpp"
#include "Core/MapSet.hpp"
#include "StagingReuse.hpp"

namespace wallpaper
{

class Image;

namespace vulkan
{

// ToVkType(TextureFormat) is defined inline in TexFormatVk.hpp (included above)
// so it has a definition in every translation unit that calls it.
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureKey {
    i32           width;
    i32           height;
    TexUsage      usage;
    TextureFormat format;
    TextureSample sample;
    uint          mipmap_level { 1 };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    TextureCache(const Device&);
    ~TextureCache();

    void Clear();

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling);
    ImageSlotsRef                    CreateTex(Image&);
    bool                             ReuploadTex(const std::string& key, Image& image);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    void                              allocateCmd();
    vvk::CommandBuffers               m_tex_cmds;
    vvk::CommandBuffer                m_tex_cmd;

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;

    // Persistent per-texture staging buffers reused by ReuploadTex (video
    // frames) instead of reallocating a ~frame-sized buffer every frame.
    // Indexed [slot][mip]; cleared in Clear().
    Map<std::string, std::vector<std::vector<VmaBufferParameters>>> m_reupload_staging;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
};

} // namespace vulkan
} // namespace wallpaper
