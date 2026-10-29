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

#include <fontconfig/fontconfig.h>
#include <unistd.h>

#include "test_scratch.hpp"

using wallpaper::ReadSystemFile;
using wallpaper::ResolveSystemFontFallback;

namespace
{

bool IsMonoFamily(const std::string& path) {
    return path.find("Mono") != std::string::npos || path.find("mono") != std::string::npos;
}

bool IsSerifFamily(const std::string& path) {
    return path.find("Serif") != std::string::npos || path.find("serif") != std::string::npos;
}

bool IsSansFamily(const std::string& path) {
    // "Sans" but NOT "SansMono" / "SansSerif" — narrow check.
    if (path.find("Mono") != std::string::npos) return false;
    if (path.find("Serif") != std::string::npos) return false;
    return path.find("Sans") != std::string::npos || path.find("sans") != std::string::npos;
}

bool HasAnyLinuxFont() {
    namespace fs = std::filesystem;
    return fs::exists("/usr/share/fonts");
}

// Replaces fontconfig's process-wide configuration with one that knows only the fonts in
// `fontDir` (none when empty), so the resolver sees exactly what a test provides.
class ScopedFontconfig {
public:
    explicit ScopedFontconfig(const std::filesystem::path& fontDir) {
        m_previous = FcConfigReference(nullptr);
        m_mine     = FcConfigCreate();
        if (! fontDir.empty()) {
            FcConfigAppFontAddDir(m_mine, reinterpret_cast<const FcChar8*>(fontDir.c_str()));
        }
        m_installed = FcConfigSetCurrent(m_mine) == FcTrue;
    }
    ~ScopedFontconfig() {
        FcConfigSetCurrent(m_previous);
        FcConfigDestroy(m_previous);
        FcConfigDestroy(m_mine);
    }
    bool installed() const { return m_installed; }

private:
    FcConfig* m_previous { nullptr };
    FcConfig* m_mine { nullptr };
    bool      m_installed { false };
};

// A font file the host's fontconfig lists for `lang` (any language when null), found without
// going through the resolver under test; empty when the host has none.
std::string hostFontFile(const char* lang) {
    FcPattern* pat = FcPatternCreate();
    if (lang != nullptr) {
        FcPatternAddString(pat, FC_LANG, reinterpret_cast<const FcChar8*>(lang));
    }
    FcObjectSet* props = FcObjectSetBuild(FC_FILE, nullptr);
    FcFontSet*   fonts = FcFontList(nullptr, pat, props);
    std::string  file;
    if (fonts != nullptr) {
        for (int i = 0; i < fonts->nfont && file.empty(); i++) {
            FcChar8* path = nullptr;
            if (FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &path) == FcResultMatch &&
                path != nullptr && std::filesystem::exists(reinterpret_cast<const char*>(path))) {
                file = reinterpret_cast<const char*>(path);
            }
        }
        FcFontSetDestroy(fonts);
    }
    FcObjectSetDestroy(props);
    FcPatternDestroy(pat);
    return file;
}

// A scratch directory holding a link to `font`; returns the link's path.
std::filesystem::path linkFontIntoScratch(const std::string& tag, const std::string& font) {
    const auto dir = std::filesystem::path(wallpaper::test::ScratchDir(tag, ::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto link = dir / std::filesystem::path(font).filename();
    std::filesystem::create_symlink(font, link);
    return link;
}

} // namespace

TEST_SUITE("SystemFontFallback") {
    TEST_CASE("consolas resolves to a monospaced font") {
        if (! HasAnyLinuxFont()) return; // skip if /usr/share/fonts absent
        const auto p = ResolveSystemFontFallback("systemfont_consolas");
        if (p.empty()) {
            // No Liberation/DejaVu installed at all — acceptable on minimal containers.
            WARN_MESSAGE(
                false, "no fallback font installed; install liberation-mono-fonts to enable test");
            return;
        }
        CHECK(IsMonoFamily(p));
    }

    TEST_CASE("courier_new resolves to a monospaced font") {
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_couriernew");
        if (p.empty()) return;
        CHECK(IsMonoFamily(p));
    }

    TEST_CASE("arial resolves to a sans-serif font") {
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_arial");
        if (p.empty()) return;
        CHECK(IsSansFamily(p));
    }

    TEST_CASE("times new roman resolves to a serif font") {
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_timesnewroman");
        if (p.empty()) return;
        CHECK(IsSerifFamily(p));
    }

    TEST_CASE("unknown systemfont_* defaults to sans-serif") {
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_madeupname");
        if (p.empty()) return;
        CHECK(IsSansFamily(p));
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
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("consolas");
        if (p.empty()) return;
        CHECK(IsMonoFamily(p));
    }

    TEST_CASE("a resolved path actually exists on disk") {
        // The old resolver returned a path only if one of its hardcoded
        // candidates existed, so on a layout it did not know about it returned
        // nothing at all.  Whatever comes back now must be a real file.
        if (! HasAnyLinuxFont()) return;
        for (const char* name :
             { "systemfont_arial", "systemfont_consolas", "systemfont_timesnewroman" }) {
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
        if (! HasAnyLinuxFont()) return;
        if (std::filesystem::is_empty("/usr/share/fonts")) return;
        const auto p = ResolveSystemFontFallback("systemfont_arial");
        REQUIRE_MESSAGE(! p.empty(), "fonts are installed but nothing resolved");
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
        if (! HasAnyLinuxFont()) return;
        const auto p = ResolveSystemFontFallback("systemfont_consolas");
        if (p.empty()) return;
        const auto data = ReadSystemFile(p);
        CHECK(! data.empty());
        // TTF files start with the magic 0x00 0x01 0x00 0x00 (TrueType) or
        // "OTTO" (OpenType CFF) or "true" (legacy Apple) — accept any.
        REQUIRE(data.size() >= 4);
        const bool ttf_magic  = static_cast<unsigned char>(data[0]) == 0x00 &&
                                static_cast<unsigned char>(data[1]) == 0x01 &&
                                static_cast<unsigned char>(data[2]) == 0x00 &&
                                static_cast<unsigned char>(data[3]) == 0x00;
        const bool otf_magic  = data.substr(0, 4) == "OTTO";
        const bool true_magic = data.substr(0, 4) == "true";
        CHECK((ttf_magic || otf_magic || true_magic));
    }

    TEST_CASE("the font fontconfig picks is returned when it exists") {
        const auto known = hostFontFile(nullptr);
        if (known.empty()) return;

        const auto link = linkFontIntoScratch("fc_only_font", known);
        {
            ScopedFontconfig fc(link.parent_path());
            REQUIRE(fc.installed());
            CHECK(ResolveSystemFontFallback("systemfont_arial") == link.string());
        }
        std::filesystem::remove_all(link.parent_path());
    }

    TEST_CASE("fixed font paths serve when fontconfig knows no fonts") {
        // The sans entries of the built-in candidate list.
        const char* const sans[] = {
            "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
        };
        const char* firstPresent = nullptr;
        for (const char* path : sans) {
            if (std::filesystem::exists(path)) {
                firstPresent = path;
                break;
            }
        }
        if (firstPresent == nullptr) return; // host keeps its fonts somewhere only fontconfig knows

        ScopedFontconfig fc(std::filesystem::path {});
        REQUIRE(fc.installed());
        CHECK(ResolveSystemFontFallback("systemfont_arial") == firstPresent);
    }

    TEST_CASE("the CJK font fontconfig picks is returned when it exists") {
        const auto known = hostFontFile("ja");
        if (known.empty()) return;

        const auto link = linkFontIntoScratch("fc_only_cjk", known);
        {
            ScopedFontconfig fc(link.parent_path());
            REQUIRE(fc.installed());
            CHECK(wallpaper::ResolveCJKHanFallback() == link.string());
        }
        std::filesystem::remove_all(link.parent_path());
    }
}
