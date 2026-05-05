// libFuzzer entry point for wpscene::WPSoundObject::FromJson — the
// JSON-driven scene-side counterpart of WPSoundParser. The Parse function
// itself takes a SoundManager + VFS + already-parsed object, so the
// interesting JSON-config bug surface is in FromJson.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "Fs/VFS.h"
#include "wpscene/WPSoundObject.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 256u * 1024u) return 0;
    std::string buf(reinterpret_cast<const char*>(data), size);

    auto j = nlohmann::json::parse(buf, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;

    wallpaper::fs::VFS              vfs;
    wallpaper::wpscene::WPSoundObject so;
    try {
        so.FromJson(j, vfs);
    } catch (...) {}
    return 0;
}
