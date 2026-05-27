// libFuzzer entry point for the source-level JSON parser path
// (wallpaper::ParseJson).  Drives raw bytes through the SAX-driven gate so
// any future nlohmann update that quietly relaxes the depth / element /
// byte caps lights up as a regression.  Excluded from ALL — build with
// `cmake --build ... --target fuzz_WPJsonParse` or the `fuzzers` umbrella
// target.

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "WPJson.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Mirror the production call shape: PARSE_JSON forwards file/func/line
    // via __SHORT_FILE__/__FUNCTION__/__LINE__.
    std::string    src(reinterpret_cast<const char*>(data), size);
    nlohmann::json out;
    (void)wallpaper::ParseJson(__FILE__, __FUNCTION__, __LINE__, src, out);
    return 0;
}
