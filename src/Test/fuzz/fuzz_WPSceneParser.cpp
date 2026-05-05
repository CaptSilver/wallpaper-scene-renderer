// libFuzzer entry point for the JSON-driven scene-object construction
// path. Fuzzes wpscene::WPScene::FromJson directly — the root of
// WPSceneParser::Parse — without the VFS/SoundManager/sub-parser graph.
// This is where the JSON-count-driven resize bug class lives.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "wpscene/WPScene.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1u * 1024u * 1024u) return 0;
    std::string buf(reinterpret_cast<const char*>(data), size);

    auto j = nlohmann::json::parse(buf, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;

    wallpaper::wpscene::WPScene sc;
    try {
        sc.FromJson(j);
    } catch (...) {
        // FromJson methods can throw on malformed types; the production path
        // wraps the call in a no-throw envelope. Swallow here so the harness
        // only flags genuine crashes / sanitizer trips.
    }
    return 0;
}
