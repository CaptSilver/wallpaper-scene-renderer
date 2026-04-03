#pragma once
#include "VideoTextureDecoder.hpp"

typedef void*   EGLDisplay;
typedef void*   EGLContext;
typedef void*   EGLConfig;
typedef unsigned int GLuint;
struct gbm_device;

namespace wallpaper {

/// Hardware-accelerated video texture decoder using mpv's OpenGL render API
/// with a headless EGL context. VA-API decodes on GPU, mpv renders to GL FBO,
/// glReadPixels copies RGBA to CPU buffer for Vulkan upload.
class HWVideoTextureDecoder : public VideoTextureDecoder {
public:
    HWVideoTextureDecoder(int width, int height);
    ~HWVideoTextureDecoder() override;

    bool open(const std::string& path) override;

protected:
    void renderFrame() override;

private:
    bool initEGL();
    bool exportDmaBuf();
    void cleanupEGL();
    void cleanupGL();

    EGLDisplay m_eglDisplay { nullptr };
    EGLContext m_eglContext { nullptr };
    ::gbm_device*      m_gbmDevice { nullptr };
    int                m_drmFd { -1 };

    GLuint m_fbo { 0 };
    GLuint m_fboTex { 0 };

    // DMA-BUF zero-copy: exported from GL texture for Vulkan import
    int      m_dmabufFd { -1 };
    int      m_dmabufStride { 0 };
    int      m_dmabufOffset { 0 };
    int      m_drmFourcc { 0 };
    uint64_t m_drmModifier { 0 };
    bool     m_dmabufExported { false };

public:
    /// DMA-BUF info for Vulkan import. Valid after first frame rendered.
    bool     hasDmaBuf()    const { return m_dmabufExported; }
    int      dmabufFd()     const { return m_dmabufFd; }
    int      dmabufStride() const { return m_dmabufStride; }
    int      dmabufOffset() const { return m_dmabufOffset; }
    int      drmFourcc()    const { return m_drmFourcc; }
    uint64_t drmModifier()  const { return m_drmModifier; }
};

} // namespace wallpaper
