#include "WPPkgFs.hpp"
#include "Utils/Logging.h"
#include "Fs/LimitedBinaryStream.h"
#include "Fs/CBinaryStream.h"
#include <vector>
#include <algorithm>
#include <cctype>

using namespace wallpaper;
using namespace wallpaper::fs;

namespace
{
std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    return s;
}

std::string ReadSizedString(IBinaryStream& f) {
    idx ilen = f.ReadInt32();
    assert(ilen >= 0);

    usize       len = (usize)ilen;
    std::string result;
    result.resize(len);
    f.Read(result.data(), len);
    return result;
}
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath) {
    auto ppkg = fs::CreateCBinaryStream(pkgpath);
    if (! ppkg) return nullptr;

    auto&       pkg = *ppkg;
    std::string ver = ReadSizedString(pkg);
    LOG_INFO("pkg version: %s", ver.data());

    std::vector<PkgFile> pkgfiles;
    i32                  entryCount = pkg.ReadInt32();
    for (i32 i = 0; i < entryCount; i++) {
        std::string path   = "/" + ReadSizedString(pkg);
        idx         offset = pkg.ReadInt32();
        idx         length = pkg.ReadInt32();
        pkgfiles.push_back({ path, offset, length });
    }
    auto pkgfs       = std::unique_ptr<WPPkgFs>(new WPPkgFs());
    pkgfs->m_pkgPath = pkgpath;
    idx headerSize   = pkg.Tell();
    for (auto& el : pkgfiles) {
        el.offset += headerSize;
        pkgfs->m_files.insert({ ToLower(el.path), el });
    }
    return pkgfs;
}

bool WPPkgFs::Contains(std::string_view path) const {
    return m_files.count(ToLower(std::string(path))) > 0;
}

std::shared_ptr<IBinaryStream> WPPkgFs::Open(std::string_view path) {
    auto pkg = fs::CreateCBinaryStream(m_pkgPath);
    if (! pkg) return nullptr;
    auto key = ToLower(std::string(path));
    auto it  = m_files.find(key);
    if (it != m_files.end()) {
        const auto& file = it->second;
        return std::make_shared<LimitedBinaryStream>(pkg, file.offset, file.length);
    }
    return nullptr;
}

std::shared_ptr<IBinaryStreamW> WPPkgFs::OpenW(std::string_view) { return nullptr; }
