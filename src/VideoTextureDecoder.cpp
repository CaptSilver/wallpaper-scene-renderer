#include "VideoTextureDecoder.hpp"
#include "Utils/Logging.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <cstring>

namespace wallpaper {

VideoTextureDecoder::VideoTextureDecoder(int width, int height)
    : m_width(width), m_height(height), m_stride(width * 4) {
    for (int i = 0; i < NUM_BUFFERS; i++) {
        m_buffers[i] = std::make_unique<uint8_t[]>((size_t)m_stride * m_height);
        std::memset(m_buffers[i].get(), 0, (size_t)m_stride * m_height);
    }
}

VideoTextureDecoder::~VideoTextureDecoder() {
    if (m_renderCtx) {
        mpv_render_context_free(m_renderCtx);
        m_renderCtx = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

bool VideoTextureDecoder::open(const std::string& path) {
    m_mpv = mpv_create();
    if (! m_mpv) {
        LOG_ERROR("VideoTextureDecoder: mpv_create failed");
        return false;
    }

    // Configure for texture decoding: no audio, loop, no terminal output
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "audio", "no");
    mpv_set_option_string(m_mpv, "loop", "inf");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");
    // Use hardware decoding if available (SW render API = software OUTPUT,
    // but the decoder itself can still use VA-API/NVDEC)
    mpv_set_option_string(m_mpv, "hwdec", "auto");

    if (mpv_initialize(m_mpv) < 0) {
        LOG_ERROR("VideoTextureDecoder: mpv_initialize failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // Create SW render context
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE,
          const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        LOG_ERROR("VideoTextureDecoder: mpv_render_context_create failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // Set update callback — called by mpv when a new frame is decoded
    mpv_render_context_set_update_callback(m_renderCtx, onMpvRenderUpdate, this);

    // Load the video file
    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    mpv_command(m_mpv, cmd);

    m_opened.store(true);
    m_playing.store(true);

    LOG_INFO("VideoTextureDecoder: opened '%s' (%dx%d)", path.c_str(), m_width, m_height);
    return true;
}

void VideoTextureDecoder::play() {
    if (! m_mpv) return;
    mpv_set_property_string(m_mpv, "pause", "no");
    m_playing.store(true);
}

void VideoTextureDecoder::pause() {
    if (! m_mpv) return;
    mpv_set_property_string(m_mpv, "pause", "yes");
    m_playing.store(false);
}

void VideoTextureDecoder::stop() {
    if (! m_mpv) return;
    mpv_command_string(m_mpv, "stop");
    m_playing.store(false);
}

void VideoTextureDecoder::onMpvRenderUpdate(void* ctx) {
    auto* self = static_cast<VideoTextureDecoder*>(ctx);
    self->m_needsRender.store(true);
}

void VideoTextureDecoder::renderFrame() {
    if (! m_renderCtx || ! m_needsRender.exchange(false)) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    int decIdx = m_decodeIdx.load();
    uint8_t* buf = m_buffers[decIdx].get();

    int    size[2]  = { m_width, m_height };
    size_t stride   = m_stride;
    mpv_render_param sw_params[] = {
        { MPV_RENDER_PARAM_SW_SIZE,    size },
        { MPV_RENDER_PARAM_SW_FORMAT,  const_cast<char*>("rgb0") },
        { MPV_RENDER_PARAM_SW_STRIDE,  &stride },
        { MPV_RENDER_PARAM_SW_POINTER, buf },
        { MPV_RENDER_PARAM_INVALID,    nullptr },
    };

    if (mpv_render_context_render(m_renderCtx, sw_params) >= 0) {
        // rgb0 format: alpha channel is uninitialized ("0" = garbage).
        // Fill alpha to 255 (opaque) so translucent blending shows the frame.
        for (int y = 0; y < m_height; y++) {
            uint8_t* row = buf + y * m_stride;
            for (int x = 0; x < m_width; x++) {
                row[x * 4 + 3] = 255;
            }
        }
        // Swap: make decode buffer the new ready buffer
        int oldReady = m_readyIdx.exchange(decIdx);
        m_decodeIdx.store(oldReady);
        m_frameNum.fetch_add(1);
    }
}

bool VideoTextureDecoder::hasNewFrame() {
    // Try to render pending frame from mpv's decode thread
    if (m_needsRender.load()) renderFrame();
    return m_frameNum.load() > m_lastReadFrame;
}

const uint8_t* VideoTextureDecoder::acquireFrame() {
    if (! m_opened.load()) return nullptr;
    uint64_t fn = m_frameNum.load();
    if (fn == 0) return nullptr; // no frame decoded yet

    // Swap ready → read
    int readyIdx = m_readyIdx.load();
    int oldRead  = m_readIdx.exchange(readyIdx);
    m_readyIdx.store(oldRead);
    m_lastReadFrame = fn;

    return m_buffers[m_readIdx.load()].get();
}

void VideoTextureDecoder::releaseFrame() {
    // No-op for now — triple buffer doesn't need explicit release
}

} // namespace wallpaper
