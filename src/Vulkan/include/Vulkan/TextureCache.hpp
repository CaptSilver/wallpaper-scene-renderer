#pragma once

#include "Parameters.hpp"
#include "Type.hpp"
// Provides the inline ToVkType(TextureFormat) definition.  Must be included
// here (not just in TextureCache.cpp) so EVERY caller that includes
// TextureCache.hpp — e.g. VulkanRender/CustomShaderPass.cpp — sees the inline
// body and doesn't emit an unresolved external reference.  Otherwise the
// plugin .so fails to dlopen with "undefined symbol: ...ToVkType...".
#include "TexFormatVk.hpp"
#include "SamplerDedupDetail.hpp"
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

// Depth-attachment sampler configuration.  NEAREST/CLAMP/no-compare; used to
// sample _rt_sceneDepth in the volumetric chain.  Pure-data factory so unit
// tests can pin the contract without a live device.
VkSamplerCreateInfo GenDepthSamplerInfo();

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

    // Lazily creates and caches a NEAREST/CLAMP/no-compare sampler used for
    // sampling depth-attachment images (path A) and the depth-resolve color
    // RT (path D).  Process-lifetime handle owned by this TextureCache.
    VkSampler GetOrCreateDepthSampler();

    // Hash-keyed VkSampler dedup.  TextureCache::CreateTex used to mint a
    // fresh VkSampler per texture even when the sampler config matched one
    // already in the cache; most scenes have ~5-15 unique configs across
    // 50-200 textures.  Lookup by hashSamplerInfo(info); on miss, create the
    // VkSampler and store it in the bucket.  Returns a raw, non-owning
    // VkSampler — the vvk::Sampler is owned by m_sampler_cache and lives
    // until Clear() (which is called on scene swap, AFTER the images that
    // reference these samplers are torn down).
    //
    // Generalizes the existing single-slot GetOrCreateDepthSampler precedent
    // on the same class.  Returns VK_NULL_HANDLE on CreateSampler failure
    // (LOG_ERROR-logged).
    VkSampler GetOrCreateSampler(const VkSamplerCreateInfo& info);

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
        // Bumped on Query hit AND at insert time.  Drives the non-persist
        // LRU eviction sort when m_query_texs grows past m_query_soft_cap.
        uint64_t lru_tick { 0 };
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;

    // Monotonic per-Query-call counter; assigned to QueryTex::lru_tick on hit
    // or insert.  Wraparound is theoretical — even at one Query() per
    // microsecond, uint64_t lasts ~580k years.
    uint64_t m_lru_clock { 0 };

    // Soft cap on m_query_texs growth.  Env-overridable via
    // WEK_TEXCACHE_QUERY_CAP (in [8, 4096]); default 64 covers measured
    // peak (~30-40) on Totoro/Blur/Nightingale with headroom.  Eviction
    // walks non-persist entries by ascending lru_tick — persist=true
    // entries are never evicted.  See evictColdQueryTexs().
    uint32_t m_query_soft_cap { 64 };

    void evictColdQueryTexs();

    // Lazily-allocated depth sampler (NEAREST/CLAMP/no-compare).  See
    // GetOrCreateDepthSampler().
    vvk::Sampler m_depth_sampler;

    // Hash-keyed VkSampler dedup cache (MEM5).  Bucket-vector layout —
    // [hash -> vector<pair<info, vvk::Sampler>>] — disambiguates collisions
    // via samplerInfoEqual on confirm.  Cleared LAST in Clear() (after the
    // images that reference these samplers).  See GetOrCreateSampler().
    SamplerBucketMap<vvk::Sampler> m_sampler_cache;
};

} // namespace vulkan
} // namespace wallpaper
