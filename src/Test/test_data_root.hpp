#pragma once
// test_data_root() — resolve the path to the parent-repo `tests/fixtures/`
// directory at test runtime. The fixtures live in the parent repo (the
// submodule is bind-mounted under src/backend_scene/), so the helper has
// to walk up from CWD to find the parent tree. Used by the fuzz-crash
// regression doctest harness in each test_WP*Parser.cpp.
//
// Resolution order:
//   1. env var WEK_TEST_DATA_ROOT (full path to tests/fixtures).
//   2. build-time -DWEK_TEST_DATA_ROOT macro (compile-time injection).
//   3. walk up from CWD until tests/fixtures/ is found (≤ 8 levels).
//
// All three forms return std::filesystem::path. The walk-up fallback
// is the practical default for `ctest` and direct binary runs from
// build/sub/.

#include <cstdlib>
#include <filesystem>
#include <string>

namespace wallpaper::test {

inline std::filesystem::path test_data_root() {
    if (auto* env = std::getenv("WEK_TEST_DATA_ROOT"); env && *env)
        return std::filesystem::path(env);
#ifdef WEK_TEST_DATA_ROOT
    return std::filesystem::path(WEK_TEST_DATA_ROOT);
#else
    namespace fs = std::filesystem;
    auto p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "tests" / "fixtures"))
            return p / "tests" / "fixtures";
        if (! p.has_parent_path()) break;
        p = p.parent_path();
    }
    return fs::path("tests") / "fixtures";
#endif
}

} // namespace wallpaper::test
