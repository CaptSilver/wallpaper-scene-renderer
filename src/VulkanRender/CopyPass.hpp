#pragma once
#include "VulkanPass.hpp"
#include <string>

#include "Vulkan/Device.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Scene/Scene.h"

namespace wallpaper
{
namespace vulkan
{

class CopyPass : public VulkanPass {
public:
    struct Desc {
        std::string src;
        std::string dst;
        bool        flipY { false };

        ImageParameters vk_src;
        ImageParameters vk_dst;
        // The slots behind src/dst; the handle copies above are refreshed
        // from them by rebindImages().
        RtSlot* src_slot { nullptr };
        RtSlot* dst_slot { nullptr };
    };

    CopyPass(const Desc&);
    virtual ~CopyPass();

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    // Re-copies src/dst handles from their slots when a slot's generation
    // moved or its image appeared or vanished.
    void rebindImages();

    // Accessor so the pass-output cache safety analyzer can see CopyPass
    // writes — otherwise a COPY into an aliased pingpong RT looks like
    // "static bytes" to a cached upstream pass, leading to cumulative
    // feedback (see the Lucy cloud acceleration bug).
    const Desc& desc() const { return m_desc; }

private:
    Desc       m_desc;
    BoundImage m_src_bound, m_dst_bound;
};

} // namespace vulkan
} // namespace wallpaper