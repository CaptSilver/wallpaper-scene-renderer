#pragma once

// Where a .pkg entry is allowed to land on disk.
//
// Entry paths inside a .pkg are just strings the packer chose, and the packer
// is whoever uploaded the wallpaper to the Steam Workshop.  Nothing validates
// them, so "../../.config/autostart/x.desktop" and "/etc/cron.d/x" are both
// perfectly representable.  Joining one of those onto the output directory
// writes wherever the string says — for an absolute path std::filesystem's
// operator/ discards the output directory entirely.
//
// The containment test compares canonical paths component by component rather
// than by string prefix, so "/tmp/out" does not vouch for "/tmp/out-evil".

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wp_pkg
{

// Destination for `entryPath` under `outdir`, or nullopt when the entry
// resolves outside `outdir` and must be refused.
//
// In flat mode the entry collapses to a single filename under
// <outdir>/<flatCategory>/; otherwise the stored folder structure is kept.
inline std::optional<std::filesystem::path> resolve_entry_dest(const std::filesystem::path& outdir,
                                                               std::string_view entryPath,
                                                               bool             flat,
                                                               std::string_view flatCategory) {
    namespace fs = std::filesystem;

    std::string rel(entryPath);
    // Stored paths are conventionally rooted ("/scene.json"), and a crafted pkg
    // can repeat the separator, so drop every leading one — not just the first.
    const auto firstReal = rel.find_first_not_of('/');
    rel.erase(0, firstReal == std::string::npos ? rel.size() : firstReal);
    if (rel.empty()) return std::nullopt;

    fs::path dest;
    if (flat) {
        // Remaining slashes become '_' so names stay unique inside one dir.
        std::replace(rel.begin(), rel.end(), '/', '_');
        dest = outdir / std::string(flatCategory) / rel;
    } else {
        dest = outdir / rel;
    }
    // Fold away any interior "..", so extraction never creates a directory the
    // pkg did not name just to step back out of it.
    dest = dest.lexically_normal();
    // Normalising an entry that named a directory rather than a file ("/." or
    // "/a/..") leaves a trailing separator, i.e. no filename.  That is outdir
    // itself; there is nothing to write.
    if (! dest.has_filename()) return std::nullopt;

    std::error_code ec;
    const fs::path  root = fs::weakly_canonical(outdir, ec);
    if (ec) return std::nullopt;
    const fs::path resolved = fs::weakly_canonical(dest, ec);
    if (ec) return std::nullopt;

    const auto mismatched =
        std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end());
    if (mismatched.first != root.end()) return std::nullopt;
    // A path equal to outdir is the directory itself, not a file in it.
    if (mismatched.second == resolved.end()) return std::nullopt;

    return dest;
}

} // namespace wp_pkg
