#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct mpv_handle;
struct mpv_render_context;

namespace wallpaper {

/// Decodes MP4 video frames via libmpv software rendering into RGBA pixel buffers.
/// Thread-safe: mpv decodes on internal threads, render thread reads frames via
/// acquireFrame()/releaseFrame(). Uses triple buffering for lock-free handoff.
class VideoTextureDecoder {
public:
    VideoTextureDecoder(int width, int height);
    ~VideoTextureDecoder();

    /// Load video file and start decoding. Returns false on failure.
    bool open(const std::string& path);

    /// Playback control
    void play();
    void pause();
    void stop();
    bool isPlaying() const { return m_playing.load(); }

    /// Returns true if a new frame is available since last acquireFrame().
    /// Also triggers pending mpv renders.
    bool hasNewFrame();

    /// Get pointer to the latest decoded frame (RGBA8, width*height*4 bytes).
    /// Returns nullptr if no frame decoded yet.
    /// Caller must call releaseFrame() when done reading.
    const uint8_t* acquireFrame();
    void           releaseFrame();

    int width()  const { return m_width; }
    int height() const { return m_height; }

private:
    static void onMpvRenderUpdate(void* ctx);
    void        renderFrame();
    std::atomic<bool> m_needsRender { false };

    int    m_width;
    int    m_height;
    size_t m_stride;

    mpv_handle*         m_mpv { nullptr };
    mpv_render_context* m_renderCtx { nullptr };

    // Triple buffer: 0=decode target, 1=ready, 2=read target
    static constexpr int NUM_BUFFERS = 3;
    std::unique_ptr<uint8_t[]> m_buffers[NUM_BUFFERS];
    std::atomic<int>           m_decodeIdx { 0 };  // decoder writes here
    std::atomic<int>           m_readyIdx  { 1 };  // latest complete frame
    std::atomic<int>           m_readIdx   { 2 };  // render reads here
    std::atomic<uint64_t>      m_frameNum  { 0 };  // monotonic frame counter
    uint64_t                   m_lastReadFrame { 0 };

    std::atomic<bool> m_playing  { false };
    std::atomic<bool> m_opened   { false };
    std::mutex        m_renderMutex; // protects mpv_render_context_render
};

} // namespace wallpaper
