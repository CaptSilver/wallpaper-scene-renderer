#include <doctest.h>

// The PulseAudio device-info callbacks are static inside the miniaudio
// implementation, so this TU compiles its own copy of it with every API
// function made static too.  That copy can't clash with the one SoundManager.cpp
// links into wpAudio, and it lets the test call the callbacks the way libpulse
// does, without a running sound server.  The atomic spinlock is the one global
// miniaudio defines without MA_API, so it needs its own name here.
#define MA_API                static
#define ma_atomic_global_lock wek_test_ma_atomic_global_lock
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio/miniaudio.h>

// libpulse reports a failed lookup (the server answered "no such entity") by
// calling the info callback with a NULL info pointer and eol = -1.  That is
// what happens when the rebind worker asks for a sink's .monitor while a
// Bluetooth headset is still coming up: miniaudio 0.11.25 dereferenced the
// NULL and took plasmashell down.  A failed lookup has to read as "no device".
TEST_SUITE("miniaudio.PulseDeviceInfo") {
    TEST_CASE("source lookup error reply (NULL info, eol=-1) is not a device") {
        ma_device_info                                  info {};
        ma_context_get_device_info_callback_data__pulse data {};
        data.pDeviceInfo = &info;

        ma_context_get_device_info_source_callback__pulse(nullptr, nullptr, -1, &data);

        CHECK_FALSE(data.foundDevice);
        CHECK(info.nativeDataFormatCount == 0);
    }

    TEST_CASE("sink lookup error reply (NULL info, eol=-1) is not a device") {
        ma_device_info                                  info {};
        ma_context_get_device_info_callback_data__pulse data {};
        data.pDeviceInfo = &info;

        ma_context_get_device_info_sink_callback__pulse(nullptr, nullptr, -1, &data);

        CHECK_FALSE(data.foundDevice);
        CHECK(info.nativeDataFormatCount == 0);
    }
}
