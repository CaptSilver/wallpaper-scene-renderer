#include <doctest.h>

#include "WPPkgFs.hpp"
#include "Fs/MemBinaryStream.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

// ---------------------------------------------------------------------------
// Binary builder helpers.
//
// The .pkg header format (driven by CreateFromStream + ReadSizedString):
//   i32  ver_len;       bytes ver[ver_len]
//   i32  entryCount
//   per-entry:
//     i32  name_len;    bytes name[name_len]
//     i32  offset       (relative to end-of-header)
//     i32  length
//   ... payload bytes ...
//
// All ints are little-endian.  Paths in the file table are lowercased and
// prefixed with '/' on construction.
//
// CreateFromStream does NOT read payloads, so in-memory builders only need
// to populate the header correctly; the resulting WPPkgFs's Open() will
// fail (no on-disk backing per the header doc-comment), but Contains() and
// the internal file table are exercised end-to-end.
// ---------------------------------------------------------------------------
namespace
{

void appendBytes(std::vector<uint8_t>& buf, const void* p, size_t n) {
    auto bp = static_cast<const uint8_t*>(p);
    buf.insert(buf.end(), bp, bp + n);
}

void appendI32(std::vector<uint8_t>& buf, int32_t v) {
    // little-endian, matches IBinaryStream::_ReadInt on LE host
    uint8_t b[4];
    b[0] = static_cast<uint8_t>(v & 0xFF);
    b[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    b[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
    appendBytes(buf, b, 4);
}

void appendSizedString(std::vector<uint8_t>& buf, const std::string& s) {
    appendI32(buf, static_cast<int32_t>(s.size()));
    appendBytes(buf, s.data(), s.size());
}

struct PkgEntrySpec {
    std::string name;
    int32_t     offset; // relative to end-of-header
    int32_t     length;
};

// Build a well-formed pkg header.  Returns the byte buffer; caller may
// append payload bytes afterward (those bytes are never read by
// CreateFromStream, but Open() on a real on-disk pkg would honor them).
std::vector<uint8_t> buildPkgHeader(const std::string&               ver,
                                    const std::vector<PkgEntrySpec>& entries) {
    std::vector<uint8_t> buf;
    appendSizedString(buf, ver);
    appendI32(buf, static_cast<int32_t>(entries.size()));
    for (const auto& e : entries) {
        appendSizedString(buf, e.name);
        appendI32(buf, e.offset);
        appendI32(buf, e.length);
    }
    return buf;
}

std::unique_ptr<WPPkgFs> parse(std::vector<uint8_t> bytes) {
    MemBinaryStream stream(std::move(bytes));
    return WPPkgFs::CreateFromStream(stream, "test.pkg");
}

// Writes a real pkg to a temp file so WPPkgFs::CreatePkgFs (not
// CreateFromStream) has an actual backing FILE* to open -- needed to
// exercise Open()'s pooling, which CreateFromStream's in-memory builders
// above can't reach (see "Open() on in-memory pkg returns null").
std::filesystem::path writeTempPkg(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<PkgEntrySpec> specs;
    std::vector<uint8_t>      payload;
    for (const auto& [name, content] : files) {
        specs.push_back(
            { name, static_cast<int32_t>(payload.size()), static_cast<int32_t>(content.size()) });
        appendBytes(payload, content.data(), content.size());
    }
    auto header = buildPkgHeader("PKGV0023", specs);
    header.insert(header.end(), payload.begin(), payload.end());

    static std::atomic<int> counter { 0 };
    auto                    path = std::filesystem::temp_directory_path() /
                                   ("wek-pkgfs-open-test-" + std::to_string(counter++) + ".pkg");
    std::ofstream           f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(header.data()), (std::streamsize)header.size());
    f.close();
    return path;
}

} // namespace

// ---------------------------------------------------------------------------
// Well-formed input
// ---------------------------------------------------------------------------
TEST_SUITE("WPPkgFs - well-formed") {
    TEST_CASE("empty entry table parses; no paths are Contained") {
        auto buf = buildPkgHeader("PKGV0001", {});
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK_FALSE(fs->Contains("/anything"));
        CHECK_FALSE(fs->Contains("/"));
        CHECK_FALSE(fs->Contains(""));
        // OpenW is always a nullptr (read-only fs)
        CHECK(fs->OpenW("/anything") == nullptr);
    }

    TEST_CASE("single entry → Contains finds the leading-slash path") {
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "scene.json", /*offset*/ 0, /*length*/ 64 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/scene.json"));
        CHECK_FALSE(fs->Contains("scene.json"));   // missing leading slash
        CHECK_FALSE(fs->Contains("/Scene.JSON2")); // wrong name
    }

    TEST_CASE("multi-entry pkg: every declared path is Contained") {
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "scene.json", /*off*/ 0, /*len*/ 100 },
                                      { "shaders/genericimage2.frag", /*off*/ 100, /*len*/ 250 },
                                      { "materials/foo.json", /*off*/ 350, /*len*/ 50 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/scene.json"));
        CHECK(fs->Contains("/shaders/genericimage2.frag"));
        CHECK(fs->Contains("/materials/foo.json"));
        CHECK_FALSE(fs->Contains("/missing.dat"));
    }

    TEST_CASE("Contains is case-insensitive (paths are lowercased on store)") {
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "Scene.JSON", 0, 10 },
                                      { "SHADERS/Foo.Frag", 10, 20 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        // Query in any case — all lookups go through ToLower
        CHECK(fs->Contains("/scene.json"));
        CHECK(fs->Contains("/SCENE.JSON"));
        CHECK(fs->Contains("/Scene.JSON"));
        CHECK(fs->Contains("/shaders/foo.frag"));
        CHECK(fs->Contains("/SHADERS/FOO.FRAG"));
    }

    TEST_CASE("empty version string parses (only length prefix, no bytes)") {
        // ReadSizedString allows len == 0 (just a 0 i32).  The pkg should
        // still parse, with no entries.
        auto buf = buildPkgHeader("", {});
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
    }

    TEST_CASE("entry with zero-length payload still registered in file table") {
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "empty.dat", 0, 0 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/empty.dat"));
    }

    TEST_CASE("entry with empty name produces a '/' path") {
        // Pathological but legal: name_len == 0 → path is "/" after the
        // "/" + name prepend.
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "", 0, 0 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/"));
    }

    TEST_CASE("Open() on in-memory pkg returns null (no on-disk backing)") {
        // The header doc-comment notes: CreateFromStream constructs the
        // file table but Open() will fail because there is no file on disk
        // at "test.pkg".  Verify that contract — we still get a non-null
        // WPPkgFs, but Open() yields nullptr for both present and absent
        // paths.
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "scene.json", 0, 16 },
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Open("/scene.json") == nullptr);
        CHECK(fs->Open("/missing.json") == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Open() against a real on-disk pkg -- proves the backing FILE* pool reuses
// a released stream instead of reopening, and that two streams held open at
// once don't share a seek cursor.
// ---------------------------------------------------------------------------
TEST_SUITE("WPPkgFs - Open on-disk") {
    TEST_CASE("Open() reuses a released backing stream instead of reopening") {
        auto path = writeTempPkg({ { "a.txt", "AAAA" }, { "b.txt", "BBBBB" } });
        auto pfs  = WPPkgFs::CreatePkgFs(path.string());
        REQUIRE(pfs);
        CHECK(pfs->DebugBackingOpenCount() == 0);

        {
            auto s = pfs->Open("/a.txt");
            REQUIRE(s);
            CHECK(pfs->DebugBackingOpenCount() == 1); // pool miss -- first ever open
            std::string buf(4, '\0');
            REQUIRE(s->Read(buf.data(), 4) == 4);
            CHECK(buf == "AAAA");
        } // s destroyed -- backing stream returns to the pool

        auto s2 = pfs->Open("/b.txt");
        REQUIRE(s2);
        CHECK(pfs->DebugBackingOpenCount() == 1); // reused -- no second fopen
        std::string buf2(5, '\0');
        REQUIRE(s2->Read(buf2.data(), 5) == 5);
        CHECK(buf2 == "BBBBB");

        std::filesystem::remove(path);
    }

    TEST_CASE("two concurrently-open streams over the same pkg don't cross-read") {
        auto path = writeTempPkg({ { "a.txt", "AAAA" }, { "b.txt", "BBBBB" } });
        auto pfs  = WPPkgFs::CreatePkgFs(path.string());
        REQUIRE(pfs);

        auto sa = pfs->Open("/a.txt"); // both held open at once -- neither pooled yet
        auto sb = pfs->Open("/b.txt");
        REQUIRE(pfs->DebugBackingOpenCount() == 2); // pool was empty for both

        // Interleave: read 2 bytes of A, 2 of B, 2 of A, 3 of B -- would
        // cross-contaminate if the two streams shared one seek cursor.
        std::string a(4, '\0'), b(5, '\0');
        REQUIRE(sa->Read(a.data(), 2) == 2);
        REQUIRE(sb->Read(b.data(), 2) == 2);
        REQUIRE(sa->Read(a.data() + 2, 2) == 2);
        REQUIRE(sb->Read(b.data() + 2, 3) == 3);
        CHECK(a == "AAAA");
        CHECK(b == "BBBBB");

        std::filesystem::remove(path);
    }

    TEST_CASE("two threads racing Open()/Read()/release on the same entry never see "
              "garbled bytes") {
        // The single-thread interleave tests above prove seek isolation
        // between two streams, but both Open() calls happen on one thread --
        // nothing here exercises the mutex the pool actually needs it for
        // (the audio-mixing thread and the asset-loading thread both open
        // against the same WPPkgFs). Two real threads hammering the same
        // entry is the only way to put the pool's pop-on-Open/push-on-release
        // pair under an actual race.
        auto path = writeTempPkg({ { "a.txt", "HELLOWORLD" } });
        auto pfs  = WPPkgFs::CreatePkgFs(path.string());
        REQUIRE(pfs);

        constexpr int     kIterationsPerThread = 500;
        std::atomic<bool> sawMismatch { false };

        auto worker = [&]() {
            for (int i = 0; i < kIterationsPerThread; ++i) {
                auto s = pfs->Open("/a.txt");
                if (! s) {
                    sawMismatch = true;
                    return;
                }
                std::string buf(10, '\0');
                auto        n = s->Read(buf.data(), buf.size());
                if (n != 10 || buf != "HELLOWORLD") {
                    sawMismatch = true;
                    return;
                }
                // `s` drops here, returning its backing stream to the pool
                // for the other thread (or this one's next iteration).
            }
        };

        std::thread t1(worker);
        std::thread t2(worker);
        t1.join();
        t2.join();

        CHECK_FALSE(sawMismatch.load());
        std::filesystem::remove(path);
    }
}

// ---------------------------------------------------------------------------
// Malformed input — every branch must return nullptr without crashing.
// ---------------------------------------------------------------------------
TEST_SUITE("WPPkgFs - malformed") {
    TEST_CASE("completely empty stream parses as empty pkg (short-read i32 = 0)") {
        // IBinaryStream::_ReadInt returns 0 on short read.  An empty stream
        // therefore produces: ver_len=0, ver="" (ReadSizedString accepts
        // len==0), entryCount=0.  Parse succeeds with an empty file table.
        // Document the actual behavior — this case is otherwise harmless.
        auto fs = parse({});
        REQUIRE(fs != nullptr);
        CHECK_FALSE(fs->Contains("/anything"));
    }

    TEST_CASE("garbage 3 bytes (cannot even read ver-len i32)") {
        // i32 ReadInt32 on a stream with <4 bytes left returns 0 (the
        // _ReadInt zero-fallback).  A zero-length version string is legal,
        // but the next ReadInt32 (entryCount) again sees <4 bytes → 0.
        // That parses to an empty pkg.  Use 5 garbage bytes to land a
        // non-zero ver-len that won't fit.
        std::vector<uint8_t> bytes = { 0xFF, 0xFF, 0xFF, 0x7F, 0x00 };
        auto                 fs    = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("negative version length → parse fails") {
        std::vector<uint8_t> bytes;
        appendI32(bytes, -1); // negative ver_len → ReadSizedString returns false
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("version length exceeds stream size → parse fails (no OOM)") {
        // ver_len declared as 1 GiB but stream only has 4 bytes total.
        // CountFitsStream must reject before any allocation.
        std::vector<uint8_t> bytes;
        appendI32(bytes, 1 << 30);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("ver-len fits but stream truncated mid-string") {
        // Declare ver_len=8 and write 4 bytes of ver — CountFitsStream
        // sees full size at the time of the check (the remaining bytes
        // include the ver+entryCount+entries), so the guard passes if the
        // declared size is <= total-remaining.  Truncate hard enough that
        // 8 bytes don't fit at all.
        std::vector<uint8_t> bytes;
        appendI32(bytes, 8);
        // only 4 bytes of payload follow → CountFitsStream(8) is false
        appendBytes(bytes, "PKGV", 4);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("negative entryCount → parse fails") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, -1);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entryCount overflows remaining stream → parse fails") {
        // entryCount = INT32_MAX with no entry bytes following — each
        // entry is at least 1 byte (CountFitsStream default stride), and
        // the remaining stream is empty, so the guard rejects.
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 0x7FFFFFFF);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entry name length negative → parse fails") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 1);  // entryCount = 1
        appendI32(bytes, -7); // bad name_len
        // even if we appended offset/length, the name read fails first
        appendI32(bytes, 0);
        appendI32(bytes, 0);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entry name length overruns stream → parse fails") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 1);
        appendI32(bytes, 1 << 30); // huge name_len, no bytes follow
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entry offset negative → parse fails") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 1);
        appendSizedString(bytes, "scene.json");
        appendI32(bytes, -1); // negative offset
        appendI32(bytes, 0);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entry length negative → parse fails") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 1);
        appendSizedString(bytes, "scene.json");
        appendI32(bytes, 0);
        appendI32(bytes, -1); // negative length
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("truncation mid-entry (offset present, length missing)") {
        // ReadInt32 returns 0 on short read.  An offset/length of 0 is
        // legal, so this case parses successfully — verify that the
        // file table still got the entry (a zero-length file).  This
        // documents the actual behavior: short reads of i32 yield 0,
        // not an error.
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 1);
        appendSizedString(bytes, "scene.json");
        // offset+length omitted — both ReadInt32 calls fall through to 0
        auto fs = parse(std::move(bytes));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/scene.json"));
    }

    TEST_CASE("entryCount = 0 with junk after → parses as empty fs") {
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 0);
        // trailing junk is never read
        appendBytes(bytes, "trailing garbage payload", 24);
        auto fs = parse(std::move(bytes));
        REQUIRE(fs != nullptr);
        CHECK_FALSE(fs->Contains("/anything"));
    }
}

// ---------------------------------------------------------------------------
// Adversarial inputs derived from fuzz-class regressions.  The fuzz
// harness's Phase 1 drives arbitrary bytes through CreateFromStream;
// these cases pin specific shapes that historically caused OOM / overrun
// before CountFitsStream was inserted.
// ---------------------------------------------------------------------------
TEST_SUITE("WPPkgFs - fuzz regressions") {
    TEST_CASE("0xFFFFFFFF ver-len does not allocate 4 GiB") {
        // Pre-guard, ReadSizedString would resize a string to 4 GiB and
        // OOM.  CountFitsStream now rejects against bytes-remaining
        // first.
        std::vector<uint8_t> bytes;
        appendI32(bytes, static_cast<int32_t>(0xFFFFFFFFu)); // = -1, also rejected by sign check
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("0x7FFFFFFF entryCount does not reserve 2 GiB") {
        // entryCount = INT32_MAX; without a follow-up CountFitsStream
        // guard the pkgfiles.reserve() call would OOM.  Rejected by
        // CountFitsStream against bytes-remaining / stride==1.
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "PKGV0001");
        appendI32(bytes, 0x7FFFFFFF);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("entryCount fits stride-1 budget but name eats it all") {
        // entryCount = (bytes_remaining) is exactly accepted (stride==1
        // means each entry needs only 1 byte).  The per-entry name read
        // then asks for more bytes than remain → entry parse fails.
        // Verify clean nullptr (not a partial fs).
        std::vector<uint8_t> bytes;
        appendSizedString(bytes, "v");
        appendI32(bytes, 4);
        // After this point only 4 bytes remain in the stream — exactly
        // enough for the CountFitsStream(4) check to pass.  Then we
        // declare a huge name_len for entry 0 that cannot be satisfied.
        appendI32(bytes, 0x7FFFFFFF);
        auto fs = parse(std::move(bytes));
        CHECK(fs == nullptr);
    }

    TEST_CASE("path collision: case-different names collapse to one entry") {
        // Two entries with names that lowercase to the same key — the
        // unordered_map insert keeps the FIRST inserted value (insert
        // is a no-op on existing key).  Verify both names match
        // Contains, but only the first survives in m_files semantics.
        auto buf = buildPkgHeader("PKGV0001",
                                  {
                                      { "FOO.txt", 0, 10 },
                                      { "foo.txt", 50, 20 }, // same lowercase key
                                  });
        auto fs  = parse(std::move(buf));
        REQUIRE(fs != nullptr);
        CHECK(fs->Contains("/foo.txt"));
        CHECK(fs->Contains("/FOO.txt"));
        // Both queries should hit; no crash regardless of which entry
        // won the map insert.
    }
}

// ---------------------------------------------------------------------------
// Fuzz crash regression replay.
//
// Iterates tests/fixtures/fuzz_regressions/WPPkgFs/*.bin and feeds each file
// through the same entry point fuzz_WPPkgFs drives (CreateFromStream via
// the local parse() helper). A re-regression surfaces as a CHECK_NOTHROW
// failure with the filename in doctest's message.
// ---------------------------------------------------------------------------

#include "test_data_root.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

TEST_SUITE("regression: minimised fuzz crashes") {
    TEST_CASE("regression: minimised fuzz crashes round-trip cleanly") {
        namespace fs2       = std::filesystem;
        const fs2::path dir = wallpaper::test::test_data_root() / "fuzz_regressions" / "WPPkgFs";
        if (! fs2::exists(dir)) return;
        for (auto& entry : fs2::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            SUBCASE(entry.path().filename().string().c_str()) {
                std::ifstream        in(entry.path(), std::ios::binary);
                std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});
                MemBinaryStream      stream(std::move(buf));
                CHECK_NOTHROW((void)WPPkgFs::CreateFromStream(stream, "fuzz.pkg"));
            }
        }
    }
}
