#pragma once

#include "RenderGraph/RenderGraph.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Swapchain/ExSwapchain.hpp"
#include "Type.hpp"

#include <cstdio>
#include <memory>
#include <string>

namespace wallpaper
{
class Scene;
class Image;

namespace vulkan
{
class FinPass;

class VulkanRender {
public:
    VulkanRender();
    ~VulkanRender();

    bool init(RenderInitInfo);

    void destroy();

    void drawFrame(Scene&);
    bool reuploadTexture(const std::string& key, Image& image);

    void clearLastRenderGraph(Scene* scene = nullptr);
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);

    ExSwapchain* exSwapchain() const;
    bool         inited() const;
    bool         deviceLost() const;
    bool         hdrContent() const;
    // Update FinPass tonemap state to match the scene's HDR intent. Forces FinPass
    // to re-prepare on next compileRenderGraph if the state changes.
    void setSceneHdrContent(bool hdr);

    // Screenshot support: record a path, then takeScreenshotIfRequested()
    // will capture the most recently presented swapchain image to a PPM file
    // and clear the path.  Thread-safe; called from the render thread right
    // after WaitIdle in drawFrameSwapchain.
    void setScreenshotPath(const std::string& path);
    bool screenshotDone() const;

    // Per-pass dump: after the NEXT frame finishes, iterate all
    // CustomShaderPasses and write each one's output render target to
    // `<dir>/pass_<NNN>_<id>_<shader>_<out>.ppm`.  Shows the state of each
    // render target immediately after its producing pass completed —
    // useful for locating where text / colour corruption first appears in
    // an effect chain.  No-op outside first dump frame, so cost is bounded.
    void setPassDumpDir(const std::string& dir);
    bool passDumpDone() const;

    // Push the present-mode policy, output-refresh-hz and target-fps into the
    // live Swapchain so the next acquire/present picks the matching mode.
    // policy is the PresentModePolicy enum encoded as int (0=Auto / 1=Fifo /
    // 2=FifoRelaxed / 3=Mailbox / 4=Immediate).  Forces a swapchain recreate so
    // the new mode takes effect without waiting for the next OUT_OF_DATE.
    // No-op when the renderer isn't initialised yet (the values stick on the
    // Swapchain's pre-Create defaults, picked up at Create() time).  Safe to
    // call before init(); the Swapchain owns the fields and reads them at
    // every Create()/Recreate().
    void setSwapchainPresentPolicy(int policy);
    void setSwapchainOutputRefreshHz(int hz);
    void setSwapchainTargetFps(int fps);

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper