#pragma once
#include "Interface/IImageParser.h"
#include "Fs/VFS.h"
#include <unordered_map>

namespace wallpaper
{

class WPTexImageParser : public IImageParser {
public:
    WPTexImageParser(fs::VFS* vfs): m_vfs(vfs) {}
    virtual ~WPTexImageParser() = default;

    std::shared_ptr<Image> Parse(const std::string&) override;
    ImageHeader            ParseHeader(const std::string&) override;

    // Register a pre-built image (e.g. rasterized text) so Parse() returns it by key.
    void RegisterImage(const std::string& key, std::shared_ptr<Image> img);

    void SetCachePath(const std::string& path) { m_cachePath = path; }

private:
    fs::VFS*                                                  m_vfs;
    std::string                                               m_cachePath;
    std::unordered_map<std::string, std::shared_ptr<Image>>   m_registered;
};
} // namespace wallpaper
