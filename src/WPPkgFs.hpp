#pragma once

#include <unordered_map>
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
    std::string                              m_pkgPath;
    std::unordered_map<std::string, PkgFile> m_files;
};
} // namespace fs
} // namespace wallpaper
