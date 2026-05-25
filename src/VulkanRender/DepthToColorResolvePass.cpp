#include "DepthToColorResolvePass.hpp"

#include "Utils/Logging.h"

using namespace wallpaper::vulkan;

DepthToColorResolvePass::DepthToColorResolvePass(const Desc& desc) {
    // Field-by-field copy: vvk::Framebuffer / vvk::Pipeline (inside
    // PipelineParameters) are move-only handles, so a memberwise copy of Desc
    // is ill-formed.  Leg 04's prepare() populates the pipeline/fb members
    // on this pass instance directly; the constructor only needs the
    // caller-supplied descriptors.
    m_desc.input_depth_image = desc.input_depth_image;
    m_desc.input_depth_view  = desc.input_depth_view;
    m_desc.extent            = desc.extent;
    m_desc.output            = desc.output;
    m_desc.output_format     = desc.output_format;
}
DepthToColorResolvePass::~DepthToColorResolvePass() {}

void DepthToColorResolvePass::prepare(Scene&, const Device&, RenderingResources&) {
    static bool warned = false;
    if (! warned) {
        LOG_INFO("DepthToColorResolvePass: stubbed (path D / leg 04 wiring pending). "
                 "Volumetric chain on this device will degrade to back-of-volume only.");
        warned = true;
    }
}

void DepthToColorResolvePass::execute(const Device&, RenderingResources&) {
    // Stub: no draw; downstream consumers will see whatever cleared value
    // _rt_sceneDepthLinear holds (typically far-plane 1.0).  Leg 04 fills in
    // the real fullscreen-triangle draw.
}

void DepthToColorResolvePass::destory(const Device&, RenderingResources&) {}
