#include "Audio/AudioCapture.h"
#include "Audio/AudioAnalyzer.h"
#include "Utils/Logging.h"

// miniaudio header-only (implementation already in miniaudio-wrapper.hpp)
#include <miniaudio/miniaudio.h>

#include <cstring>
#include <string>
#include <atomic>

using namespace wallpaper::audio;

struct AudioCapture::Impl {
    ma_context                     context {};
    ma_device                      device {};
    std::shared_ptr<AudioAnalyzer> analyzer;
    std::atomic<bool>              active { false };
    bool                           contextInited { false };
    bool                           deviceInited { false };

    static void captureCallback(ma_device* pDevice, void* pOutput, const void* pInput,
                                ma_uint32 frameCount) {
        (void)pOutput;
        auto* impl = static_cast<Impl*>(pDevice->pUserData);
        if (impl->active && pInput) {
            const float* input = static_cast<const float*>(pInput);
            impl->analyzer->FeedPcm(input, frameCount, pDevice->capture.channels);
        }
    }
};

AudioCapture::AudioCapture(): m_impl(std::make_unique<Impl>()) {}

AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Init(std::shared_ptr<AudioAnalyzer> analyzer) {
    if (! analyzer) return false;
    m_impl->analyzer = analyzer;

    // Initialize miniaudio context (separate from playback context)
    ma_context_config ctxConfig  = ma_context_config_init();
    ma_backend        backends[] = { ma_backend_pulseaudio };
    ma_result         result     = ma_context_init(backends, 1, &ctxConfig, &m_impl->context);
    if (result != MA_SUCCESS) {
        LOG_INFO("AudioCapture: Failed to init PulseAudio context (result=%d), "
                 "system audio capture unavailable",
                 (int)result);
        return false;
    }
    m_impl->contextInited = true;

    // Enumerate capture devices, find a .monitor source
    ma_device_info* pCaptureInfos = nullptr;
    ma_uint32       captureCount  = 0;
    result =
        ma_context_get_devices(&m_impl->context, nullptr, nullptr, &pCaptureInfos, &captureCount);
    if (result != MA_SUCCESS) {
        LOG_INFO("AudioCapture: Failed to enumerate capture devices");
        return false;
    }

    int monitorIdx = -1;
    for (ma_uint32 i = 0; i < captureCount; i++) {
        std::string name(pCaptureInfos[i].name);
        LOG_INFO("AudioCapture: capture device [%u] = '%s'", i, name.c_str());
        // PulseAudio uses ".monitor" suffix, PipeWire uses "Monitor of " prefix
        bool isMonitor =
            name.find(".monitor") != std::string::npos || name.find("Monitor of ") == 0;
        if (isMonitor) {
            monitorIdx = (int)i;
            // Don't break — prefer the last monitor (usually the default sink)
        }
    }

    if (monitorIdx < 0) {
        LOG_INFO("AudioCapture: No .monitor source found, system audio capture unavailable");
        return false;
    }

    LOG_INFO("AudioCapture: Using monitor source '%s'", pCaptureInfos[monitorIdx].name);

    // Open capture device
    ma_device_config config  = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = &pCaptureInfos[monitorIdx].id;
    config.capture.format    = ma_format_f32;
    config.capture.channels  = 2;
    config.sampleRate        = 48000;
    config.dataCallback      = Impl::captureCallback;
    config.pUserData         = m_impl.get();

    result = ma_device_init(&m_impl->context, &config, &m_impl->device);
    if (result != MA_SUCCESS) {
        LOG_INFO("AudioCapture: Failed to init capture device (result=%d)", (int)result);
        return false;
    }
    m_impl->deviceInited = true;

    result = ma_device_start(&m_impl->device);
    if (result != MA_SUCCESS) {
        LOG_INFO("AudioCapture: Failed to start capture device (result=%d)", (int)result);
        ma_device_uninit(&m_impl->device);
        m_impl->deviceInited = false;
        return false;
    }

    m_impl->active = true;
    LOG_INFO("AudioCapture: System audio capture active (%u Hz, %u ch)",
             m_impl->device.sampleRate,
             m_impl->device.capture.channels);
    return true;
}

bool AudioCapture::IsActive() const { return m_impl->active; }

void AudioCapture::Stop() {
    m_impl->active = false;
    if (m_impl->deviceInited) {
        ma_device_uninit(&m_impl->device);
        m_impl->deviceInited = false;
    }
    if (m_impl->contextInited) {
        ma_context_uninit(&m_impl->context);
        m_impl->contextInited = false;
    }
}
