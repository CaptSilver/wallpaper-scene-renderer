#pragma once
#include "VideoTextureDecoder.hpp"

typedef void*   EGLDisplay;
typedef void*   EGLContext;
typedef void*   EGLConfig;
typedef unsigned int GLuint;

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
    void cleanupEGL();
    void cleanupGL();

    EGLDisplay m_eglDisplay { nullptr };
    EGLContext m_eglContext { nullptr };

    GLuint m_fbo { 0 };
    GLuint m_fboTex { 0 };
};

} // namespace wallpaper
