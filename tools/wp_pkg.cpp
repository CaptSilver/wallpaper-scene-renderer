// wp-pkg: Wallpaper Engine .pkg inspection / extraction tool.
//
// Subcommands:
//   list    <pkg>                          Print header + entries.
//   extract <pkg> <outdir> [--flat] [--dry-run] [--raw]
//   scripts <pkg|dir> [<outdir>] [--dry-run]
//
// By default, `extract` preserves the internal folder structure (so a
// scene's /scene.json, /shaders/*.frag, /models/*.mdl, /materials/*.json,
// /*.tex, /scripts/*.js land under <outdir>/<original path>).
//
// --flat groups every file into <outdir>/<category>/... where category is
// one of: scripts, shaders, textures, models, materials, scenes, audio,
// other -- easy one-folder analysis.
//
// --dry-run prints what would be written but does not touch the fs.
//
// --raw disables the tex quick-summary sidecar (default: .tex files get a
// .info.txt next to them describing format / dimensions / sprite flag).
//
// `scripts` walks scene.json and dumps every inlined SceneScript body
// (escaped as a JSON string) to its own file plus a one-line manifest.
// Replaces the recurring one-off Python walkers we used to audit which
// objects carry which scripts — especially useful when investigating
// wallpapers that don't ship any .js files, only inlined scripts in
// property/instanceoverride bindings (e.g. NieR:Automata).
//
// Self-contained: does NOT link the renderer, only nlohmann/json
// (header-only, already a submodule).  The .pkg format is tiny enough
// that re-implementing it here is cheaper than pulling in
// wescene-renderer + lz4 + mpv + freetype.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace
{

struct PkgEntry {
    std::string path;   // always leading '/'
    uint32_t    offset; // absolute (data region start + relative offset)
    uint32_t    length;
};

struct Pkg {
    std::string           version;
    uint32_t              dataStart { 0 };
    std::vector<PkgEntry> entries;
};

// Read N little-endian bytes into uint32_t. Returns false on short read.
bool read_u32_le(std::ifstream& f, uint32_t& out) {
    uint8_t b[4];
    if (! f.read(reinterpret_cast<char*>(b), 4)) return false;
    out = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool read_sized_string(std::ifstream& f, std::string& out) {
    uint32_t n;
    if (! read_u32_le(f, n)) return false;
    if (n > (16u << 20)) return false; // sanity: 16 MiB cap on any path/version string
    out.resize(n);
    if (n && ! f.read(out.data(), n)) return false;
    return true;
}

bool load_pkg(const fs::path& path, Pkg& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (! f) {
        err = "cannot open pkg: " + path.string();
        return false;
    }
    if (! read_sized_string(f, out.version)) {
        err = "pkg header: version string";
        return false;
    }
    uint32_t count;
    if (! read_u32_le(f, count)) {
        err = "pkg header: entry count";
        return false;
    }
    if (count > (1u << 20)) {
        err = "pkg header: unreasonable entry count " + std::to_string(count);
        return false;
    }
    out.entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        PkgEntry    e;
        std::string rel;
        if (! read_sized_string(f, rel)) {
            err = "pkg entry " + std::to_string(i) + ": path";
            return false;
        }
        e.path = "/" + rel;
        if (! read_u32_le(f, e.offset)) {
            err = "pkg entry " + std::to_string(i) + ": offset";
            return false;
        }
        if (! read_u32_le(f, e.length)) {
            err = "pkg entry " + std::to_string(i) + ": length";
            return false;
        }
        out.entries.push_back(std::move(e));
    }
    auto header_end = f.tellg();
    if (header_end < 0) {
        err = "tellg failed";
        return false;
    }
    out.dataStart = static_cast<uint32_t>(header_end);
    for (auto& e : out.entries) e.offset += out.dataStart;
    return true;
}

std::string_view extension_of(std::string_view path) {
    auto slash = path.find_last_of('/');
    auto dot   = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return path.substr(dot + 1);
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return char(std::tolower(c));
    });
    return out;
}

std::string_view category_of(std::string_view ext_lower) {
    if (ext_lower == "tex") return "textures";
    if (ext_lower == "frag" || ext_lower == "vert" || ext_lower == "geom" || ext_lower == "comp" ||
        ext_lower == "h" || ext_lower == "hlsl" || ext_lower == "glsl")
        return "shaders";
    if (ext_lower == "js" || ext_lower == "lua" || ext_lower == "txt") return "scripts";
    if (ext_lower == "mdl" || ext_lower == "mdla" || ext_lower == "mdat") return "models";
    if (ext_lower == "mp3" || ext_lower == "wav" || ext_lower == "ogg") return "audio";
    if (ext_lower == "mp4" || ext_lower == "webm" || ext_lower == "mov") return "video";
    if (ext_lower == "json") return "json";
    return "other";
}

// Light TEX inspection. Returns empty string on failure.
// Format recap (see WPTexImageParser.cpp):
//   TEXV0001  (9 bytes, null-terminated 8-char tag)
//   TEXI0002  (9 bytes)
//   int32 format
//   uint32 flags
//   int32 width, height, mapWidth, mapHeight
//   int32 unknown
//   TEXB0003  (9 bytes)
//   int32 count
//   [if TEXB >= 3] int32 type
//   [if TEXB >= 4] int32 isVideoMp4
std::string inspect_tex(const char* data, uint32_t length) {
    auto read_tag = [&](uint32_t off, int& ver, std::string& prefix) -> bool {
        if (off + 9 > length) return false;
        char tag[9];
        std::memcpy(tag, data + off, 9);
        if (tag[8] != '\0') return false;
        prefix.assign(tag, tag + 3);
        if (prefix != "TEX") return false;
        char num[5] = { tag[4], tag[5], tag[6], tag[7], '\0' };
        ver         = std::atoi(num);
        return true;
    };
    auto read_i32 = [&](uint32_t off, int32_t& out) -> bool {
        if (off + 4 > length) return false;
        uint32_t u;
        std::memcpy(&u, data + off, 4);
        out = int32_t(u);
        return true;
    };

    uint32_t    cur  = 0;
    int         texv = 0, texi = 0, texb = 0;
    std::string p;
    if (! read_tag(cur, texv, p)) return {};
    cur += 9;
    if (! read_tag(cur, texi, p)) return {};
    cur += 9;

    int32_t format = 0;
    int32_t flags  = 0;
    int32_t w = 0, h = 0, mw = 0, mh = 0, unk = 0, count = 0, type = -1;
    if (! read_i32(cur, format)) return {};
    cur += 4;
    if (! read_i32(cur, flags)) return {};
    cur += 4;
    if (! read_i32(cur, w)) return {};
    cur += 4;
    if (! read_i32(cur, h)) return {};
    cur += 4;
    if (! read_i32(cur, mw)) return {};
    cur += 4;
    if (! read_i32(cur, mh)) return {};
    cur += 4;
    if (! read_i32(cur, unk)) return {};
    cur += 4;
    if (! read_tag(cur, texb, p)) return {};
    cur += 9;
    if (! read_i32(cur, count)) return {};
    cur += 4;
    if (texb >= 3) {
        if (! read_i32(cur, type)) return {};
        cur += 4;
    }

    bool is_sprite = (flags & (1u << 2)) != 0;
    bool clamp     = (flags & (1u << 1)) != 0;
    bool no_filter = (flags & (1u << 0)) != 0;

    char buf[512];
    int  n = std::snprintf(buf,
                          sizeof(buf),
                          "tex_version  TEXV=%d TEXI=%d TEXB=%d\n"
                           "format       %d\n"
                           "width        %d\n"
                           "height       %d\n"
                           "mapWidth     %d\n"
                           "mapHeight    %d\n"
                           "image_count  %d\n"
                           "type         %d\n"
                           "sprite       %s\n"
                           "clamp_uvs    %s\n"
                           "no_filter    %s\n",
                          texv,
                          texi,
                          texb,
                          format,
                          w,
                          h,
                          mw,
                          mh,
                          count,
                          type,
                          is_sprite ? "true" : "false",
                          clamp ? "true" : "false",
                          no_filter ? "true" : "false");
    if (n <= 0) return {};
    return std::string(buf, buf + n);
}

// Extract a single entry. Returns true on success. If is_tex && !raw, also
// writes a .info.txt sidecar describing the texture header.
bool extract_entry(std::ifstream& pkg, const PkgEntry& e, const fs::path& dest, bool is_tex,
                   bool raw, bool dry_run) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr,
                     "mkdir failed: %s (%s)\n",
                     dest.parent_path().string().c_str(),
                     ec.message().c_str());
        return false;
    }

    std::vector<char> buf(e.length);
    if (e.length > 0) {
        pkg.seekg(e.offset);
        if (! pkg.read(buf.data(), e.length)) {
            std::fprintf(stderr,
                         "read failed at offset %u length %u for %s\n",
                         e.offset,
                         e.length,
                         e.path.c_str());
            return false;
        }
    }

    if (! dry_run) {
        std::ofstream out(dest, std::ios::binary);
        if (! out) {
            std::fprintf(stderr, "write failed: %s\n", dest.string().c_str());
            return false;
        }
        if (e.length > 0) out.write(buf.data(), e.length);
    }

    if (is_tex && ! raw) {
        std::string info = inspect_tex(buf.data(), e.length);
        if (! info.empty() && ! dry_run) {
            fs::path side = dest;
            side += ".info.txt";
            std::ofstream out(side);
            if (out) out << info;
        }
    }
    return true;
}

int cmd_list(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: wp-pkg list <pkg>\n");
        return 2;
    }
    Pkg         pkg;
    std::string err;
    if (! load_pkg(argv[2], pkg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("pkg_version  %s\n", pkg.version.c_str());
    std::printf("data_start   %u\n", pkg.dataStart);
    std::printf("entries      %zu\n", pkg.entries.size());
    std::printf("%-10s  %-10s  %s\n", "offset", "size", "path");
    for (const auto& e : pkg.entries) {
        std::printf("%-10u  %-10u  %s\n", e.offset, e.length, e.path.c_str());
    }
    return 0;
}

int cmd_extract(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: wp-pkg extract <pkg> <outdir> [--flat] [--dry-run] [--raw]\n");
        return 2;
    }
    const char* pkg_path = argv[2];
    fs::path    outdir   = argv[3];
    bool        flat     = false;
    bool        dry_run  = false;
    bool        raw      = false;
    for (int i = 4; i < argc; i++) {
        std::string_view a = argv[i];
        if (a == "--flat")
            flat = true;
        else if (a == "--dry-run")
            dry_run = true;
        else if (a == "--raw")
            raw = true;
        else {
            std::fprintf(stderr, "unknown flag: %.*s\n", int(a.size()), a.data());
            return 2;
        }
    }

    Pkg         pkg;
    std::string err;
    if (! load_pkg(pkg_path, pkg, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    std::ifstream f(pkg_path, std::ios::binary);
    if (! f) {
        std::fprintf(stderr, "cannot reopen pkg for data read\n");
        return 1;
    }

    if (! dry_run) {
        std::error_code ec;
        fs::create_directories(outdir, ec);
        if (ec) {
            std::fprintf(stderr, "mkdir %s: %s\n", outdir.string().c_str(), ec.message().c_str());
            return 1;
        }
    }

    size_t ok = 0, fail = 0;
    for (const auto& e : pkg.entries) {
        std::string      ext    = to_lower(extension_of(e.path));
        bool             is_tex = (ext == "tex");
        std::string_view cat    = category_of(ext);

        fs::path dest;
        if (flat) {
            std::string leaf(e.path);
            // strip leading '/'
            if (! leaf.empty() && leaf.front() == '/') leaf.erase(0, 1);
            // replace remaining slashes with '_' so names stay unique inside one dir
            for (auto& c : leaf)
                if (c == '/') c = '_';
            dest = outdir / std::string(cat) / leaf;
        } else {
            std::string rel(e.path);
            if (! rel.empty() && rel.front() == '/') rel.erase(0, 1);
            dest = outdir / rel;
        }

        if (extract_entry(f, e, dest, is_tex, raw, dry_run)) {
            ok++;
            std::printf("%s  %s\n", dry_run ? "DRY " : "WROTE", dest.string().c_str());
        } else {
            fail++;
        }
    }

    std::printf("done: %zu ok, %zu failed\n", ok, fail);
    return fail == 0 ? 0 : 1;
}

// ─── scripts subcommand ───────────────────────────────────────────────────
//
// Walk scene.json and dump every inlined script body to its own file, one
// per occurrence.  Matches the shape WE uses today:
//   objects[N].{visible,origin,scale,angles,alpha}.script       (top-level)
//   objects[N].animationlayers[M].{visible,...}.script          (per-rig)
//   objects[N].instanceoverride.<field>.script                  (particle)
// plus any future scripted property — the walker is generic, it just
// looks for `.script` string values that smell like JS.

// `isJsLike`: permissive sniffer — anything that contains common JS
// keywords or the WE boilerplate counts.  The WE format always has the
// `'use strict'` prelude on inlined scripts, which by itself is enough.
static bool isJsLike(const std::string& s) {
    if (s.empty()) return false;
    if (s.find("'use strict'") != std::string::npos) return true;
    if (s.find("\"use strict\"") != std::string::npos) return true;
    if (s.find("export function") != std::string::npos) return true;
    if (s.find("function ") != std::string::npos) return true;
    if (s.find("return ") != std::string::npos && s.find("{") != std::string::npos) return true;
    return false;
}

// Join a JSON path pointer like ".instanceoverride.rate.script".
static std::string pathJoin(const std::string& base, const std::string& leaf) {
    if (base.empty()) return leaf;
    return base + "." + leaf;
}

// Walk the tree, collecting (id, name, bindingPath, scriptBody) for every
// `.script` leaf that looks like JS.  Object id/name come from the nearest
// enclosing object that has them (scene.json top-level "objects" entries).
struct ScriptHit {
    int32_t     id { -1 };
    std::string name;
    std::string path;
    std::string body;
};
static void walkForScripts(const nlohmann::json& node, const std::string& path, int32_t curId,
                           const std::string& curName, std::vector<ScriptHit>& out) {
    if (node.is_object()) {
        int32_t     nextId   = curId;
        std::string nextName = curName;
        // Only capture the OUTERMOST object's id/name — nested bindings like
        // instanceoverride sometimes carry their own `id` field (the target
        // instance id to override on), which would otherwise shadow the
        // enclosing scene object's identity.  NieR:Automata obj 299 has
        // instanceoverride.id=301; we want the script to be tagged with 299.
        if (curId < 0 && node.contains("id") && node.at("id").is_number_integer()) {
            nextId = node.at("id").get<int32_t>();
        }
        if (curName.empty() && node.contains("name") && node.at("name").is_string()) {
            nextName = node.at("name").get<std::string>();
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
            const std::string& key = it.key();
            const auto&        v   = it.value();
            if (key == "script" && v.is_string()) {
                std::string body = v.get<std::string>();
                if (isJsLike(body)) {
                    ScriptHit hit;
                    hit.id   = nextId;
                    hit.name = nextName;
                    hit.path =
                        path; // path to the containing object (e.g. ".instanceoverride.rate")
                    hit.body = std::move(body);
                    out.push_back(std::move(hit));
                    continue; // don't recurse into a script string
                }
            }
            walkForScripts(v, pathJoin(path, key), nextId, nextName, out);
        }
    } else if (node.is_array()) {
        for (size_t i = 0; i < node.size(); i++) {
            std::ostringstream ss;
            ss << path << "[" << i << "]";
            walkForScripts(node[i], ss.str(), curId, curName, out);
        }
    }
}

// Sanitize object name for use as a filename component.  Strip slashes,
// collapse whitespace, cap length.  Keeps the output readable without
// breaking on exotic characters (Chinese wallpaper names, etc).
static std::string sanitizeForFilename(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '<' || c == '>' ||
            c == '|' || c == '"' || (unsigned char)c < 0x20) {
            out.push_back('_');
        } else if (c == ' ') {
            out.push_back('-');
        } else {
            out.push_back(c);
        }
    }
    if (out.size() > 40) out.resize(40);
    return out;
}

// Look up the scene.json bytes.  If `src` is a directory, read
// `<src>/scene.json` directly.  If it's a .pkg file, read it via load_pkg +
// in-memory copy of the entry.  Falls through with err set on failure.
static bool readSceneJson(const fs::path& src, std::string& out, std::string& err) {
    std::error_code ec;
    if (fs::is_directory(src, ec)) {
        // Prefer scene.json; fall back to project.json's `file` field for
        // defaultprojects like ricepod (ricepod.json) or fantasticcar
        // (fantasticcar.json) where the scene file has a non-standard name.
        fs::path candidates[] = { src / "scene.json" };
        for (const auto& sj : candidates) {
            std::ifstream f(sj, std::ios::binary);
            if (! f) continue;
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }
        fs::path      pj = src / "project.json";
        std::ifstream pf(pj, std::ios::binary);
        if (pf) {
            std::ostringstream ss;
            ss << pf.rdbuf();
            try {
                auto proj = nlohmann::json::parse(ss.str(), nullptr, true, true);
                if (proj.contains("file") && proj.at("file").is_string()) {
                    fs::path      fallback = src / proj.at("file").get<std::string>();
                    std::ifstream ff(fallback, std::ios::binary);
                    if (ff) {
                        std::ostringstream ss2;
                        ss2 << ff.rdbuf();
                        out = ss2.str();
                        return true;
                    }
                }
            } catch (...) { /* fall through to error */
            }
        }
        err = "no scene.json (or project.json `file` reference) under " + src.string();
        return false;
    }
    Pkg pkg;
    if (! load_pkg(src, pkg, err)) return false;
    const PkgEntry* scene = nullptr;
    for (const auto& e : pkg.entries) {
        if (e.path == "/scene.json") {
            scene = &e;
            break;
        }
    }
    if (! scene) {
        err = "scene.json not found inside " + src.string();
        return false;
    }
    std::ifstream f(src, std::ios::binary);
    if (! f) {
        err = "cannot reopen pkg for data read";
        return false;
    }
    out.resize(scene->length);
    f.seekg(scene->offset);
    if (! f.read(out.data(), scene->length)) {
        err = "scene.json read failed";
        return false;
    }
    return true;
}

int cmd_scripts(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: wp-pkg scripts <pkg|dir> [<outdir>] [--dry-run]\n"
                     "\n"
                     "  Walks scene.json and dumps every inlined SceneScript body to\n"
                     "  <outdir>/scripts/s<NN>_obj<id>_<path>.js, with an index on stdout.\n"
                     "  <outdir> defaults to `./scripts_<stem>`.  Omit <outdir> to only\n"
                     "  print the manifest (set --dry-run to skip writes too).\n");
        return 2;
    }

    const char* src_arg = argv[2];
    fs::path    src(src_arg);
    bool        dry_run = false;
    fs::path    outdir;
    bool        outdir_set = false;
    for (int i = 3; i < argc; i++) {
        std::string_view a = argv[i];
        if (a == "--dry-run") {
            dry_run = true;
        } else if (! outdir_set) {
            outdir     = a;
            outdir_set = true;
        } else {
            std::fprintf(stderr, "unknown arg: %.*s\n", int(a.size()), a.data());
            return 2;
        }
    }
    if (! outdir_set) {
        std::string stem = src.stem().string();
        if (stem.empty()) stem = "out";
        outdir = fs::path("scripts_" + stem);
    }

    std::string sceneBytes, err;
    if (! readSceneJson(src, sceneBytes, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    nlohmann::json scene;
    try {
        // WE scene.json occasionally contains // comments (defaultprojects/
        // fantasticcar has them in submesh blocks).  Enable comment-tolerant
        // parsing here to match the production loader.
        scene = nlohmann::json::parse(sceneBytes,
                                      /*cb=*/nullptr,
                                      /*allow_exceptions=*/true,
                                      /*ignore_comments=*/true);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "scene.json parse error: %s\n", ex.what());
        return 1;
    }

    std::vector<ScriptHit> hits;
    walkForScripts(scene, "", -1, "", hits);

    if (hits.empty()) {
        std::printf("no inlined scripts in scene.json (%s)\n", src.string().c_str());
        return 0;
    }

    if (! dry_run) {
        std::error_code ec;
        fs::create_directories(outdir / "scripts", ec);
        if (ec) {
            std::fprintf(stderr,
                         "mkdir %s: %s\n",
                         (outdir / "scripts").string().c_str(),
                         ec.message().c_str());
            return 1;
        }
    }

    size_t idx = 0;
    for (const auto& h : hits) {
        // Path in scene.json starts with "." from pathJoin's initial call;
        // strip it and simplify for the filename.
        std::string pathKey = h.path;
        if (! pathKey.empty() && pathKey.front() == '.') pathKey.erase(0, 1);
        for (auto& c : pathKey) {
            if (c == '.')
                c = '_';
            else if (c == '[')
                c = '_';
            else if (c == ']')
                c = '\0';
        }
        pathKey.erase(std::remove(pathKey.begin(), pathKey.end(), '\0'), pathKey.end());

        char idxBuf[16];
        std::snprintf(idxBuf, sizeof(idxBuf), "s%02zu_obj%d", idx, h.id);
        std::string name = idxBuf;
        if (! h.name.empty()) name += "_" + sanitizeForFilename(h.name);
        if (! pathKey.empty()) name += "__" + pathKey;
        name += ".js";

        fs::path dest = outdir / "scripts" / name;
        std::printf("%s  obj=%d name=\"%s\" path=%s len=%zu -> %s\n",
                    dry_run ? "DRY " : "WROTE",
                    h.id,
                    h.name.c_str(),
                    h.path.empty() ? "." : h.path.c_str(),
                    h.body.size(),
                    dest.string().c_str());
        if (! dry_run) {
            std::ofstream out(dest, std::ios::binary);
            if (! out) {
                std::fprintf(stderr, "write failed: %s\n", dest.string().c_str());
                return 1;
            }
            out.write(h.body.data(), h.body.size());
        }
        idx++;
    }

    std::printf("%zu inlined scripts\n", hits.size());
    return 0;
}

// Write N bytes little-endian.  Wraps ofstream put() for readability.
void write_u32_le(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = { uint8_t(v & 0xff),
                     uint8_t((v >> 8) & 0xff),
                     uint8_t((v >> 16) & 0xff),
                     uint8_t((v >> 24) & 0xff) };
    f.write(reinterpret_cast<const char*>(b), 4);
}

// Write a length-prefixed (u32 LE) string, matching wp-pkg layout.
void write_sized_string(std::ofstream& f, std::string_view s) {
    write_u32_le(f, static_cast<uint32_t>(s.size()));
    if (! s.empty()) f.write(s.data(), s.size());
}

// `pack` — inverse of `extract` (non-flat mode).  Walks <indir>, writes a
// .pkg with version PKGV0023 (matching the WE format our loader parses).
//
// Layout:
//   [u32 verlen][verstr][u32 count]
//   count × [u32 pathlen][pathstr][u32 offsetRel][u32 size]
//   [data blob]
// where offsetRel is relative to the end of the header.  This matches
// load_pkg above.  Input file order = deterministic fs::recursive_directory
// walk with stable sort, so repeated packs of the same dir produce byte-
// identical output.
int cmd_pack(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: wp-pkg pack <indir> <outpkg> [--version=PKGV0023]\n");
        return 2;
    }
    fs::path    indir   = argv[2];
    fs::path    outpkg  = argv[3];
    std::string version = "PKGV0023";
    for (int i = 4; i < argc; i++) {
        std::string_view a = argv[i];
        if (a.rfind("--version=", 0) == 0) {
            version = std::string(a.substr(10));
        } else {
            std::fprintf(stderr, "unknown flag: %.*s\n", int(a.size()), a.data());
            return 2;
        }
    }
    if (! fs::is_directory(indir)) {
        std::fprintf(stderr, "not a directory: %s\n", indir.c_str());
        return 1;
    }

    // Collect entries in a stable, sorted order so `pack` is deterministic.
    struct InEntry {
        std::string rel; // no leading slash (matches WE format)
        fs::path    abs;
        uint64_t    size;
    };
    std::vector<InEntry> entries;
    for (auto& p : fs::recursive_directory_iterator(indir)) {
        if (! p.is_regular_file()) continue;
        auto rel = fs::relative(p.path(), indir).generic_string();
        // Skip bookkeeping files that `extract --raw=off` writes alongside
        // the real assets.  These are not part of the scene.
        if (rel.size() >= 9 && rel.substr(rel.size() - 9) == ".info.txt") continue;
        // Skip patched/backup scene.json variants — only the real scene.json
        // belongs in the pkg.  Users can rename if they want them packed.
        if (rel == "scene.json.orig" || rel == "scene_nofx.json") continue;
        InEntry e;
        e.rel  = rel;
        e.abs  = p.path();
        e.size = fs::file_size(p.path());
        entries.push_back(std::move(e));
    }
    std::sort(entries.begin(), entries.end(), [](const InEntry& a, const InEntry& b) {
        return a.rel < b.rel;
    });

    if (entries.empty()) {
        std::fprintf(stderr, "no files to pack under %s\n", indir.c_str());
        return 1;
    }

    // Compute header size so we know each entry's data offset.  Offsets are
    // RELATIVE to dataStart (end of header), per load_pkg semantics.
    uint64_t header_size = 4 + version.size() + 4; // verlen + ver + count
    for (auto& e : entries) {
        header_size += 4 + e.rel.size() + 4 + 4; // pathlen + path + off + size
    }

    // Emit header with placeholder offsets, then append data and remember
    // each entry's offset as we go.
    std::ofstream f(outpkg, std::ios::binary | std::ios::trunc);
    if (! f) {
        std::fprintf(stderr, "cannot open output: %s\n", outpkg.c_str());
        return 1;
    }
    write_sized_string(f, version);
    write_u32_le(f, static_cast<uint32_t>(entries.size()));

    // Reserve entry header slots so we can come back and patch offsets.
    struct SlotPos {
        std::streampos off, len;
    };
    std::vector<SlotPos> slots;
    slots.reserve(entries.size());
    for (auto& e : entries) {
        write_sized_string(f, e.rel);
        SlotPos s;
        s.off = f.tellp();
        write_u32_le(f, 0); // offset placeholder
        s.len = f.tellp();
        write_u32_le(f, static_cast<uint32_t>(e.size));
        slots.push_back(s);
    }

    auto data_start = f.tellp();
    if (static_cast<uint64_t>(data_start) != header_size) {
        std::fprintf(stderr,
                     "pack: header size mismatch (got %lld, expected %llu)\n",
                     static_cast<long long>(data_start),
                     static_cast<unsigned long long>(header_size));
        return 1;
    }

    uint32_t          cur = 0;
    std::vector<char> buf;
    for (size_t i = 0; i < entries.size(); i++) {
        auto& e = entries[i];
        if (e.size > 0xffffffffull) {
            std::fprintf(stderr, "entry too large (>4GiB): %s\n", e.rel.c_str());
            return 1;
        }
        auto entry_start = f.tellp();
        if (static_cast<uint64_t>(entry_start) != header_size + cur) {
            std::fprintf(stderr, "pack: offset drift at entry %zu\n", i);
            return 1;
        }

        // Patch this entry's relative offset in the header.
        auto saved = f.tellp();
        f.seekp(slots[i].off);
        write_u32_le(f, cur);
        f.seekp(saved);

        // Copy bytes from source file.
        std::ifstream src(e.abs, std::ios::binary);
        if (! src) {
            std::fprintf(stderr, "cannot read: %s\n", e.abs.c_str());
            return 1;
        }
        buf.resize(static_cast<size_t>(e.size));
        if (e.size && ! src.read(buf.data(), e.size)) {
            std::fprintf(stderr, "short read: %s\n", e.abs.c_str());
            return 1;
        }
        f.write(buf.data(), e.size);
        cur += static_cast<uint32_t>(e.size);
    }

    std::fprintf(stdout,
                 "wrote %s: %zu entries, %llu bytes payload\n",
                 outpkg.c_str(),
                 entries.size(),
                 static_cast<unsigned long long>(cur));
    return 0;
}

void print_usage() {
    std::fprintf(stderr,
                 "wp-pkg: Wallpaper Engine .pkg tool\n"
                 "\n"
                 "  wp-pkg list    <pkg>\n"
                 "  wp-pkg extract <pkg> <outdir> [--flat] [--dry-run] [--raw]\n"
                 "  wp-pkg pack    <indir> <outpkg> [--version=PKGV0023]\n"
                 "  wp-pkg scripts <pkg|dir> [<outdir>] [--dry-run]\n"
                 "\n"
                 "  --flat    group output by category (scripts/shaders/textures/...)\n"
                 "  --raw     skip .tex header sidecar\n"
                 "  --dry-run print targets without writing\n"
                 "\n"
                 "  pack recursively walks <indir>, sorts paths for\n"
                 "  deterministic output, and writes a .pkg whose load_pkg\n"
                 "  structure roundtrips with extract (non-flat mode).\n"
                 "\n"
                 "  scripts walks scene.json (either inside a .pkg or loose\n"
                 "  under <dir>) and dumps every inlined SceneScript to its own\n"
                 "  file with a one-line manifest on stdout.\n");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    std::string_view sub = argv[1];
    if (sub == "list") return cmd_list(argc, argv);
    if (sub == "extract") return cmd_extract(argc, argv);
    if (sub == "pack") return cmd_pack(argc, argv);
    if (sub == "scripts") return cmd_scripts(argc, argv);
    if (sub == "-h" || sub == "--help" || sub == "help") {
        print_usage();
        return 0;
    }
    std::fprintf(stderr, "unknown subcommand: %.*s\n", int(sub.size()), sub.data());
    print_usage();
    return 2;
}
