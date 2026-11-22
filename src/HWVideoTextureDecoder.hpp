#pragma once
#include "VideoTextureDecoder.hpp"

#include <filesystem>
#include <string>
#include <vector>

typedef void*        EGLDisplay;
typedef void*        EGLContext;
typedef void*        EGLConfig;
typedef unsigned int GLuint;
struct gbm_device;

namespace wallpaper
{

/// A DRM render node the hardware decoder can run on, with the kernel driver
/// bound to it ("i915", "amdgpu", "nvidia", ...).  `driver` is empty when it
/// can't be told (not a device node, no sysfs entry).
struct HwDecodeRenderNode {
    std::filesystem::path path;
    std::string           driver;
};

/// Kernel driver of the character device major:minor, read from
/// <sysRoot>/dev/char/<major>:<minor>/device/driver.  Empty when there is none.
std::string hwDecodeKernelDriver(unsigned int devMajor, unsigned int devMinor,
                                 const std::filesystem::path& sysRoot = "/sys");

/// Puts `nodes` in the order they are tried: nodes that can do VA-API first,
/// the proprietary NVIDIA driver (no VA-API; decodes through nvdec) last, by
/// file name inside each group.  On a hybrid laptop that is the integrated GPU
/// first, whatever order the directory lists the nodes in.
void sortHwDecodeRenderNodes(std::vector<HwDecodeRenderNode>& nodes);

/// The renderD* entries of `driRoot`, in the order they are tried.
std::vector<HwDecodeRenderNode>
listHwDecodeRenderNodes(const std::filesystem::path& driRoot,
                        const std::filesystem::path& sysRoot = "/sys");

/// Hardware-accelerated video texture decoder using mpv's OpenGL render API
/// with a headless EGL context. VA-API (nvdec on the NVIDIA driver) decodes on
/// GPU, mpv renders to GL FBO, glReadPixels copies RGBA to CPU buffer for
/// Vulkan upload.
class HWVideoTextureDecoder : public VideoTextureDecoder {
public:
    HWVideoTextureDecoder(int width, int height);
    ~HWVideoTextureDecoder() override;

    bool open(const std::string& path, std::string* outError = nullptr) override;

protected:
    void renderFrame() override;

private:
    bool initEGL(std::string* outError = nullptr);
    void cleanupEGL();
    void cleanupGL();

    EGLDisplay    m_eglDisplay { nullptr };
    EGLContext    m_eglContext { nullptr };
    ::gbm_device* m_gbmDevice { nullptr };
    int           m_drmFd { -1 };
    // The render node again, opened a second time for VA-API (see open()).
    int         m_vaFd { -1 };
    std::string m_nodePath;

    GLuint m_fbo { 0 };
    GLuint m_fboTex { 0 };
};

} // namespace wallpaper
