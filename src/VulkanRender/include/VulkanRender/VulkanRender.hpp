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
    bool inited() const;
    bool deviceLost() const;
    bool hdrContent() const;
    // Update FinPass tonemap state to match the scene's HDR intent. Forces FinPass
    // to re-prepare on next compileRenderGraph if the state changes.
    void setSceneHdrContent(bool hdr);

    // Screenshot support: record a path, then takeScreenshotIfRequested()
    // will capture the most recently presented swapchain image to a PPM file
    // and clear the path.  Thread-safe; called from the render thread right
    // after WaitIdle in drawFrameSwapchain.
    void setScreenshotPath(const std::string& path);
    bool screenshotDone() const;

private:
    struct Impl;
    std::unique_ptr<Impl> pImpl;
};
} // namespace vulkan
} // namespace wallpaper