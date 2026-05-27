#include "SystemFontFallback.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace wallpaper {

namespace {

enum class FontFamily { Mono, Sans, Serif };

struct WeNameMapping {
    const char* prefix;
    FontFamily  family;
};

// Map WE systemfont_* names to a logical font family.  The list is matched
// substring-wise (case-insensitive on the WE side, lowered before lookup), so
// `systemfont_arial_bold` and `systemfont_arial` both resolve to sans.
constexpr WeNameMapping kNameMappings[] = {
    // Monospaced
    { "consolas",       FontFamily::Mono  },
    { "couriernew",     FontFamily::Mono  },
    { "courier_new",    FontFamily::Mono  },
    { "courier",        FontFamily::Mono  },
    { "lucidaconsole",  FontFamily::Mono  },
    { "lucida_console", FontFamily::Mono  },
    { "monaco",         FontFamily::Mono  },
    { "monospace",      FontFamily::Mono  },
    // Serif
    { "timesnewroman",  FontFamily::Serif },
    { "times_new_roman",FontFamily::Serif },
    { "times",          FontFamily::Serif },
    { "georgia",        FontFamily::Serif },
    { "cambria",        FontFamily::Serif },
    { "serif",          FontFamily::Serif },
    // Sans (everything else falls through to sans, but keeping these explicit
    // helps if we later want to log "exact match" vs "default fallback")
    { "arial",          FontFamily::Sans  },
    { "helvetica",      FontFamily::Sans  },
    { "segoe",          FontFamily::Sans  },
    { "tahoma",         FontFamily::Sans  },
    { "verdana",        FontFamily::Sans  },
    { "calibri",        FontFamily::Sans  },
    { "trebuchet",      FontFamily::Sans  },
};

// Per-family candidate paths.  Tried in order; first existing file wins.
// Covers Fedora/Bazzite (`liberation-*-fonts`), Debian/Ubuntu
// (`truetype/liberation`), Arch (`TTF/`), and DejaVu fallbacks.  Listed
// Liberation first because it ships metric-compatible substitutes for the
// classic Microsoft fonts and is widely available.
const std::vector<const char*>& CandidatesFor(FontFamily f) {
    static const std::vector<const char*> kMono = {
        "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };
    static const std::vector<const char*> kSans = {
        "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    static const std::vector<const char*> kSerif = {
        "/usr/share/fonts/liberation-serif-fonts/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/TTF/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/dejavu-serif-fonts/DejaVuSerif.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
        "/usr/share/fonts/TTF/DejaVuSerif.ttf",
    };
    switch (f) {
    case FontFamily::Mono:  return kMono;
    case FontFamily::Sans:  return kSans;
    case FontFamily::Serif: return kSerif;
    }
    return kSans;
}

std::string ToLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

FontFamily ClassifyName(const std::string& we_name) {
    const std::string lowered = ToLower(we_name);
    for (const auto& m : kNameMappings) {
        if (lowered.find(m.prefix) != std::string::npos) {
            return m.family;
        }
    }
    // Default for unknown systemfont_*: sans-serif (matches Windows default UI).
    return FontFamily::Sans;
}

} // namespace

std::string ResolveSystemFontFallback(const std::string& we_name) {
    const FontFamily family = ClassifyName(we_name);
    for (const char* path : CandidatesFor(family)) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            return path;
        }
    }
    return {};
}

std::string ReadSystemFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    const size_t read = std::fread(out.data(), 1, static_cast<size_t>(size), f);
    std::fclose(f);
    out.resize(read);
    return out;
}

std::string ResolveCJKHanFallback() {
    // Curated candidate list — first existing wins.  Covers:
    //   Fedora / RHEL / Bazzite — google-noto-sans-cjk-fonts
    //   Debian / Ubuntu         — fonts-noto-cjk
    //   Arch                    — noto-fonts-cjk
    //   openSUSE                — noto-sans-cjk-fonts
    //   Generic /usr/share/fonts/noto layouts as a last resort.
    // Noto Sans CJK is the canonical pan-CJK choice because it ships as a
    // single OTC covering Hans/Hant/Hangul/Kana in one face.  Source Han Sans
    // and Sarasa Gothic are alternative families; if a host has only those
    // installed the fallback returns empty and the renderer emits .notdef.
    static const char* kCandidates[] = {
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/TTF/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
        // Last-resort: any Noto Sans CJK weight at the canonical Fedora path.
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Medium.ttc",
    };
    for (const char* path : kCandidates) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec) && !ec) {
            return path;
        }
    }
    return {};
}

} // namespace wallpaper
