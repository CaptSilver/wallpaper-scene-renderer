#pragma once
#include "RenderGraph/Pass.hpp"
#include "VulkanRender/RtResidency.hpp"
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>

namespace wallpaper
{

class Scene;

namespace vulkan
{

class Device;
struct RenderingResources;
class Resource;

class VulkanPass : public rg::Pass {
public:
    VulkanPass()                                                     = default;
    virtual ~VulkanPass()                                            = default;
    virtual void prepare(Scene&, const Device&, RenderingResources&) = 0;
    virtual void execute(const Device&, RenderingResources&)         = 0;
    virtual void destory(const Device&, RenderingResources&)         = 0;

    void addReleaseTexs(std::span<const std::string_view> texs) {
        m_release_texs.clear();
        std::transform(texs.begin(), texs.end(), std::back_inserter(m_release_texs), [](auto& sv) {
            return std::string(sv);
        });
    }
    bool                         prepared() const { return m_prepared; }
    std::span<const std::string> releaseTexs() const { return m_release_texs; }
    void                         clearReleaseTexs() { m_release_texs.clear(); }

    // What the residency walk needs from this pass this frame.  Passes with no
    // node (PrePass, FinPass, SkyboxPass, CopyPass) always draw.
    virtual PassDemandFrame demandFrame() const { return { true, false, DemandInputs::Own }; }

    // Whether the demand walk wanted this pass's output this frame.  Set by the
    // residency step before the pass loop; a pass whose managed output is
    // resident but not needed skips, since nothing will read what it draws.
    bool outputNeeded() const { return m_output_needed; }
    void setOutputNeeded(bool v) { m_output_needed = v; }

    // Override in subclasses to indicate if pass can be cached
    virtual bool isStatic() const { return false; }

protected:
    void setPrepared(bool v = true) { m_prepared = v; }

private:
    bool                     m_prepared { false };
    bool                     m_output_needed { true };
    std::vector<std::string> m_release_texs;
};
} // namespace vulkan
} // namespace wallpaper
