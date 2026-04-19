#include "VideoTextureDecoder.hpp"
#include "Utils/Logging.h"

#include <mpv/client.h>
#include <mpv/render.h>
#include <clocale>
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

bool VideoTextureDecoder::initMpv() {
    // mpv refuses to initialize when LC_NUMERIC is non-C (prints "Non-C locale
    // detected" and returns NULL from mpv_create).  Qt sets the process locale
    // from the environment during QCoreApplication init, so we force LC_NUMERIC
    // back to "C" for numeric parsing before handing off to libmpv.  Scoped to
    // LC_NUMERIC so time/message formatting elsewhere is unaffected.
    std::setlocale(LC_NUMERIC, "C");
    m_mpv = mpv_create();
    if (! m_mpv) {
        LOG_ERROR("VideoTextureDecoder: mpv_create failed");
        return false;
    }
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "audio", "no");
    mpv_set_option_string(m_mpv, "loop", "inf");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "msg-level", "all=warn");
    mpv_set_option_string(m_mpv, "hwdec", "vaapi");

    if (mpv_initialize(m_mpv) < 0) {
        LOG_ERROR("VideoTextureDecoder: mpv_initialize failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }
    return true;
}

bool VideoTextureDecoder::loadFile(const std::string& path) {
    const char* cmd[] = { "loadfile", path.c_str(), nullptr };
    mpv_command(m_mpv, cmd);
    m_opened.store(true);
    m_playing.store(true);
    return true;
}

bool VideoTextureDecoder::open(const std::string& path) {
    if (! initMpv()) return false;

    // Create SW render context
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE,
          const_cast<char*>(MPV_RENDER_API_TYPE_SW) },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&m_renderCtx, m_mpv, params) < 0) {
        LOG_ERROR("VideoTextureDecoder: mpv_render_context_create (SW) failed");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    mpv_render_context_set_update_callback(m_renderCtx, onMpvRenderUpdate, this);
    loadFile(path);

    LOG_INFO("VideoTextureDecoder(SW): opened '%s' (%dx%d)", path.c_str(), m_width, m_height);
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

double VideoTextureDecoder::getCurrentTimeSec() const {
    if (! m_mpv) return 0.0;
    double v = 0.0;
    if (mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &v) < 0) return 0.0;
    return v;
}

double VideoTextureDecoder::getDurationSec() const {
    if (! m_mpv) return 0.0;
    double v = 0.0;
    if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &v) < 0) return 0.0;
    return v;
}

void VideoTextureDecoder::setCurrentTimeSec(double t) {
    if (! m_mpv) return;
    // "time-pos" seek is only valid once the file is loaded.  Clamp negatives
    // (mpv rejects them) but let overrun pass through; libmpv will cap at
    // duration-on-its-own.
    if (t < 0) t = 0;
    mpv_set_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &t);
}

void VideoTextureDecoder::setRate(double r) {
    if (! m_mpv) return;
    // libmpv's "speed" must be > 0.  A zero rate would be semantically "pause"
    // — map to pause() to mirror mpv's own behavior and avoid EINVAL.
    if (r <= 0.0) { pause(); return; }
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &r);
}

void VideoTextureDecoder::onMpvRenderUpdate(void* ctx) {
    auto* self = static_cast<VideoTextureDecoder*>(ctx);
    self->m_needsRender.store(true);
}

void VideoTextureDecoder::fillAlpha(uint8_t* buf) {
    for (int y = 0; y < m_height; y++) {
        uint8_t* row = buf + y * m_stride;
        for (int x = 0; x < m_width; x++) {
            row[x * 4 + 3] = 255;
        }
    }
}

void VideoTextureDecoder::publishFrame() {
    int oldReady = m_readyIdx.exchange(m_decodeIdx.load());
    m_decodeIdx.store(oldReady);
    m_frameNum.fetch_add(1);
}

void VideoTextureDecoder::renderFrame() {
    if (! m_renderCtx || ! m_needsRender.exchange(false)) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    int decIdx = m_decodeIdx.load();
    uint8_t* buf = m_buffers[decIdx].get();

    int    size[2] = { m_width, m_height };
    size_t stride  = m_stride;
    mpv_render_param sw_params[] = {
        { MPV_RENDER_PARAM_SW_SIZE,    size },
        { MPV_RENDER_PARAM_SW_FORMAT,  const_cast<char*>("rgb0") },
        { MPV_RENDER_PARAM_SW_STRIDE,  &stride },
        { MPV_RENDER_PARAM_SW_POINTER, buf },
        { MPV_RENDER_PARAM_INVALID,    nullptr },
    };

    if (mpv_render_context_render(m_renderCtx, sw_params) >= 0) {
        fillAlpha(buf);
        publishFrame();
    }
}

bool VideoTextureDecoder::hasNewFrame() {
    if (m_needsRender.load()) renderFrame();
    return m_frameNum.load() > m_lastReadFrame;
}

const uint8_t* VideoTextureDecoder::acquireFrame() {
    if (! m_opened.load()) return nullptr;
    uint64_t fn = m_frameNum.load();
    if (fn == 0) return nullptr;

    int readyIdx = m_readyIdx.load();
    int oldRead  = m_readIdx.exchange(readyIdx);
    m_readyIdx.store(oldRead);
    m_lastReadFrame = fn;

    return m_buffers[m_readIdx.load()].get();
}

void VideoTextureDecoder::releaseFrame() {
    // No-op — triple buffer doesn't need explicit release
}

} // namespace wallpaper
