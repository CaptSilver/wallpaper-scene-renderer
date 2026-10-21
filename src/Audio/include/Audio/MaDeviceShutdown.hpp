#pragma once

#include <miniaudio/miniaudio.h>

namespace wallpaper
{
namespace audio
{

// Use this instead of a bare ma_device_uninit for any device that may be
// running.  On the PulseAudio backend the worker thread sleeps in
// pa_mainloop_iterate until the server sends something, and ma_device_uninit
// only flips the state and joins it.  The call that wakes that loop lives in
// ma_device_stop, which miniaudio's uninit no longer makes, so the join lasts
// until the next server event.  A capture stream on the monitor of an idle
// sink never gets one, and the join never returns.
inline void StopAndUninitDevice(ma_device* device) {
    if (ma_device_is_started(device)) ma_device_stop(device);
    ma_device_uninit(device);
}

} // namespace audio
} // namespace wallpaper
