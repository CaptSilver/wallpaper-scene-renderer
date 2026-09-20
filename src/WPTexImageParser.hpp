#pragma once
#include "Interface/IImageParser.h"
#include "Fs/VFS.h"
#include <mutex>
#include <span>
#include <unordered_map>

namespace wallpaper
{

// Cumulative texture budget Parse() enforces when no explicit budget is
// passed to the constructor.  Reads WEKDE_MAX_TEX_BYTES per call (no
// caching -- same pattern as WEKDE_MSAA/WEKDE_DEBUG_FRAMETIME, so a test or
// a live operator can change it without restarting the process); falls back
// to the WEK_MAX_TEX_BYTES compile-time default when unset or unparsable.
i64 DefaultMaxTexBytes();

class WPTexImageParser : public IImageParser {
public:
    explicit WPTexImageParser(fs::VFS* vfs, i64 maxTotalBytes = DefaultMaxTexBytes())
        : m_vfs(vfs), m_maxTotalBytes(maxTotalBytes) {}
    virtual ~WPTexImageParser() = default;

    std::shared_ptr<Image> Parse(const std::string&) override;
    ImageHeader            ParseHeader(const std::string&) override;

    // Register a pre-built image (e.g. rasterized text) so Parse() returns it by key.
    void RegisterImage(const std::string& key, std::shared_ptr<Image> img);

    void SetCachePath(const std::string& path) { m_cachePath = path; }

    // Returns the number of entries in the header-only cache.  Used in tests
    // to assert that error-path returns are not written to the cache.
    std::size_t headerCacheSize() const { return m_headerCache.size(); }

private:
    fs::VFS*    m_vfs;
    i64         m_maxTotalBytes;
    std::string m_cachePath;
    // Guards m_registered only.  PrefetchTextures() (below) calls Parse()
    // for many names at once from a worker pool, and Parse() both reads
    // and writes m_registered on every call.  m_headerCache stays
    // unguarded -- ParseHeader() is never called concurrently with a
    // prefetch in production (every ParseHeader() call happens earlier,
    // on the load thread, before SET_SCENE is dispatched).
    std::mutex                                              m_registered_mutex;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_registered;
    // Header-only cache: avoids re-opening + re-parsing the .tex file when
    // ParseHeader is called multiple times for the same unregistered texture
    // during a single WPSceneParser::Parse run (e.g. autosize ortho pre-pass,
    // ParseImageObj, and LoadMaterial all query the same tex).
    std::unordered_map<std::string, ImageHeader> m_headerCache;
};

// Decode every name in `names` across up to `worker_count` threads, each
// calling parser.Parse(name).  Parse() self-caches a successful decode
// into its own m_registered map, so a caller that walks the same names
// afterward with a plain serial Parse() loop gets cache hits for
// everything this call already finished -- that's the point: it moves
// the actual stbi_load_from_memory cost off whichever thread calls
// Parse() second, onto this bounded pool instead.  worker_count is
// clamped to [1, names.size()]; pass 1 for a deterministic,
// single-threaded decode.
void PrefetchTextures(WPTexImageParser& parser, std::span<const std::string> names,
                      unsigned worker_count);

} // namespace wallpaper
