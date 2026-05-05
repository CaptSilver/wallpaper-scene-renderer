// libFuzzer entry point for WPParticleParser's JSON-driven factories.
// genParticleInitOp + genParticleOperatorOp build particle ops from JSON
// — these are where particle-config bugs land (initializer/operator
// type dispatch, color/range parsing, instanceoverride application).

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "WPParticleParser.hpp"
#include "wpscene/WPParticleObject.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 256u * 1024u) return 0;
    std::string buf(reinterpret_cast<const char*>(data), size);

    auto j = nlohmann::json::parse(buf, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return 0;

    try {
        (void)wallpaper::WPParticleParser::genParticleInitOp(j);
        wallpaper::wpscene::ParticleInstanceoverride empty_override;
        (void)wallpaper::WPParticleParser::genParticleOperatorOp(j, empty_override);
    } catch (...) {
        // Production wraps these in no-throw envelopes; swallow exceptions
        // here so only crashes/sanitizer trips fail the fuzz run.
    }
    return 0;
}
