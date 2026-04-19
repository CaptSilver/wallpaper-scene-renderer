#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct mpv_handle;
struct mpv_render_context;

namespace wallpaper {

/// Base class for video texture decoders. Provides triple-buffered frame handoff
/// between decoder and render threads.
class VideoTextureDecoder {
public:
    VideoTextureDecoder(int width, int height);
    virtual ~VideoTextureDecoder();

    /// Load video file and start decoding. Returns false on failure.
    virtual bool open(const std::string& path);

    /// Playback control
    virtual void play();
    virtual void pause();
    virtual void stop();
    bool isPlaying() const { return m_playing.load(); }

    /// libmpv property queries. Return 0 / no-op when the decoder's mpv handle
    /// isn't yet initialized or the property isn't available yet (pre-frame).
    /// Thread-safe: libmpv property R/W is safe across threads.
    double getCurrentTimeSec() const;
    double getDurationSec() const;
    void   setCurrentTimeSec(double t);
    void   setRate(double r);

    /// Returns true if a new frame is available since last acquireFrame().
    virtual bool hasNewFrame();

    /// Get pointer to the latest decoded frame (RGBA8, width*height*4 bytes).
    /// Returns nullptr if no frame decoded yet.
    /// Caller must call releaseFrame() when done reading.
    const uint8_t* acquireFrame();
    void           releaseFrame();

    int width()  const { return m_width; }
    int height() const { return m_height; }

protected:
    static void onMpvRenderUpdate(void* ctx);
    virtual void renderFrame();

    int    m_width;
    int    m_height;
    size_t m_stride;

    mpv_handle*         m_mpv { nullptr };
    mpv_render_context* m_renderCtx { nullptr };

    // Triple buffer: 0=decode target, 1=ready, 2=read target
    static constexpr int NUM_BUFFERS = 3;
    std::unique_ptr<uint8_t[]> m_buffers[NUM_BUFFERS];
    std::atomic<int>           m_decodeIdx { 0 };
    std::atomic<int>           m_readyIdx  { 1 };
    std::atomic<int>           m_readIdx   { 2 };
    std::atomic<uint64_t>      m_frameNum  { 0 };
    uint64_t                   m_lastReadFrame { 0 };

    std::atomic<bool> m_playing  { false };
    std::atomic<bool> m_opened   { false };
    std::atomic<bool> m_needsRender { false };
    std::mutex        m_renderMutex;

    /// Publish a newly decoded frame (called after writing to m_buffers[m_decodeIdx])
    void publishFrame();
    /// Set alpha channel to 255 for the given buffer
    void fillAlpha(uint8_t* buf);

    /// Initialize mpv handle with common options. Subclasses call this then
    /// create their own render context.
    bool initMpv();
    /// Load video file into an already-initialized mpv instance.
    bool loadFile(const std::string& path);
};

} // namespace wallpaper
