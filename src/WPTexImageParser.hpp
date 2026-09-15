#pragma once
#include "Interface/IImageParser.h"
#include "Fs/VFS.h"
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
    fs::VFS*                                                m_vfs;
    i64                                                     m_maxTotalBytes;
    std::string                                             m_cachePath;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_registered;
    // Header-only cache: avoids re-opening + re-parsing the .tex file when
    // ParseHeader is called multiple times for the same unregistered texture
    // during a single WPSceneParser::Parse run (e.g. autosize ortho pre-pass,
    // ParseImageObj, and LoadMaterial all query the same tex).
    std::unordered_map<std::string, ImageHeader>            m_headerCache;
};
} // namespace wallpaper
