// Pin the systemfont_* → Linux font path mapping logic.
//
// Fontconfig may choose Microsoft fonts, Croscore, or user-installed faces.
// Check its match for the requested family rather than guessing from filenames.

#include <doctest.h>

#include "SystemFontFallback.hpp"

#include <fontconfig/fontconfig.h>

#include <filesystem>
#include <string>
#include <utility>

using wallpaper::ReadSystemFile;
using wallpaper::ResolveSystemFontFallback;

namespace
{

bool MatchesFontconfig(const std::string& path, const char* name, const char* fallback_family) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(name));
    if (! pattern) return false;
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult   result;
    FcPattern* match  = FcFontMatch(nullptr, pattern, &result);
    FcChar8*   file   = nullptr;
    const bool usable = match && result == FcResultMatch &&
                        FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file &&
                        std::filesystem::exists(reinterpret_cast<const char*>(file));
    // Without usable Fontconfig data, production chooses Liberation/DejaVu.
    const std::string family(fallback_family);
    const auto        filename = std::filesystem::path(path).filename().string();
    const bool        matches =
        usable ? path == reinterpret_cast<const char*>(file)
               : filename == "Liberation" + family + "-Regular.ttf" ||
                     filename == "DejaVu" + (family == "Mono" ? "SansMono" : family) + ".ttf";
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return matches;
}

bool HasAnyLinuxFont() {
    namespace fs = std::filesystem;
    return fs::exists("/usr/share/fonts");
}

} // namespace

TEST_SUITE("SystemFontFallback") {
    TEST_CASE("consolas resolves to a monospaced font") {
        if (!HasAnyLinuxFont()) return;  // skip if /usr/share/fonts absent
        const auto p = ResolveSystemFontFallback("systemfont_consolas");
        if (p.empty()) {
            // No Liberation/DejaVu installed at all — acceptable on minimal containers.
            WARN_MESSAGE(false, "no fallback font installed; install liberation-mono-fonts to enable test");
            return;
        }
        CHECK(MatchesFontconfig(p, "Courier New:spacing=100", "Mono"));
    }

    TEST_CASE("courier_new resolves to a monospaced font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_couriernew");
        if (p.empty()) return;
        CHECK(MatchesFontconfig(p, "Courier New:spacing=100", "Mono"));
    }

    TEST_CASE("arial resolves to a sans-serif font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_arial");
        if (p.empty()) return;
        CHECK(MatchesFontconfig(p, "Arial", "Sans"));
    }

    TEST_CASE("times new roman resolves to a serif font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_timesnewroman");
        if (p.empty()) return;
        CHECK(MatchesFontconfig(p, "Times New Roman", "Serif"));
    }

    TEST_CASE("unknown systemfont_* defaults to sans-serif") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_madeupname");
        if (p.empty()) return;
        CHECK(MatchesFontconfig(p, "Arial", "Sans"));
    }

    TEST_CASE("curated fallback works without Fontconfig fonts") {
        struct ConfigGuard {
            FcConfig* previous = FcConfigReference(FcConfigGetCurrent());
            FcConfig* empty    = FcConfigCreate();
            ~ConfigGuard() {
                FcConfigSetCurrent(previous);
                FcConfigDestroy(previous);
                if (empty) FcConfigDestroy(empty);
            }
        } config;
        REQUIRE(config.empty != nullptr);
        REQUIRE(FcConfigSetCurrent(config.empty));
        for (const auto& request : { std::pair { "systemfont_consolas", "Mono" },
                                     std::pair { "systemfont_arial", "Sans" },
                                     std::pair { "systemfont_timesnewroman", "Serif" } }) {
            const auto p = ResolveSystemFontFallback(request.first);
            if (p.empty()) continue; // No curated font installed on this host.
            CAPTURE(p);
            CHECK(std::filesystem::exists(p));
            CHECK(MatchesFontconfig(p, request.first, request.second));
        }
    }

    TEST_CASE("name lookup is case-insensitive") {
        if (! HasAnyLinuxFont()) return;
        const auto a = ResolveSystemFontFallback("systemfont_Consolas");
        const auto b = ResolveSystemFontFallback("systemfont_CONSOLAS");
        const auto c = ResolveSystemFontFallback("systemfont_consolas");
        if (a.empty() || b.empty() || c.empty()) return;
        CHECK(a == b);
        CHECK(b == c);
    }

    TEST_CASE("non-systemfont names still classify (no prefix requirement)") {
        // The classifier matches substring-wise so a bare "consolas" still
        // resolves.  Real callers pass `font.find("systemfont")` first, but
        // the helper itself is tolerant.
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("consolas");
        if (p.empty()) return;
        CHECK(MatchesFontconfig(p, "Courier New:spacing=100", "Mono"));
    }

    TEST_CASE("a resolved path actually exists on disk") {
        // The old resolver returned a path only if one of its hardcoded
        // candidates existed, so on a layout it did not know about it returned
        // nothing at all.  Whatever comes back now must be a real file.
        if (!HasAnyLinuxFont()) return;
        for (const char* name : { "systemfont_arial", "systemfont_consolas",
                                  "systemfont_timesnewroman" }) {
            const auto p = ResolveSystemFontFallback(name);
            if (p.empty()) continue;
            CAPTURE(name);
            CAPTURE(p);
            CHECK(std::filesystem::exists(p));
        }
    }

    TEST_CASE("resolution succeeds on any host that has fonts installed") {
        // This is the openSUSE/Mageia regression: both ship fonts, neither uses
        // a layout the curated list covered, so every candidate missed and text
        // layers silently drew a placeholder.  Asking fontconfig removes the
        // dependence on knowing the layout up front.
        if (!HasAnyLinuxFont()) return;
        if (std::filesystem::is_empty("/usr/share/fonts")) return;
        const auto p = ResolveSystemFontFallback("systemfont_arial");
        REQUIRE_MESSAGE(!p.empty(),
                        "fonts are installed but nothing resolved");
        CHECK(std::filesystem::exists(p));
    }

    TEST_CASE("CJK fallback never returns a path that does not exist") {
        // Returning a Latin face for a CJK request would draw tofu, which is
        // worse than reporting nothing, so an unmatched language must stay empty.
        const auto p = wallpaper::ResolveCJKHanFallback();
        if (p.empty()) return;
        CHECK(std::filesystem::exists(p));
    }

    TEST_CASE("ReadSystemFile returns empty for missing path") {
        const auto data = ReadSystemFile("/nonexistent/path/that/does/not/exist.ttf");
        CHECK(data.empty());
    }

    TEST_CASE("ReadSystemFile reads a real font when one is resolvable") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_consolas");
        if (p.empty()) return;
        const auto data = ReadSystemFile(p);
        CHECK(!data.empty());
        // TTF files start with the magic 0x00 0x01 0x00 0x00 (TrueType) or
        // "OTTO" (OpenType CFF) or "true" (legacy Apple) — accept any.
        REQUIRE(data.size() >= 4);
        const bool ttf_magic = static_cast<unsigned char>(data[0]) == 0x00
                               && static_cast<unsigned char>(data[1]) == 0x01
                               && static_cast<unsigned char>(data[2]) == 0x00
                               && static_cast<unsigned char>(data[3]) == 0x00;
        const bool otf_magic = data.substr(0, 4) == "OTTO";
        const bool true_magic = data.substr(0, 4) == "true";
        CHECK((ttf_magic || otf_magic || true_magic));
    }
}
