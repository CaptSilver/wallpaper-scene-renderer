#include "HWVideoTextureDecoder.hpp"
#include "Utils/Logging.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <gbm.h>

#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <filesystem>

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
    // Find a working GPU render node via GBM
    for (const auto& entry : std::filesystem::directory_iterator("/dev/dri")) {
        std::string name = entry.path().filename().string();
        if (name.find("renderD") != 0) continue;

        int fd = ::open(entry.path().c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        struct gbm_device* gbm = gbm_create_device(fd);
        if (! gbm) { ::close(fd); continue; }

        EGLDisplay display = eglGetPlatformDisplay(
            EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        if (display == EGL_NO_DISPLAY) {
            gbm_device_destroy(gbm);
            ::close(fd);
            continue;
        }

        EGLint major, minor;
        if (! eglInitialize(display, &major, &minor)) {
            gbm_device_destroy(gbm);
            ::close(fd);
            continue;
        }

        // Try desktop GL config
        eglBindAPI(EGL_OPENGL_API);
        EGLint    configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
        EGLConfig config = nullptr;
        EGLint    numConfigs = 0;
        eglChooseConfig(display, configAttribs, &config, 1, &numConfigs);

        if (numConfigs == 0) {
            eglTerminate(display);
            gbm_device_destroy(gbm);
            ::close(fd);
            continue;
        }

        EGLint ctxAttribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 2,
            EGL_NONE,
        };
        EGLContext ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, ctxAttribs);
        if (ctx == EGL_NO_CONTEXT) {
            eglTerminate(display);
            gbm_device_destroy(gbm);
            ::close(fd);
            continue;
        }

        if (! eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            eglDestroyContext(display, ctx);
            eglTerminate(display);
            gbm_device_destroy(gbm);
            ::close(fd);
            continue;
        }

        // Success!
        m_eglDisplay = display;
        m_eglContext = ctx;
        m_gbmDevice  = gbm;
        m_drmFd      = fd;
        LOG_INFO("HWVideoTextureDecoder: EGL %d.%d on %s (GBM)",
                 major, minor, entry.path().c_str());
        return true;
    }

    LOG_ERROR("HWVideoTextureDecoder: no working GPU render node found");
    return false;
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
    if (m_gbmDevice) {
        gbm_device_destroy(m_gbmDevice);
        m_gbmDevice = nullptr;
    }
    if (m_drmFd >= 0) {
        ::close(m_drmFd);
        m_drmFd = -1;
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

    // Create OpenGL render context with DRM render node for VA-API
    mpv_opengl_init_params gl_init_params {
        eglGetProcAddressWrapper,
        nullptr,
    };
    mpv_opengl_drm_params_v2 drm_params {};
    drm_params.fd        = -1;
    drm_params.crtc_id   = -1;
    drm_params.connector_id = -1;
    drm_params.render_fd = m_drmFd; // render node for VA-API interop

    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE,
          const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL) },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params },
        { MPV_RENDER_PARAM_DRM_DISPLAY_V2,     &drm_params },
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

    // Force VA-API hwdec now that GL context is available
    mpv_set_property_string(m_mpv, "hwdec", "vaapi");

    loadFile(path);

    LOG_INFO("HWVideoTextureDecoder(GL): opened '%s' (%dx%d)",
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

    publishFrame();
}

} // namespace wallpaper
