#include "Audio/AudioCapture.h"
#include "Audio/AudioAnalyzer.h"
#include "Audio/RebindRetry.hpp"
#include "Utils/Logging.h"

// miniaudio header-only (implementation already in miniaudio-wrapper.hpp)
#include <miniaudio/miniaudio.h>

#ifdef WEK_HAVE_LIBPULSE
#    include <pulse/pulseaudio.h>
#    include <pulse/thread-mainloop.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace wallpaper::audio;

namespace
{

constexpr const char* kPulseClientName = "wallpaper-engine-kde-plugin";

#ifdef WEK_HAVE_LIBPULSE
// Lock + signal helpers — pa_threaded_mainloop_signal must always be paired
// with the mainloop lock being held on the caller side; we centralize that.
struct PALock {
    pa_threaded_mainloop* loop;
    explicit PALock(pa_threaded_mainloop* l): loop(l) { pa_threaded_mainloop_lock(loop); }
    ~PALock() { pa_threaded_mainloop_unlock(loop); }
};
#endif

} // namespace

struct AudioCapture::Impl {
    // ---- miniaudio: actual PCM capture ----
    ma_context maContext {};
    ma_device  maDevice {};
    bool       maContextInited { false };
    bool       maDeviceInited { false };

    std::shared_ptr<AudioAnalyzer> analyzer;
    std::atomic<bool>              active { false };

#ifdef WEK_HAVE_LIBPULSE
    // ---- libpulse: default-sink discovery + change subscription ----
    pa_threaded_mainloop* paLoop { nullptr };
    pa_context*           paContext { nullptr };
    bool                  paReady { false };

    // ---- rebind worker (we can't manipulate ma_device from PA callback) ----
    std::thread             rebindThread;
    std::mutex              rebindMutex;
    std::condition_variable rebindCV;
    std::string             targetSink; // protected by rebindMutex
    std::string             boundSink;  // touched only by rebindThread + Init
    std::atomic<bool>       rebindPending { false };
    std::atomic<bool>       shutdown { false };
#endif

    static void captureCallback(ma_device* pDevice, void* pOutput, const void* pInput,
                                ma_uint32 frameCount) {
        (void)pOutput;
        auto* impl = static_cast<Impl*>(pDevice->pUserData);
        if (impl->active && pInput) {
            const float* input = static_cast<const float*>(pInput);
            impl->analyzer->FeedPcm(input, frameCount, pDevice->capture.channels);
        }
    }

    bool OpenMaDevice(const std::string& sinkName) {
        if (maDeviceInited) {
            ma_device_uninit(&maDevice);
            maDeviceInited = false;
        }
        std::string  monitorName = sinkName + ".monitor";
        ma_device_id devId {};
        std::strncpy(devId.pulse, monitorName.c_str(), sizeof(devId.pulse) - 1);

        ma_device_config cfg  = ma_device_config_init(ma_device_type_capture);
        cfg.capture.pDeviceID = &devId;
        cfg.capture.format    = ma_format_f32;
        cfg.capture.channels  = 2;
        cfg.sampleRate        = 48000;
        cfg.dataCallback      = captureCallback;
        cfg.pUserData         = this;

        ma_result r = ma_device_init(&maContext, &cfg, &maDevice);
        if (r != MA_SUCCESS) {
            LOG_INFO(
                "AudioCapture: ma_device_init('%s') failed (r=%d)", monitorName.c_str(), (int)r);
            return false;
        }
        maDeviceInited = true;
        r              = ma_device_start(&maDevice);
        if (r != MA_SUCCESS) {
            LOG_INFO("AudioCapture: ma_device_start failed (r=%d)", (int)r);
            ma_device_uninit(&maDevice);
            maDeviceInited = false;
            return false;
        }
        return true;
    }

    // Fallback when libpulse is not available — enumerate via miniaudio and pick
    // any .monitor source.  No live re-bind; user has to restart plasmashell if
    // the chosen source disappears.
    bool InitLegacy() {
        ma_device_info* pCaptureInfos = nullptr;
        ma_uint32       captureCount  = 0;
        if (ma_context_get_devices(&maContext, nullptr, nullptr, &pCaptureInfos, &captureCount) !=
            MA_SUCCESS) {
            LOG_INFO("AudioCapture: ma_context_get_devices failed in legacy path");
            return false;
        }

        int monitorIdx = -1;
        for (ma_uint32 i = 0; i < captureCount; i++) {
            std::string name(pCaptureInfos[i].name);
            // PulseAudio descriptions: "Monitor of <sink description>"
            bool isMonitor =
                name.find(".monitor") != std::string::npos || name.find("Monitor of ") == 0;
            if (isMonitor) {
                monitorIdx = (int)i;
            }
        }
        if (monitorIdx < 0) {
            LOG_INFO("AudioCapture: no monitor source found in %u capture devices "
                     "(legacy enumeration)",
                     (unsigned)captureCount);
            return false;
        }

        ma_device_config cfg  = ma_device_config_init(ma_device_type_capture);
        cfg.capture.pDeviceID = &pCaptureInfos[monitorIdx].id;
        cfg.capture.format    = ma_format_f32;
        cfg.capture.channels  = 2;
        cfg.sampleRate        = 48000;
        cfg.dataCallback      = captureCallback;
        cfg.pUserData         = this;
        ma_result r           = ma_device_init(&maContext, &cfg, &maDevice);
        if (r != MA_SUCCESS) {
            LOG_INFO("AudioCapture: legacy ma_device_init failed (r=%d, device='%s')",
                     (int)r,
                     pCaptureInfos[monitorIdx].name);
            return false;
        }
        maDeviceInited = true;
        r              = ma_device_start(&maDevice);
        if (r != MA_SUCCESS) {
            LOG_INFO("AudioCapture: legacy ma_device_start failed (r=%d, device='%s')",
                     (int)r,
                     pCaptureInfos[monitorIdx].name);
            ma_device_uninit(&maDevice);
            maDeviceInited = false;
            return false;
        }
        return true;
    }

#ifdef WEK_HAVE_LIBPULSE
    static void paContextStateCb(pa_context* c, void* userdata) {
        auto*              impl = static_cast<Impl*>(userdata);
        pa_context_state_t st   = pa_context_get_state(c);
        if (st == PA_CONTEXT_READY || st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED)
            pa_threaded_mainloop_signal(impl->paLoop, 0);
    }

    // server-info callback used by both initial query and subscription
    struct ServerInfoQuery {
        std::string           name;
        bool                  done { false };
        bool                  ok { false };
        pa_threaded_mainloop* loop { nullptr };
    };

    static void paQueryServerInfoCb(pa_context* /*c*/, const pa_server_info* info, void* userdata) {
        auto* q = static_cast<ServerInfoQuery*>(userdata);
        if (info && info->default_sink_name) {
            q->name = info->default_sink_name;
            q->ok   = true;
        }
        q->done = true;
        pa_threaded_mainloop_signal(q->loop, 0);
    }

    // server-info callback used by the subscription path: schedule a rebind if
    // the default sink moved.
    static void paSubscribedServerInfoCb(pa_context* /*c*/, const pa_server_info* info,
                                         void* userdata) {
        auto* impl = static_cast<Impl*>(userdata);
        if (! info || ! info->default_sink_name) return;
        std::string newSink = info->default_sink_name;
        {
            std::lock_guard<std::mutex> lk(impl->rebindMutex);
            if (newSink == impl->targetSink) return;
            impl->targetSink = newSink;
        }
        impl->rebindPending = true;
        impl->rebindCV.notify_one();
    }

    static void paSubscribeCb(pa_context* c, pa_subscription_event_type_t t, uint32_t /*idx*/,
                              void* userdata) {
        // Any server/sink event triggers a default-sink re-check.  Sink unload
        // (e.g. user removes EasyEffects sink) is what makes this necessary —
        // SERVER events alone don't fire on sink removal.
        auto facility = static_cast<unsigned>(t) & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SERVER || facility == PA_SUBSCRIPTION_EVENT_SINK) {
            pa_operation* op = pa_context_get_server_info(c, paSubscribedServerInfoCb, userdata);
            if (op) pa_operation_unref(op);
        }
    }

    bool ConnectPulse() {
        paLoop = pa_threaded_mainloop_new();
        if (! paLoop) return false;
        if (pa_threaded_mainloop_start(paLoop) < 0) {
            pa_threaded_mainloop_free(paLoop);
            paLoop = nullptr;
            return false;
        }
        // If anything inside the PALock block fails, set failed=true so the
        // post-lock cleanup can stop and free the now-orphan threaded
        // mainloop. Without it, the idle PA worker thread runs for the full
        // scene lifetime until Stop() eventually disposes of it (the
        // previous early-returns from inside PALock leaked paLoop).
        bool failed = false;
        {
            PALock lk(paLoop);
            paContext = pa_context_new(pa_threaded_mainloop_get_api(paLoop), kPulseClientName);
            if (! paContext) {
                failed = true;
            } else {
                pa_context_set_state_callback(paContext, paContextStateCb, this);
                if (pa_context_connect(paContext, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                    pa_context_unref(paContext);
                    paContext = nullptr;
                    failed    = true;
                } else {
                    while (true) {
                        pa_context_state_t st = pa_context_get_state(paContext);
                        if (st == PA_CONTEXT_READY) {
                            paReady = true;
                            break;
                        }
                        if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED) break;
                        pa_threaded_mainloop_wait(paLoop);
                    }
                    if (paReady) {
                        pa_context_set_subscribe_callback(paContext, paSubscribeCb, this);
                        pa_operation* op = pa_context_subscribe(
                            paContext,
                            static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SERVER |
                                                                PA_SUBSCRIPTION_MASK_SINK),
                            nullptr,
                            nullptr);
                        if (op) pa_operation_unref(op);
                    }
                }
            }
        }
        if (failed) {
            pa_threaded_mainloop_stop(paLoop);
            pa_threaded_mainloop_free(paLoop);
            paLoop = nullptr;
            return false;
        }
        return paReady;
    }

    bool QueryDefaultSinkBlocking(std::string& out) {
        if (! paReady) return false;
        ServerInfoQuery q;
        q.loop = paLoop;
        {
            PALock        lk(paLoop);
            pa_operation* op = pa_context_get_server_info(paContext, paQueryServerInfoCb, &q);
            if (! op) return false;
            while (! q.done) pa_threaded_mainloop_wait(paLoop);
            pa_operation_unref(op);
        }
        if (! q.ok) return false;
        out = std::move(q.name);
        return true;
    }

    void DisconnectPulse() {
        if (paLoop && paContext) {
            LOG_INFO("AudioCapture::DisconnectPulse: locking + disconnecting context...");
            PALock lk(paLoop);
            pa_context_disconnect(paContext);
            pa_context_unref(paContext);
            paContext = nullptr;
            paReady   = false;
            LOG_INFO("AudioCapture::DisconnectPulse: context torn down");
        }
        if (paLoop) {
            LOG_INFO("AudioCapture::DisconnectPulse: pa_threaded_mainloop_stop...");
            pa_threaded_mainloop_stop(paLoop);
            LOG_INFO("AudioCapture::DisconnectPulse: pa_threaded_mainloop_free...");
            pa_threaded_mainloop_free(paLoop);
            paLoop = nullptr;
            LOG_INFO("AudioCapture::DisconnectPulse: paLoop freed");
        }
    }

    void RebindLoop() {
        LOG_INFO("AudioCapture::RebindLoop: started (impl=%p)", (void*)this);
        while (! shutdown) {
            std::string newSink;
            {
                std::unique_lock<std::mutex> lk(rebindMutex);
                rebindCV.wait(lk, [this] {
                    return shutdown.load() || rebindPending.load();
                });
                LOG_INFO("AudioCapture::RebindLoop: woke (shutdown=%d, rebindPending=%d)",
                         (int)shutdown.load(), (int)rebindPending.load());
                if (shutdown) return;
                rebindPending = false;
                if (targetSink == boundSink) continue;
                newSink = targetSink;
            }
            LOG_INFO("AudioCapture: default sink changed → rebinding to monitor of '%s'",
                     newSink.c_str());
            // ma_device_uninit blocks until in-flight callbacks return, so it
            // is safe to interleave with the running capture.
            if (OpenMaDevice(newSink)) {
                boundSink = newSink;
            } else {
                // Initial open failed (sink unloaded mid-init, PA race, card
                // vanished).  Try a short exponential backoff before giving
                // up and waiting for the next PA event.  RetryOpenWithBackoff
                // aborts early on shutdown or on a newer rebindPending so we
                // don't block the worker indefinitely.
                const bool recovered = RetryOpenWithBackoff(
                    newSink, shutdown, rebindPending, rebindCV, rebindMutex,
                    [this](const std::string& s) { return this->OpenMaDevice(s); });
                if (recovered) boundSink = newSink;
                // No `else` log here — RetryOpenWithBackoff already emits the
                // terminal "rebind to '%s' failed after N retries" line on
                // giveup.
            }
        }
    }
#endif // WEK_HAVE_LIBPULSE
};

AudioCapture::AudioCapture(): m_impl(std::make_unique<Impl>()) {}

AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Init(std::shared_ptr<AudioAnalyzer> analyzer) {
    if (! analyzer) return false;
    m_impl->analyzer = analyzer;

    // Init miniaudio context against the PulseAudio backend (works on
    // PipeWire-pulse too).
    ma_context_config ctxConfig  = ma_context_config_init();
    ma_backend        backends[] = { ma_backend_pulseaudio };
    ma_result         result     = ma_context_init(backends, 1, &ctxConfig, &m_impl->maContext);
    if (result != MA_SUCCESS) {
        LOG_INFO("AudioCapture: Failed to init PulseAudio context (result=%d), "
                 "system audio capture unavailable",
                 (int)result);
        return false;
    }
    m_impl->maContextInited = true;

#ifdef WEK_HAVE_LIBPULSE
    if (m_impl->ConnectPulse()) {
        std::string defaultSink;
        if (! m_impl->QueryDefaultSinkBlocking(defaultSink) || defaultSink.empty()) {
            LOG_INFO("AudioCapture: no default sink reported by PulseAudio");
            return false;
        }
        if (! m_impl->OpenMaDevice(defaultSink)) {
            LOG_INFO("AudioCapture: failed to open monitor of default sink '%s'",
                     defaultSink.c_str());
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(m_impl->rebindMutex);
            m_impl->boundSink  = defaultSink;
            m_impl->targetSink = defaultSink;
        }
        m_impl->active       = true;
        m_impl->rebindThread = std::thread([impl = m_impl.get()] {
            impl->RebindLoop();
        });
        LOG_INFO("AudioCapture: active on monitor of '%s' (%u Hz, %u ch) — live-rebind enabled",
                 defaultSink.c_str(),
                 m_impl->maDevice.sampleRate,
                 m_impl->maDevice.capture.channels);
        return true;
    }
    LOG_INFO("AudioCapture: libpulse unavailable, falling back to one-shot enumeration");
#endif

    if (! m_impl->InitLegacy()) {
        LOG_INFO("AudioCapture: no monitor source found");
        return false;
    }
    m_impl->active = true;
    LOG_INFO("AudioCapture: active via legacy enumeration (%u Hz, %u ch) — no live-rebind",
             m_impl->maDevice.sampleRate,
             m_impl->maDevice.capture.channels);
    return true;
}

bool AudioCapture::IsActive() const { return m_impl->active; }

void AudioCapture::Stop() {
    LOG_INFO("AudioCapture::Stop: enter (this=%p)", (void*)this);
    m_impl->active = false;
#ifdef WEK_HAVE_LIBPULSE
    {
        // Acquire rebindMutex before signaling shutdown so the rebind
        // thread's predicate check observes the store; without this
        // ordering, a notify that races the wait() lock-release window
        // can be lost (the futex registration races the predicate flag
        // store on weak hardware).  Holding the lock during notify_all
        // is safe — wait() releases lk before parking, so the notifier
        // never serializes with a parked waiter.
        std::lock_guard<std::mutex> lk(m_impl->rebindMutex);
        m_impl->shutdown = true;
        m_impl->rebindCV.notify_all();
    }
    LOG_INFO("AudioCapture::Stop: joining rebind thread (joinable=%d)",
             (int)m_impl->rebindThread.joinable());
    if (m_impl->rebindThread.joinable()) m_impl->rebindThread.join();
    LOG_INFO("AudioCapture::Stop: rebind joined; DisconnectPulse...");
    m_impl->DisconnectPulse();
    LOG_INFO("AudioCapture::Stop: DisconnectPulse done");
#endif
    if (m_impl->maDeviceInited) {
        LOG_INFO("AudioCapture::Stop: ma_device_uninit...");
        ma_device_uninit(&m_impl->maDevice);
        m_impl->maDeviceInited = false;
        LOG_INFO("AudioCapture::Stop: ma_device_uninit done");
    }
    if (m_impl->maContextInited) {
        LOG_INFO("AudioCapture::Stop: ma_context_uninit...");
        ma_context_uninit(&m_impl->maContext);
        m_impl->maContextInited = false;
        LOG_INFO("AudioCapture::Stop: ma_context_uninit done");
    }
    LOG_INFO("AudioCapture::Stop: exit (this=%p)", (void*)this);
}
