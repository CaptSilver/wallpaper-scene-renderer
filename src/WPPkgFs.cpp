#include "WPPkgFs.hpp"
#include "Utils/Logging.h"
#include "Fs/LimitedBinaryStream.h"
#include "Fs/CBinaryStream.h"
#include "WPCommon.hpp"
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

// Returns false on hostile or truncated input (negative/oversized length, EOF).
// Caller must check before using `out` — and bail out of further parsing.
bool ReadSizedString(IBinaryStream& f, std::string& out) {
    idx ilen = f.ReadInt32();
    if (ilen < 0) return false;
    usize len = (usize)ilen;
    if (! CountFitsStream(f, len)) return false;
    out.resize(len);
    if (len > 0 && f.Read(out.data(), len) != len) return false;
    return true;
}
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath) {
    auto ppkg = fs::CreateCBinaryStream(pkgpath);
    if (! ppkg) return nullptr;
    return CreateFromStream(*ppkg, pkgpath);
}

std::unique_ptr<WPPkgFs> WPPkgFs::CreateFromStream(IBinaryStream&  pkg,
                                                   std::string_view pkgpath) {
    std::string ver;
    if (! ReadSizedString(pkg, ver)) {
        LOG_ERROR("pkg: invalid version field");
        return nullptr;
    }
    LOG_INFO("pkg version: %s", ver.data());

    i32 entryCount = pkg.ReadInt32();
    if (entryCount < 0 || ! CountFitsStream(pkg, (usize)entryCount)) {
        LOG_ERROR("pkg: bad entryCount %d", entryCount);
        return nullptr;
    }
    std::vector<PkgFile> pkgfiles;
    pkgfiles.reserve((usize)entryCount);
    for (i32 i = 0; i < entryCount; i++) {
        std::string name;
        if (! ReadSizedString(pkg, name)) {
            LOG_ERROR("pkg: malformed entry name at index %d", i);
            return nullptr;
        }
        std::string path   = "/" + name;
        idx         offset = pkg.ReadInt32();
        idx         length = pkg.ReadInt32();
        if (offset < 0 || length < 0) {
            LOG_ERROR("pkg: negative offset/length on entry %d", i);
            return nullptr;
        }
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
