// Pin the systemfont_* → Linux font path mapping logic.
//
// The mapping is OS-dependent: ResolveSystemFontFallback returns whatever
// candidate path exists first.  Tests assert the *family* of the resolved
// path (mono/sans/serif via filename substring) rather than a specific path,
// because Bazzite ships Liberation but Debian/Ubuntu/Arch vary.

#include <doctest.h>

#include "SystemFontFallback.hpp"

#include <filesystem>
#include <string>

using wallpaper::ResolveSystemFontFallback;
using wallpaper::ReadSystemFile;

namespace {

bool IsMonoFamily(const std::string& path) {
    return path.find("Mono") != std::string::npos
        || path.find("mono") != std::string::npos;
}

bool IsSerifFamily(const std::string& path) {
    return path.find("Serif") != std::string::npos
        || path.find("serif") != std::string::npos;
}

bool IsSansFamily(const std::string& path) {
    // "Sans" but NOT "SansMono" / "SansSerif" — narrow check.
    if (path.find("Mono") != std::string::npos) return false;
    if (path.find("Serif") != std::string::npos) return false;
    return path.find("Sans") != std::string::npos
        || path.find("sans") != std::string::npos;
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
        CHECK(IsMonoFamily(p));
    }

    TEST_CASE("courier_new resolves to a monospaced font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_couriernew");
        if (p.empty()) return;
        CHECK(IsMonoFamily(p));
    }

    TEST_CASE("arial resolves to a sans-serif font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_arial");
        if (p.empty()) return;
        CHECK(IsSansFamily(p));
    }

    TEST_CASE("times new roman resolves to a serif font") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_timesnewroman");
        if (p.empty()) return;
        CHECK(IsSerifFamily(p));
    }

    TEST_CASE("unknown systemfont_* defaults to sans-serif") {
        if (!HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_madeupname");
        if (p.empty()) return;
        CHECK(IsSansFamily(p));
    }

    TEST_CASE("name lookup is case-insensitive") {
        if (!HasAnyLinuxFont()) return;
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
        CHECK(IsMonoFamily(p));
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
