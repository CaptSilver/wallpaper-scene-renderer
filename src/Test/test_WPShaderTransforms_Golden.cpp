// Golden-snapshot tests for WPShaderTransforms.h string rewrites.
//
// For each <name>.hlsl under tests/fixtures/shaders/**, applies the
// transform that <name> selects (clip-discard -> TranslateHlslClip,
// vec2-swizzle-upgrade / uint-modulo / vec4-truncate -> FixImplicitConversions,
// geom-shader-rename -> TranslateGeometryShader, combine-alpha ->
// FixCombineAlpha), then diffs the result against <name>.glsl.expected.
//
// Mismatch -> write <name>.glsl.actual next to the .glsl.expected and FAIL
// with a unified-diff hint.
//
// Regenerate the goldens (only when an intentional translator change lands):
//   WEK_GOLDEN_REGEN=1 ./backend_scene_tests --test-suite="WPShaderTransforms.golden"
//   or:   cmake --build build/sub --target shader-golden-regen
//
// CI must NOT set WEK_GOLDEN_REGEN -- the explicit env var prevents silent
// drift.  Whitespace normalised (trailing whitespace + trailing blank lines)
// so editor whitespace churn is invisible.

#include <doctest.h>
#include "WPShaderTransforms.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

// Strip trailing whitespace per line + trailing blank lines.  Insulates the
// goldens from editor whitespace churn without weakening the structural
// guarantee.
std::string normalise(const std::string& s) {
    std::vector<std::string> lines;
    std::stringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'
                                 || line.back() == '\r')) {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::stringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) out << '\n';
    }
    return out.str();
}

// Map fixture basename to the transform under test.  Adding a new fixture
// requires adding a new arm here; that's deliberate -- keeps the harness
// dispatch explicit.
std::string applyTransform(const std::string& stem, const std::string& src) {
    if (stem == "clip-discard") {
        return TranslateHlslClip(src);
    }
    if (stem == "vec2-swizzle-upgrade" || stem == "uint-modulo"
        || stem == "vec4-truncate") {
        return FixImplicitConversions(src);
    }
    if (stem == "geom-shader-rename") {
        return TranslateGeometryShader(src);
    }
    if (stem == "combine-alpha") {
        return FixCombineAlpha(src);
    }
    // Unknown stem -- pass through and fail the diff so the author notices.
    return src;
}

std::vector<fs::path> enumerateFixtures(const fs::path& root) {
    std::vector<fs::path> out;
    if (!fs::exists(root)) return out;
    for (auto& e : fs::recursive_directory_iterator(root)) {
        if (e.is_regular_file() && e.path().extension() == ".hlsl") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Locate fixtures dir.  The build pins the source-tree absolute path via
// WEK_FIXTURES_DIR (compile-time #define); the env var
// WEK_FIXTURES_DIR_OVERRIDE wins over both for ad-hoc relocations.  As a
// last resort we walk a small list of cwd-relative candidates so a
// developer running the binary by hand still finds the corpus.
fs::path locateFixturesRoot() {
    if (const char* env = std::getenv("WEK_FIXTURES_DIR_OVERRIDE")) {
        fs::path p = fs::path(env) / "shaders" / "effects";
        if (fs::exists(p)) return p;
    }
#ifdef WEK_FIXTURES_DIR
    {
        fs::path p = fs::path(WEK_FIXTURES_DIR) / "shaders" / "effects";
        if (fs::exists(p)) return p;
    }
#endif
    const std::vector<fs::path> candidates = {
        fs::path("tests/fixtures/shaders/effects"),
        fs::path("../tests/fixtures/shaders/effects"),
        fs::path("../../tests/fixtures/shaders/effects"),
        fs::path("../../../tests/fixtures/shaders/effects"),
        fs::path("../../../../tests/fixtures/shaders/effects"),
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return c;
    }
    return {};
}

} // namespace

TEST_SUITE("WPShaderTransforms.golden") {
    TEST_CASE("golden corpus") {
        const fs::path fixturesRoot = locateFixturesRoot();
        const bool fixturesRootOk
            = !fixturesRoot.empty() && fs::exists(fixturesRoot);
        REQUIRE_MESSAGE(fixturesRootOk,
            "fixtures dir not found; set WEK_FIXTURES_DIR_OVERRIDE to "
            "<submodule>/tests/fixtures");

        const bool regen = std::getenv("WEK_GOLDEN_REGEN") != nullptr;
        int filesSeen = 0;

        for (const auto& hlsl : enumerateFixtures(fixturesRoot)) {
            ++filesSeen;
            const std::string stem = hlsl.stem().string();
            const fs::path expected
                = hlsl.parent_path() / (stem + ".glsl.expected");
            const fs::path actual
                = hlsl.parent_path() / (stem + ".glsl.actual");

            const std::string got = normalise(applyTransform(stem, slurp(hlsl)));

            if (regen) {
                writeFile(expected, got);
                MESSAGE("REGEN: wrote " << expected.string());
                continue;
            }

            if (!fs::exists(expected)) {
                writeFile(actual, got);
                FAIL_CHECK("missing golden: " << expected.string()
                    << " -- regenerate with WEK_GOLDEN_REGEN=1 or "
                    << "`cmake --build <dir> --target shader-golden-regen`. "
                    << "Actual at " << actual.string());
                continue;
            }

            const std::string want = normalise(slurp(expected));
            if (got != want) {
                writeFile(actual, got);
                FAIL_CHECK("golden mismatch for " << hlsl.string()
                    << " -- see " << actual.string()
                    << "; diff against " << expected.string()
                    << "; regenerate with WEK_GOLDEN_REGEN=1.");
            } else {
                std::error_code ec;
                fs::remove(actual, ec);
            }
        }

        CHECK_MESSAGE(filesSeen >= 1,
            "no .hlsl fixtures found under " << fixturesRoot.string());
    }
}
