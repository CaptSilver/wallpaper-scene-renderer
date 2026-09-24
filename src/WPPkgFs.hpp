#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "Fs/Fs.h"

namespace wallpaper
{
namespace fs
{
class WPPkgFs : public Fs {
public:
    virtual ~WPPkgFs() = default;
    static std::unique_ptr<WPPkgFs> CreatePkgFs(std::string_view pkgpath);
    // Parse the .pkg header out of an in-memory stream — exposed for fuzzing
    // and unit tests. The resulting WPPkgFs's Open() will fail (no on-disk
    // backing), but the constructed file table is fully populated.
    static std::unique_ptr<WPPkgFs> CreateFromStream(IBinaryStream&  pkg,
                                                     std::string_view pkgpath);

private:
    WPPkgFs() = default;

public:
    bool                            Contains(std::string_view path) const override;
    std::shared_ptr<IBinaryStream>  Open(std::string_view path) override;
    std::shared_ptr<IBinaryStreamW> OpenW(std::string_view path) override;

private:
    struct PkgFile {
        std::string path;

        idx offset { 0 };
        idx length { 0 };
    };

    // Backing FILE* handles for this pkg, pooled across Open() calls instead
    // of a fresh fopen() per call. Held in its own ref-counted object (not
    // state on `this`) because an Open() result can outlive the WPPkgFs that
    // created it -- e.g. a sound mid-playback while the scene that owns this
    // WPPkgFs tears down. The pool only ever grows (a returned handle is
    // never closed early); that's fine in practice since the number of
    // concurrently-open reads against one pkg is small.
    struct StreamPool {
        std::mutex                                  mtx;
        std::vector<std::shared_ptr<IBinaryStream>> idle;
        std::string                                 pkgPath;
        // Test-only: counts real fopen()s (pool misses) so a test can prove
        // reuse without OS-level fd introspection. Atomic because both the
        // audio-mixing thread and the asset-loading thread can call Open()
        // on the same WPPkgFs concurrently.
        std::atomic<u32> openCount { 0 };
    };
    std::shared_ptr<StreamPool>              m_pool;
    std::unordered_map<std::string, PkgFile> m_files;

public:
    // Test-only.
    u32 DebugBackingOpenCount() const { return m_pool->openCount.load(); }
};
} // namespace fs
} // namespace wallpaper
