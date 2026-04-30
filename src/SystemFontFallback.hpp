#pragma once
//
// Resolve Windows-style "systemfont_xxx" font names to a Linux font file path.
//
// Wallpaper Engine on Windows lets authors reference system fonts directly
// (e.g. `font: "systemfont_consolas"` for Microsoft Consolas).  On Linux those
// font files don't exist, but Liberation / DejaVu provide metric-compatible
// substitutes.  This resolver maps the WE name to a logical family
// (mono / sans / serif) and tries known distro-specific paths in order until
// one exists.
//
// Returns the absolute path of the first existing candidate, or empty string
// if no fallback could be located.
//
// Example:
//   ResolveSystemFontFallback("systemfont_consolas")
//     → "/usr/share/fonts/liberation-mono-fonts/LiberationMono-Regular.ttf"
//
// Pure helper.  No side effects.  Unit-testable in isolation.

#include <string>

namespace wallpaper {

std::string ResolveSystemFontFallback(const std::string& we_name);

// Read a system file's contents into a string.  Used for fonts resolved
// outside the WE VFS.  Returns empty on failure.
std::string ReadSystemFile(const std::string& path);

} // namespace wallpaper
