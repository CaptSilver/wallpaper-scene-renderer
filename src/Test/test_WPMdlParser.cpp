#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "WPMdlParser.hpp"

using namespace wallpaper;

namespace
{
// ParseStream on corrupt input should return almost instantly; 5s is very generous.
constexpr int kHangTimeoutMs = 5000;

struct ParseResult {
    bool completed;
    bool ok;
};

// Run ParseStream on a watchdog thread — reports hang rather than letting the
// test binary spin forever on a regression.
ParseResult parseWithWatchdog(fs::IBinaryStream& f, std::string_view path, WPMdl& mdl) {
    std::packaged_task<bool()> task([&]() {
        return WPMdlParser::ParseStream(f, path, mdl);
    });
    auto                       fut = task.get_future();
    std::thread                worker(std::move(task));
    bool                       completed =
        fut.wait_for(std::chrono::milliseconds(kHangTimeoutMs)) == std::future_status::ready;
    if (! completed) {
        worker.detach(); // cannot safely kill a stuck thread
        return { false, false };
    }
    bool ok = fut.get();
    worker.join();
    return { true, ok };
}

std::vector<uint8_t> mdlvHeader(int version) {
    char buf[9] {};
    std::snprintf(buf, sizeof(buf), "MDLV%04d", version);
    return std::vector<uint8_t>(buf, buf + 9);
}

void appendInt32(std::vector<uint8_t>& v, int32_t x) {
    for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>((x >> (i * 8)) & 0xff));
}

} // namespace

// ===========================================================================
// WPMdlParser EOF safety
//
// Regression from Sword Art Online wallpaper 3463520581. Static deform puppets
// (hair strands, body parts) ship without an MDLA animation section. The old
// scan loops — `do { ReadStr(); } while (mdType != "MDLA");` and the
// alt-format herald `while` — spun forever at EOF because ReadStr() returns an
// empty string past end-of-stream and ReadUint32() returns 0. These tests pin
// the new behavior: ParseStream returns within a bounded time on any input,
// and static skinned puppets parse successfully.
// ===========================================================================

TEST_SUITE("WPMdlParser_EOF_safety") {
    TEST_CASE("Empty stream does not hang") {
        std::vector<uint8_t> data;
        fs::MemBinaryStream  f(std::move(data));
        WPMdl                mdl;
        auto                 r = parseWithWatchdog(f, "empty.mdl", mdl);
        CHECK(r.completed);
    }

    TEST_CASE("Partial MDL header does not hang") {
        std::vector<uint8_t> data = { 'M', 'D', 'L', 'V' }; // truncated version tag
        fs::MemBinaryStream  f(std::move(data));
        WPMdl                mdl;
        auto                 r = parseWithWatchdog(f, "trunc.mdl", mdl);
        CHECK(r.completed);
    }

    TEST_CASE("Puppet MDL with no MDLA section parses as static skinned puppet") {
        // Reproduces the hair-back-big-chunk case: puppet vertex data, skeleton
        // header, but no animation section before EOF. The old code looped
        // forever scanning for MDLA. Fix: treat this as a valid static skinned
        // puppet with zero animations.
        std::vector<uint8_t> data = mdlvHeader(13);
        appendInt32(data, 0);          // mdl_flag = 0 → puppet path
        appendInt32(data, 1);          // unk
        appendInt32(data, 1);          // unk
        data.push_back(0);             // empty mat_json_file
        appendInt32(data, 0);          // zero after mat path
        appendInt32(data, 0x01800009); // std-format vertex size herald
        appendInt32(data, 0);          // vertex_size = 0
        appendInt32(data, 0);          // indices_size = 0

        // MDLS skeleton tag (9 bytes including null terminator).
        const char mdls_tag[9] = "MDLS0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdls_tag[i]));
        appendInt32(data, 0); // bones_file_end
        data.push_back(0);    // bones_num = 0 (u16)
        data.push_back(0);
        data.push_back(0); // unk u16
        data.push_back(0);
        // No bones, mdls == 1 so the extra trans/index blocks are skipped.
        // The MDLA-scan loop now runs at EOF and breaks out cleanly.

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "static_skinned.mdl", mdl);
        REQUIRE(r.completed);
        CHECK(r.ok);
        CHECK(mdl.is_puppet == true);
        REQUIRE(mdl.puppet != nullptr);
        CHECK(mdl.puppet->anims.size() == 0);
    }

    TEST_CASE("Alt-format herald never appearing does not hang") {
        // Triggers the other infinite-loop site: when `curr == 0` enables
        // alt-format detection but the herald value never appears before EOF.
        std::vector<uint8_t> data = mdlvHeader(13);
        appendInt32(data, 0); // mdl_flag = 0 → puppet path
        appendInt32(data, 1);
        appendInt32(data, 1);
        data.push_back(0); // empty mat_json_file
        appendInt32(data, 0);
        appendInt32(data, 0); // first uint32 = 0 → alt-format scan entry
        for (int i = 0; i < 200; i++) data.push_back(0);
        appendInt32(data, 0xDEADBEEF); // non-herald garbage
        for (int i = 0; i < 200; i++) data.push_back(0);

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "alt_herald.mdl", mdl);
        CHECK(r.completed);
    }

} // TEST_SUITE
