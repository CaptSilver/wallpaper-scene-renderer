#include <doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "WPCommon.hpp"
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

void appendFloat(std::vector<uint8_t>& v, float x) {
    uint32_t bits;
    std::memcpy(&bits, &x, sizeof(bits));
    appendInt32(v, static_cast<int32_t>(bits));
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

    TEST_CASE("9-byte non-null-terminated header does not OOB read") {
        // Fuzz-found regression. ReadVersion in WPCommon.hpp reads exactly 9
        // bytes into a char[9]; with no in-buffer null, the implicit
        // string_view(const char*) ctor inside sstart_with strlen'd past the
        // stack array. ASAN trip in 15s on the first libFuzzer run.
        std::vector<uint8_t> data(9, 0xff);
        fs::MemBinaryStream  f(std::move(data));
        WPMdl                mdl;
        auto                 r = parseWithWatchdog(f, "fuzz.mdl", mdl);
        CHECK(r.completed);
        CHECK(! r.ok);
    }

    TEST_CASE("Non-numeric MDLA version does not throw") {
        // Fuzz-found regression. The MDLA-section parser called std::stoi
        // on bytes 4-7 of the tag — for hostile inputs those aren't digits,
        // so stoi threw std::invalid_argument and abort()ed the process.
        // Now uses std::from_chars (non-throwing) like the rest of the parser.
        std::vector<uint8_t> data = mdlvHeader(13);
        appendInt32(data, 0);              // mdl_flag = 0 → puppet
        appendInt32(data, 1);
        appendInt32(data, 1);
        data.push_back(0);                 // empty mat_json_file
        appendInt32(data, 0);
        appendInt32(data, 0);              // first u32 = 0 → standard path
        appendInt32(data, 0);              // vertex_size = 0
        appendInt32(data, 0);              // indices_size = 0
        // MDLS skeleton tag (consumed by the alt-format scan).
        const char mdls_tag[9] = "MDLS0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdls_tag[i]));
        appendInt32(data, 0); // bones_file_end
        data.push_back(0); data.push_back(0); // bones_num = 0
        data.push_back(0); data.push_back(0); // unk
        // MDLA tag with non-numeric 4-byte version (hostile bytes).
        const char mdla_bad[8] = { 'M', 'D', 'L', 'A', 'X', 'Y', 'Z', 'W' };
        for (int i = 0; i < 8; i++) data.push_back(static_cast<uint8_t>(mdla_bad[i]));
        data.push_back('\0'); // null terminator for ReadStr

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "bad_mdla.mdl", mdl);
        CHECK(r.completed);
        CHECK(! r.ok);
    }

    TEST_CASE("Hostile indices_size does not OOM-resize") {
        // Fuzz-found regression #2. Puppet path: a u32 indices_size near
        // UINT32_MAX divided by singile_indices (6) still left ~715M as
        // indices_num, then mdl.indices.resize(indices_num) requested ~4 GB
        // and tripped libFuzzer's malloc limit. CountFitsStream now bounds
        // the count against bytes-remaining; parse should bail out cleanly.
        std::vector<uint8_t> data = mdlvHeader(13);
        appendInt32(data, 0);                      // mdl_flag = 0 → puppet
        appendInt32(data, 1);
        appendInt32(data, 1);
        data.push_back(0);                         // empty mat_json_file
        appendInt32(data, 0);
        appendInt32(data, 0);                      // first u32 = 0 → standard path
        appendInt32(data, 0);                      // vertex_size = 0
        appendInt32(data, 0xFFFFFFFC);             // indices_size hostile (multiple of 6)
        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "hostile.mdl", mdl);
        CHECK(r.completed);
        CHECK(! r.ok);
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

    // Regression from wallpaper 3363252053 (夜莺Night/Nightingale body+head).  The
    // body's MDLV0023 puppet has MDLS v4, MDAT v1, MDLA v6 sections back-to-back.
    // Between has_index and MDAT, the format inserts a per-bone u32 table guarded
    // by a 1-byte flag — total 1 + bones_num*4 bytes.  Without consuming it, the
    // parser entered the MDAT/MDLA scan loop with bytes still pending.  Most
    // puppets survived because ReadStr() chunked through the table NUL-by-NUL and
    // still landed on the tag; this one's last u32 ended in 0xFF, so ReadStr()
    // swallowed the 'MDAT'/'MDLA' tags inside a longer non-NUL-terminated string
    // and the parser ran to EOF reporting "static skinned puppet".  Result: 1
    // attachment ('头' on bone 4) and 2 animations dropped, so the head decoupled
    // from the body's bones.
    TEST_CASE("MDLS v4 bone-table is consumed before MDAT/MDLA scan") {
        std::vector<uint8_t> data = mdlvHeader(23);
        appendInt32(data, 0); // mdl_flag = 0 → puppet path
        appendInt32(data, 1);
        appendInt32(data, 1);
        data.push_back(0);                                 // empty mat_json_file
        appendInt32(data, 0);                              // zero after mat
        for (int i = 0; i < 24; i++) data.push_back(0);    // v17+ bbox padding
        appendInt32(data, 0x01800009);                     // std-format vertex herald
        appendInt32(data, 0);                              // vertex_size
        appendInt32(data, 0);                              // indices_size

        const char mdls_tag[9] = "MDLS0004";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdls_tag[i]));
        appendInt32(data, 0); // bones_file_end (unused)
        // Two bones so the bone-table block (bones_num*4) covers >1 entry.
        data.push_back(2);
        data.push_back(0); // bones_num = 2
        data.push_back(0);
        data.push_back(0); // unk

        for (int b = 0; b < 2; b++) {
            data.push_back(0);                                     // empty bone name
            appendInt32(data, 1);                                  // unk
            appendInt32(data, b == 0 ? (int32_t)0xFFFFFFFF : 0);   // parent
            appendInt32(data, 64);                                 // matrix size
            for (int i = 0; i < 16; i++) appendInt32(data, 0);     // identity-ish matrix
            data.push_back(0);                                     // empty sim_json
        }

        // mdls > 1 extras block.
        data.push_back(0);
        data.push_back(0);                                         // unk16 = 0
        data.push_back(0);                                         // has_trans = 0
        appendInt32(data, 0);                                      // size_unk = 0
        appendInt32(data, 0);                                      // unk
        data.push_back(0);                                         // has_offset_trans = 0
        data.push_back(0);                                         // has_index = 0

        // NEW: per-bone u32 table guarded by a 1-byte flag.  Pin 0xFF as the
        // very last byte before MDAT — exactly the failure mode that breaks the
        // fallback ReadStr() scan: with no NUL between the table and the tag,
        // an unfixed parser reads "...\xFFMDAT0001" as a 9+ char string and
        // misses the section.
        data.push_back(1);                                         // has_bone_table = 1
        // 2 bones × 4 bytes; last byte must be 0xFF.
        for (int i = 0; i < 7; i++) data.push_back(0);
        data.push_back(0xFF);

        // MDAT0001 with one attachment named "head" rigged to bone 0.
        const char mdat_tag[9] = "MDAT0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdat_tag[i]));
        appendInt32(data, 0);     // 4-byte skip
        data.push_back(1);
        data.push_back(0);        // num_attachments = 1 (u16)
        data.push_back(0);
        data.push_back(0);        // bone_index = 0 (u16)
        const char head_name[] = "head";
        for (char c : head_name) data.push_back(static_cast<uint8_t>(c));
        for (int i = 0; i < 16; i++) appendInt32(data, 0); // identity-ish 4x4

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "v4_bonetable.mdl", mdl);
        REQUIRE(r.completed);
        CHECK(r.ok);
        REQUIRE(mdl.puppet != nullptr);
        CHECK(mdl.mdlv == 23);
        CHECK(mdl.mdls == 4);
        // The attachment must be reachable — proving the parser found MDAT
        // despite the 0xFF byte immediately before the tag.
        REQUIRE(mdl.puppet->attachments.size() == 1);
        CHECK(mdl.puppet->attachments[0].name == "head");
        CHECK(mdl.puppet->attachments[0].bone_index == 0);
    }

    // Regression from Totoro 2891663007's body puppet (_0001_Totoro_puppet.mdl,
    // MDLV0019).  Its MDLA section is misaligned for this format variant, so the
    // second animation reads a garbage `b_num` that exceeds the stream.  The
    // parser correctly bails the animation track — but the old code did so with
    // an early `return true` BEFORE WPPuppet::prepared().  prepared() is the only
    // place that sizes m_final_affines and computes the bind-pose bone
    // transforms, so skipping it left genFrame() returning an empty span,
    // g_Bones never uploaded, and the skinned mesh collapsed to the origin — the
    // whole Totoro body rendered invisible.  The fix keeps the cleanly-parsed
    // animations, finalizes the puppet, and renders the static bind pose.
    TEST_CASE("Animation b_num overrun still finalizes the bind pose") {
        std::vector<uint8_t> data = mdlvHeader(19);
        appendInt32(data, 0); // mdl_flag = 0 → puppet path
        appendInt32(data, 1); // unk
        appendInt32(data, 1); // unk
        data.push_back(0);    // empty mat_json_file
        appendInt32(data, 0); // zero after mat
        for (int i = 0; i < 24; i++) data.push_back(0); // v17+ bbox padding
        appendInt32(data, 0x01800009);                  // std-format vertex herald
        appendInt32(data, 0);                           // vertex_size = 0
        appendInt32(data, 0);                           // indices_size = 0

        const char mdls_tag[9] = "MDLS0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdls_tag[i]));
        appendInt32(data, 0); // bones_file_end
        data.push_back(1);
        data.push_back(0); // bones_num = 1 (u16)
        data.push_back(0);
        data.push_back(0); // unk

        // One root bone with a non-identity bind transform (translate 10,20,0)
        // so we can confirm prepared() computed its world_transform.
        data.push_back(0);             // empty bone name
        appendInt32(data, 1);          // unk
        appendInt32(data, (int32_t)0xFFFFFFFF); // parent = root
        appendInt32(data, 64);         // matrix size
        // 4x4 column-major: identity rotation + translation in the last column.
        appendFloat(data, 1); appendFloat(data, 0); appendFloat(data, 0); appendFloat(data, 0);
        appendFloat(data, 0); appendFloat(data, 1); appendFloat(data, 0); appendFloat(data, 0);
        appendFloat(data, 0); appendFloat(data, 0); appendFloat(data, 1); appendFloat(data, 0);
        appendFloat(data, 10); appendFloat(data, 20); appendFloat(data, 0); appendFloat(data, 1);
        data.push_back(0);             // empty bone_simulation_json
        // mdls == 1 → no extras block.

        // MDLA0001 section with one animation whose b_num overruns the stream.
        const char mdla_tag[9] = "MDLA0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdla_tag[i]));
        appendInt32(data, 0); // end_size
        appendInt32(data, 1); // anim_num = 1
        appendInt32(data, 287); // anim.id (>0, first byte non-zero ends the pad scan)
        appendInt32(data, 0);   // unk
        data.push_back('A'); data.push_back(0);                          // anim.name = "A"
        for (char c : std::string("loop")) data.push_back((uint8_t)c);   // play mode
        data.push_back(0);
        appendFloat(data, 30.0f); // fps
        appendInt32(data, 60);    // length
        appendInt32(data, 0);     // unk
        appendInt32(data, 0x7FFFFFFF); // b_num — far exceeds remaining bytes → bail
        // EOF immediately after: nothing left for CountFitsStream(b_num).

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "totoro_body.mdl", mdl);
        REQUIRE(r.completed);
        CHECK(r.ok); // keep the puppet (static mesh + bones still valid)
        REQUIRE(mdl.puppet != nullptr);
        REQUIRE(mdl.puppet->bones.size() == 1u);

        // prepared() must have run: bind-pose world transform computed.
        CHECK(mdl.puppet->bones[0].world_transform.translation().x() == doctest::Approx(10.0f));
        CHECK(mdl.puppet->bones[0].world_transform.translation().y() == doctest::Approx(20.0f));

        // genFrame() must return a non-empty bind-pose span (size == bone count)
        // so g_Bones is uploaded; an empty span is exactly the invisible-body bug.
        WPPuppetLayer                              layer(mdl.puppet);
        std::vector<WPPuppetLayer::AnimationLayer> none;
        layer.prepared(none);
        auto span = layer.genFrame(0.0);
        REQUIRE(span.size() == mdl.puppet->bones.size());
        // Bind pose → identity skinning matrix.
        CHECK(span[0].matrix().isApprox(Eigen::Matrix4f::Identity(), 1e-3f));
    }

    // Anim-padding scan EOF test — line 500 `Tell() >= Size()` mutates to `>`.
    // Trailing-zero MDLA section means the scan loop reads past end-of-stream
    // looking for a non-zero byte that never comes.  The fixed code returns
    // cleanly at EOF; the mutated version hangs.
    TEST_CASE("MDLA with all-zero padding scan does not hang at EOF") {
        // Build minimal puppet header up through the MDLA tag, then write
        // anim_num=1 followed by infinite-zero padding (which exhausts the
        // stream without ever finding a non-zero byte).
        std::vector<uint8_t> data = mdlvHeader(13);
        appendInt32(data, 0); // mdl_flag = 0 → puppet path
        appendInt32(data, 1);
        appendInt32(data, 1);
        data.push_back(0);
        appendInt32(data, 0);
        appendInt32(data, 0x01800009); // std herald
        appendInt32(data, 0);          // vertex_size = 0
        appendInt32(data, 0);          // indices_size = 0

        const char mdls_tag[9] = "MDLS0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdls_tag[i]));
        appendInt32(data, 0); // bones_file_end
        data.push_back(0);
        data.push_back(0); // bones_num=0
        data.push_back(0);
        data.push_back(0); // unk

        // MDLA0001 with anim_num=1 then all-zero padding through EOF.
        const char mdla_tag[9] = "MDLA0001";
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(mdla_tag[i]));
        appendInt32(data, 0); // end_size
        appendInt32(data, 1); // anim_num = 1 → enters the scan loop
        // 64 bytes of zero padding — no non-zero anim id ever appears.
        for (int i = 0; i < 64; i++) data.push_back(0);

        fs::MemBinaryStream f(std::move(data));
        WPMdl               mdl;
        auto                r = parseWithWatchdog(f, "anim_padding_eof.mdl", mdl);
        REQUIRE(r.completed);
        // ParseStream must report failure (no non-zero byte found).
        CHECK_FALSE(r.ok);
    }

} // TEST_SUITE

TEST_SUITE("WPMdl_IndexPacking") {
    // U32SlotsForU16Triangles(N) = (N*3)/2 + 1
    // Tests lock in the `* 3` and `/ 2 + 1` arithmetic so cxx_mul_to_div and
    // cxx_div_to_mul mutants on `indices.resize(u16_count / 2 + 1)` produce
    // observable errors.
    TEST_CASE("zero triangles still yields one slot (for memcpy safety)") {
        CHECK(U32SlotsForU16Triangles(0) == 1);
    }
    TEST_CASE("one triangle needs two slots (3 u16s → 1 full u32 + 1 half)") {
        CHECK(U32SlotsForU16Triangles(1) == 2);
    }
    TEST_CASE("two triangles need four slots (6 u16s → 3 full u32 + 1 half)") {
        CHECK(U32SlotsForU16Triangles(2) == 4);
    }
    TEST_CASE("three triangles need five slots (9 u16s → 4 u32 + 1 half)") {
        // Distinguishes (N*3)/2+1 from (N/3)*2+1 or (N+3)/2+1.
        CHECK(U32SlotsForU16Triangles(3) == 5);
    }
    TEST_CASE("ten triangles need sixteen slots (30 u16s → 15 full + 1)") {
        CHECK(U32SlotsForU16Triangles(10) == 16);
    }
    TEST_CASE("hundred triangles need 151 slots (300 u16s → 150 full + 1)") {
        CHECK(U32SlotsForU16Triangles(100) == 151);
    }
    TEST_CASE("U16BytesForTriangles: zero triangles = zero bytes") {
        CHECK(U16BytesForTriangles(0) == 0);
    }
    TEST_CASE("U16BytesForTriangles: one triangle = 6 bytes (3 × u16)") {
        CHECK(U16BytesForTriangles(1) == 6);
    }
    TEST_CASE("U16BytesForTriangles: 100 triangles = 600 bytes") {
        // Kills `* 3 → / 3` and `* sizeof(u16) → / sizeof(u16)` mutants.
        CHECK(U16BytesForTriangles(100) == 600);
    }
    TEST_CASE("U16BytesForTriangles: triangle count scales linearly") {
        CHECK(U16BytesForTriangles(5) == 30);
        CHECK(U16BytesForTriangles(10) == 60);
        CHECK(U16BytesForTriangles(20) == 120);
    }
}

TEST_SUITE("WPCommon_CountFitsStream") {
    TEST_CASE("Count <= remaining bytes accepted") {
        std::vector<uint8_t> data(100, 0);
        fs::MemBinaryStream  f(std::move(data));
        CHECK(CountFitsStream(f, 100));
        CHECK(CountFitsStream(f, 0));
    }
    TEST_CASE("Count > remaining bytes rejected") {
        std::vector<uint8_t> data(50, 0);
        fs::MemBinaryStream  f(std::move(data));
        CHECK(! CountFitsStream(f, 51));
        CHECK(! CountFitsStream(f, 0xFFFFFFFFu));
    }
    TEST_CASE("Stride scales the bound") {
        std::vector<uint8_t> data(60, 0);
        fs::MemBinaryStream  f(std::move(data));
        CHECK(CountFitsStream(f, 10, 6));      // 10*6 = 60 fits
        CHECK(! CountFitsStream(f, 11, 6));    // 11*6 = 66 doesn't
    }
    TEST_CASE("Stride 0 rejected (degenerate)") {
        std::vector<uint8_t> data(100, 0);
        fs::MemBinaryStream  f(std::move(data));
        CHECK(! CountFitsStream(f, 1, 0));
    }
    TEST_CASE("After consuming bytes, remaining shrinks") {
        std::vector<uint8_t> data(100, 0);
        fs::MemBinaryStream  f(std::move(data));
        char buf[40];
        f.Read(buf, 40);
        CHECK(CountFitsStream(f, 60));
        CHECK(! CountFitsStream(f, 61));
    }
}

// ---------------------------------------------------------------------------
// Fuzz crash regression replay.
//
// Iterates tests/fixtures/fuzz_regressions/WPMdlParser/*.bin and feeds each
// file through the same entry point fuzz_WPMdlParser drives. The harness
// catches re-regressions: a fixed crash that creeps back in surfaces as a
// CHECK_NOTHROW failure with the filename in doctest's message.
// ---------------------------------------------------------------------------

#include "test_data_root.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>

TEST_SUITE("regression: minimised fuzz crashes") {
    TEST_CASE("regression: minimised fuzz crashes round-trip cleanly") {
        namespace fs2 = std::filesystem;
        const fs2::path dir = wallpaper::test::test_data_root()
                              / "fuzz_regressions" / "WPMdlParser";
        if (! fs2::exists(dir)) return; // no fixtures yet — pass
        for (auto& entry : fs2::directory_iterator(dir)) {
            if (entry.path().extension() != ".bin") continue;
            SUBCASE(entry.path().filename().string().c_str()) {
                std::ifstream in(entry.path(), std::ios::binary);
                std::vector<uint8_t> buf(std::istreambuf_iterator<char>(in), {});
                fs::MemBinaryStream f(std::move(buf));
                WPMdl mdl;
                CHECK_NOTHROW(
                    (void)WPMdlParser::ParseStream(f, "fuzz.mdl", mdl));
            }
        }
    }
}
