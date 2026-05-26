#pragma once
#include <memory>
#include <vector>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <cstring>
#include <atomic>
#include <utility>

#include "Utils/Logging.h"
#include "Core/NoCopyMove.hpp"

#define MA_NO_WASAPI
#define MA_NO_DSOUND
#define MA_NO_WINMM
#define MA_NO_COREAUDIO
#define MA_NO_ENCODING
// The miniaudio + stb_vorbis implementations must live in exactly one
// translation unit (SoundManager.cpp).  Other TUs that need the types
// (e.g. unit tests for ProcessFrame) include this header without the
// _IMPL macro, getting only declarations.
#ifdef WEK_MINIAUDIO_IMPL
#  define STB_VORBIS_HEADER_ONLY
#  include <miniaudio/extras/stb_vorbis.c> /* header form */
#  define MINIAUDIO_IMPLEMENTATION
#  include <miniaudio/miniaudio.h>
#  undef STB_VORBIS_HEADER_ONLY
#  include <miniaudio/extras/stb_vorbis.c> /* implementation */
#else
#  include <miniaudio/miniaudio.h>
#endif

namespace miniaudio
{

struct DeviceDesc {
    ma_uint32              phyChannels;
    ma_uint32              sampleRate;
    static const ma_format format { ma_format_f32 };
};

template<typename TStream>
class Decoder : NoCopy {
public:
    Decoder(TStream&& s): m_stream(std::move(s)) {}
    ~Decoder() { ma_decoder_uninit(&m_decoder); }
    Decoder(Decoder&& o) noexcept
        : m_decoder(std::exchange(o.m_decoder, ma_decoder()), m_stream(std::move(m_stream))) {}
    Decoder& operator=(Decoder&& o) noexcept {
        m_decoder = std::exchange(o.m_decoder, ma_decoder());
        m_stream  = std::move(m_stream);
        return *this;
    }

    bool Init(const DeviceDesc& d) {
        ma_decoder_config config =
            ma_decoder_config_init(DeviceDesc::format, d.phyChannels, d.sampleRate);
        ma_result result = ma_decoder_init(Read, Seek, this, &config, &m_decoder);
        m_inited         = result == MA_SUCCESS;
        if (! m_inited) {
            LOG_ERROR("init decoder failed (ma_result=%d)", (int)result);
        }
        return m_inited;
    }
    ma_uint64 NextPcmData(void* pData, ma_uint64 frameCount) {
        if (! m_inited) return 0;
        decltype(frameCount) readed { 0 };
        ma_result result = ma_decoder_read_pcm_frames(&m_decoder, pData, frameCount, &readed);
        // Preserve partial reads at MA_AT_END — otherwise tail frames are dropped at
        // every loop boundary, causing an audible glitch. Callers treat readed==0
        // as "EOF, reload" regardless of the ma_result code.
        if (result == MA_SUCCESS || result == MA_AT_END) return readed;
        return 0;
    }
    bool IsInited() { return m_inited; }

private:
    static ma_result Read(ma_decoder* pMaDecoder, void* pBufferOut, size_t bytesToRead,
                          size_t* pBytesRead) {
        auto*  pDecoder = static_cast<Decoder<TStream>*>(pMaDecoder->pUserData);
        size_t r        = pDecoder->m_stream.Read(pBufferOut, bytesToRead);
        *pBytesRead     = r;
        // Match miniaudio's stdio-vfs convention: short read at EOF must signal
        // MA_AT_END. Format-format matters — WAV stops on its known sample count,
        // but OGG (stb_vorbis push mode) relies on this return code to know the
        // stream is done. Without it, Musik loops never trigger EOF.
        if (r == 0) return MA_AT_END;
        return MA_SUCCESS;
    }
    static ma_result Seek(ma_decoder* pMaDecoder, ma_int64 byteOffset, ma_seek_origin origin) {
        auto* pDecoder = static_cast<Decoder<TStream>*>(pMaDecoder->pUserData);
        bool  ok       = pDecoder->m_stream.Seek(byteOffset, origin);
        return ok ? MA_SUCCESS : MA_ERROR;
    }
    bool       m_inited { false };
    ma_decoder m_decoder {};
    TStream    m_stream;
};

class Channel : NoCopy {
public:
    Channel()          = default;
    virtual ~Channel() = default;

    virtual ma_uint64 NextPcmData(void* pData, ma_uint32 frameCount) = 0;
    virtual void      PassDeviceDesc(const DeviceDesc&)              = 0;
};

class Device : NoCopy {
public:
    Device() {}
    ~Device() { UnInit(); }
    Device(Device&& o) noexcept: m_device(std::exchange(o.m_device, ma_device())) {}
    Device& operator=(Device&& o) noexcept {
        m_device = std::exchange(o.m_device, ma_device());
        return *this;
    }

public:
    bool Init(const DeviceDesc& d) {
        if (IsInited()) return true; // already inited
        ma_result result;
        auto      config = GenMaDeviceConfig(d);
        Stop();
        result = ma_device_init(NULL, &config, &m_device);
        if (result == MA_SUCCESS) {
            LOG_INFO("sound device inited");
        }
        if (result != MA_SUCCESS || ! IsInited()) {
            LOG_ERROR("can't init sound device");
            UnInit();
            return false;
        }
        if (m_device.playback.format != ma_format_f32) {
            LOG_ERROR("wrong playback format");
            UnInit();
            return false;
        }
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            LOG_ERROR("can't start sound device");
            UnInit();
            return false;
        }
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            for (auto& el : m_channels) {
                el.chn->PassDeviceDesc(GetDesc());
            }
        }
        Start();
        return true;
    }
    bool IsInited() const { return m_device.state.value != ma_device_state_uninitialized; }
    void UnInit() {
        if (IsInited()) {
            LOG_INFO("uninit sound device");
        }
        UnmountAll();
        ma_device_uninit(&m_device); // always do it
    }
    // bool IsStarted() const { return ma_device_is_started(&m_device); }
    // bool IsStopped() const { return ma_device_get_state(&m_device) == MA_STATE_STOPPED; }
    void Start() {
        m_running = true;
        /*
        if(!IsStopped()) return;
        LOG_INFO("state: %d", ma_device_get_state(&m_device));
        if (ma_device_start(&m_device) != MA_SUCCESS) {
            LOG_ERROR("can't start sound device");
            //ma_device_uninit(&m_device);
        }
        */
    }
    void Stop() {
        m_running = false;
        /*
        if(!IsStarted()) return;
        LOG_INFO("state: %d", ma_device_get_state(&m_device));
        if(ma_device_stop(&m_device) != MA_SUCCESS){
            LOG_ERROR("can't stop sound device");
        }*/
    }
    float Volume() const { return m_volume.load(std::memory_order_relaxed); }
    bool  Muted() const { return m_muted.load(std::memory_order_relaxed); }
    void  SetMuted(bool v) { m_muted.store(v, std::memory_order_relaxed); }

    void SetVolume(float v) { m_volume.store(v, std::memory_order_relaxed); };

    using SpectrumCallback = std::function<void(const float*, uint32_t, uint32_t)>;
    // Spectrum callback is reassigned from the main looper and invoked from
    // the audio thread inside ProcessFrame. Take a dedicated mutex on both
    // sides so a move-assign can't tear into the audio thread's in-flight
    // read of std::function internals.
    void SetSpectrumCallback(SpectrumCallback cb) {
        std::lock_guard<std::mutex> lock(m_spectrum_callback_mutex);
        m_spectrum_callback = std::move(cb);
    }
    void MountChannel(std::shared_ptr<Channel> chn) {
        ChannelWrap chnw;
        chnw.chn = chn;
        chnw.chn->PassDeviceDesc(GetDesc());
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            m_channels.push_back(chnw);
        }
    }
    void UnmountAll() {
        {
            std::unique_lock<std::mutex> lock { m_mutex };
            m_channels.clear();
        }
    }
    DeviceDesc GetDesc() const {
        return DeviceDesc { .phyChannels = m_device.playback.channels,
                            .sampleRate  = m_device.sampleRate };
    }

public:
    // Public for testing.  Drives one frame of mixing + spectrum dispatch +
    // mute-aware output writing, using the same logic the real ma_device
    // data callback runs.  Tests can feed synthetic channels and observe
    // both the output buffer and the spectrum callback without standing up
    // a real audio backend.
    void ProcessFrame(void* pOutput, ma_uint32 frameCount, ma_uint32 phyChannels) {
        if (! m_running) return;
        const auto framesSize     = frameCount * phyChannels;
        const auto framesByteSize = framesSize * sizeof(float);
        {
            if (m_frameBuffer.size() < framesByteSize) m_frameBuffer.resize(framesByteSize);
            if (m_mixBuffer.size() < framesSize) m_mixBuffer.resize(framesSize);
            std::memset(m_mixBuffer.data(), 0, m_mixBuffer.size() * sizeof(float));
        }
        // Always decode channels into a private mix buffer so the spectrum
        // callback gets fresh PCM even when muted — wallpapers like Cyberpunk
        // Lucy (2866203962) drive UI fades from `engine.registerAudioBuffers`,
        // and reactivity must outlive the user's mute toggle.
        {
            std::unique_lock<std::mutex> lock { m_mutex };

            // Snapshot the atomic volume once per frame — the writer
            // (SetVolume from the main looper) races the audio-thread reader,
            // but the per-channel mix loop wants a stable scalar.
            const float volume   = m_volume.load(std::memory_order_relaxed);
            float* pBuffer_float = reinterpret_cast<float*>(m_frameBuffer.data());
            for (size_t i = 0; i < m_channels.size(); i++) {
                ma_uint64 framesReaded =
                    m_channels[i].chn->NextPcmData(m_frameBuffer.data(), frameCount);
                if (framesReaded == 0) {
                    m_channels[i].end = true;
                } else {
                    for (size_t s = 0; s < framesSize; s++)
                        m_mixBuffer[s] += volume * pBuffer_float[s];
                }
            }
            m_channels.erase(std::remove_if(m_channels.begin(),
                                            m_channels.end(),
                                            [](auto& c) {
                                                return c.end;
                                            }),
                             m_channels.end());
        }
        if (m_muted.load(std::memory_order_relaxed)) {
            std::memset(pOutput, 0, framesByteSize);
        } else {
            std::memcpy(pOutput, m_mixBuffer.data(), framesByteSize);
        }
        // Copy under lock, invoke unlocked — keeps the callback body off
        // the critical mix path (m_mutex). A torn read of the std::function
        // via concurrent SetSpectrumCallback would dereference a moved-from
        // lambda capture.
        SpectrumCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(m_spectrum_callback_mutex);
            cb_copy = m_spectrum_callback;
        }
        if (cb_copy) {
            cb_copy(m_mixBuffer.data(), frameCount, phyChannels);
        }
    }
    // Test-only: force-running so ProcessFrame won't bail.  Real devices
    // flip this via Start() in the Init path.
    void TestSetRunning(bool v) { m_running = v; }

private:
    static void data_callback(ma_device* pMaDevice, void* pOutput, const void* pInput,
                              ma_uint32 frameCount) {
        Device* pDevice = static_cast<Device*>(pMaDevice->pUserData);
        if (! pDevice->IsInited()) return;
        (void)pInput;
        pDevice->ProcessFrame(pOutput, frameCount, pMaDevice->playback.channels);
    }
    ma_device_config GenMaDeviceConfig(const DeviceDesc& d) {
        ma_device_config config  = ma_device_config_init(ma_device_type_playback);
        config.sampleRate        = d.sampleRate;
        config.playback.format   = ma_format_f32;
        config.playback.channels = d.phyChannels;
        config.dataCallback      = data_callback;
        config.pUserData         = (void*)this;
        return config;
    }

private:
    struct ChannelWrap {
        bool                     end { false };
        std::shared_ptr<Channel> chn;
    };
    ma_device         m_device {}; // must init c struct
    std::mutex        m_mutex;     // for operating channel vector
    std::atomic<bool> m_running { false };

    // Atomic so SetVolume/SetMuted (called from the main looper) don't tear
    // against the audio-thread data callback's reads inside ProcessFrame.
    // Relaxed is sufficient — these are independent values, not gating any
    // other read.
    std::atomic<float> m_volume { 1.0f };
    std::atomic<bool>  m_muted { false };

    std::vector<ChannelWrap> m_channels;
    std::vector<uint8_t>     m_frameBuffer;
    std::vector<float>       m_mixBuffer;

    // Independent mutex so SetSpectrumCallback doesn't contend with the
    // per-frame channel-mix lock on m_mutex.
    std::mutex       m_spectrum_callback_mutex;
    SpectrumCallback m_spectrum_callback;
};

} // namespace miniaudio
