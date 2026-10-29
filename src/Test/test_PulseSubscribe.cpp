#include <doctest.h>

#include <initializer_list>

#if __has_include(<pulse/def.h>)
#    include "Audio/PulseSubscribe.hpp"

using wallpaper::audio::ShouldRecheckDefaultSink;

namespace
{
constexpr pa_subscription_event_type_t Ev(unsigned facility, unsigned kind) {
    return static_cast<pa_subscription_event_type_t>(facility | kind);
}
} // namespace

TEST_SUITE("AudioCapture.ShouldRecheckDefaultSink") {
    TEST_CASE("server and sink events recheck, whatever the event kind") {
        for (unsigned kind : { (unsigned)PA_SUBSCRIPTION_EVENT_NEW,
                               (unsigned)PA_SUBSCRIPTION_EVENT_CHANGE,
                               (unsigned)PA_SUBSCRIPTION_EVENT_REMOVE }) {
            CHECK(ShouldRecheckDefaultSink(Ev(PA_SUBSCRIPTION_EVENT_SERVER, kind)));
            CHECK(ShouldRecheckDefaultSink(Ev(PA_SUBSCRIPTION_EVENT_SINK, kind)));
        }
    }

    TEST_CASE("other facilities do not recheck") {
        for (unsigned facility : { (unsigned)PA_SUBSCRIPTION_EVENT_SOURCE,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_SINK_INPUT,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_MODULE,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_CLIENT,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_CARD }) {
            for (unsigned kind : { (unsigned)PA_SUBSCRIPTION_EVENT_NEW,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_CHANGE,
                                   (unsigned)PA_SUBSCRIPTION_EVENT_REMOVE }) {
                CHECK_FALSE(ShouldRecheckDefaultSink(Ev(facility, kind)));
            }
        }
    }
}
#else
TEST_CASE("AudioCapture.ShouldRecheckDefaultSink" * doctest::skip()) {
    MESSAGE("libpulse headers not found; the PulseAudio event filter is not built");
}
#endif
