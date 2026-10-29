#pragma once
#include <pulse/def.h>

namespace wallpaper
{
namespace audio
{

// Whether a PulseAudio subscription event should make the capture re-check the
// default sink.  Sink unload (the user removing an EasyEffects sink, say) is
// what makes the SINK facility necessary: SERVER events alone don't fire on
// sink removal.  The event type carries the facility in the bits under
// PA_SUBSCRIPTION_EVENT_FACILITY_MASK and the new/change/remove kind above
// them, so the facility has to be masked out before comparing.
inline bool ShouldRecheckDefaultSink(pa_subscription_event_type_t t) {
    const auto facility = static_cast<unsigned>(t) & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    return facility == PA_SUBSCRIPTION_EVENT_SERVER || facility == PA_SUBSCRIPTION_EVENT_SINK;
}

} // namespace audio
} // namespace wallpaper
