#include "HWVideoTextureDecoder.hpp"
#include "Utils/Logging.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include <cstring>
#include <dlfcn.h>

namespace wallpaper {

static void* eglGetProcAddressWrapper(void*, const char* name) {
    return (void*)eglGetProcAddress(name);
}

HWVideoTextureDecoder::HWVideoTextureDecoder(int width, int height)
    : VideoTextureDecoder(width, height) {}

HWVideoTextureDecoder::~HWVideoTextureDecoder() {
    // Destroy mpv render context before EGL context
    if (m_renderCtx) {
        mpv_render_context_free(m_renderCtx);
        m_renderCtx = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    cleanupGL();
    cleanupEGL();
}

bool HWVideoTextureDecoder::initEGL() {
    // Use surfaceless platform — no window or GBM needed
    m_eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,
                                          EGL_DEFAULT_DISPLAY, nullptr);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        LOG_ERROR("HWVideoTextureDecoder: eglGetPlatformDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (! eglInitialize(m_eglDisplay, &major, &minor)) {
        LOG_ERROR("HWVideoTextureDecoder: eglInitialize failed");
        return false;
    }

    // Try desktop GL first, then GLES
    EGLConfig config = nullptr;
    EGLint    numConfigs = 0;

    // Attempt 1: Desktop OpenGL
    eglBindAPI(EGL_OPENGL_API);
    {
        EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
        eglChooseConfig(m_eglDisplay, attribs, &config, 1, &numConfigs);
    }

    bool useGLES = false;
    if (numConfigs == 0) {
        // Attempt 2: OpenGL ES 3.x
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
        eglChooseConfig(m_eglDisplay, attribs, &config, 1, &numConfigs);
        useGLES = true;
    }

    if (numConfigs == 0) {
        LOG_ERROR("HWVideoTextureDecoder: eglChooseConfig failed (no GL or GLES3 config)");
        return false;
    }

    EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, useGLES ? 3 : 3,
        EGL_CONTEXT_MINOR_VERSION, useGLES ? 0 : 2,
        EGL_NONE,
    };
    m_eglContext = eglCreateContext(m_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (m_eglContext == EGL_NO_CONTEXT) {
        LOG_ERROR("HWVideoTextureDecoder: eglCreateContext failed");
        return false;
    }

    if (! eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext)) {
        LOG_ERROR("HWVideoTextureDecoder: eglMakeCurrent failed");
        return false;
    }

    LOG_INFO("HWVideoTextureDecoder: EGL %d.%d initialized (surfaceless)", major, minor);
    return true;
}

void HWVideoTextureDecoder::cleanupGL() {
    if (m_eglDisplay && m_eglContext) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext);
        if (m_fbo) {
            auto glDeleteFramebuffers = (void(*)(GLsizei, const GLuint*))
                eglGetProcAddress("glDeleteFramebuffers");
            if (glDeleteFramebuffers) glDeleteFramebuffers(1, &m_fbo);
            m_fbo = 0;
        }
        if (m_fboTex) {
            glDeleteTextures(1, &m_fboTex);
            m_fboTex = 0;
        }
    }
}

void HWVideoTextureDecoder::cleanupEGL() {
    if (m_eglDisplay) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_eglContext) {
            eglDestroyContext(m_eglDisplay, m_eglContext);
            m_eglContext = nullptr;
        }
        eglTerminate(m_eglDisplay);
        m_eglDisplay = nullptr;
    }
}

bool HWVideoTextureDecoder::open(const std::string& path) {
    if (! initEGL()) return false;

    // Create GL FBO for mpv to render into
    auto glGenFramebuffers = (void(*)(GLsizei, GLuint*))
        eglGetProcAddress("glGenFramebuffers");
    auto glBindFramebuffer = (void(*)(GLenum, GLuint))
        eglGetProcAddress("glBindFramebuffer");
    auto glFramebufferTexture2D = (void(*)(GLenum, GLenum, GLenum, GLuint, GLint))
        eglGetProcAddress("glFramebufferTexture2D");

    glGenTextures(1, &m_fboTex);
    glBindTexture(GL_TEXTURE_2D, m_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_width, m_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, m_fbo);
    glFramebufferTexture2D(0x8D40, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/,
                           GL_TEXTURE_2D, m_fboTex, 0);

    // Initialize mpv
    if (! initMpv()) {
        cleanupGL();
        cleanupEGL();
        return false;
    }

    // Create OpenGL render context
    mpv_opengl_init_params gl_init_params {
        eglGetProcAddressWrapper,
        nullptr,
    };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE,
          const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        LOG_ERROR("HWVideoTextureDecoder: mpv_render_context_create (GL) failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        cleanupGL();
        cleanupEGL();
        return false;
    }

    mpv_render_context_set_update_callback(m_renderCtx, onMpvRenderUpdate, this);
    loadFile(path);

    LOG_INFO("HWVideoTextureDecoder(GL+VA-API): opened '%s' (%dx%d)",
             path.c_str(), m_width, m_height);
    return true;
}

void HWVideoTextureDecoder::renderFrame() {
    if (! m_renderCtx || ! m_needsRender.exchange(false)) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    // Make our EGL context current (may have been unset by another thread)
    eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, m_eglContext);

    // Render into our FBO
    mpv_opengl_fbo mpv_fbo {
        .fbo = (int)m_fbo,
        .w   = m_width,
        .h   = m_height,
    };
    int flip_y = 0;
    mpv_render_param render_params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mpv_fbo },
        { MPV_RENDER_PARAM_FLIP_Y,     &flip_y },
        { MPV_RENDER_PARAM_INVALID,     nullptr },
    };

    if (mpv_render_context_render(m_renderCtx, render_params) < 0) return;

    // Read back pixels from FBO to CPU buffer
    int decIdx = m_decodeIdx.load();
    uint8_t* buf = m_buffers[decIdx].get();

    auto glBindFB = (void(*)(GLenum, GLuint))
        eglGetProcAddress("glBindFramebuffer");
    glBindFB(0x8D40 /*GL_FRAMEBUFFER*/, m_fbo);
    glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, buf);

    // GL renders bottom-up, flip vertically
    size_t rowSize = (size_t)m_width * 4;
    auto rowBuf = std::make_unique<uint8_t[]>(rowSize);
    for (int y = 0; y < m_height / 2; y++) {
        uint8_t* top = buf + y * rowSize;
        uint8_t* bot = buf + (m_height - 1 - y) * rowSize;
        std::memcpy(rowBuf.get(), top, rowSize);
        std::memcpy(top, bot, rowSize);
        std::memcpy(bot, rowBuf.get(), rowSize);
    }

    publishFrame();
}

} // namespace wallpaper
