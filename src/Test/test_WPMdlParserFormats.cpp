#include <doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "Fs/MemBinaryStream.h"
#include "Scene/SceneMesh.h"
#include "WPMdlParser.hpp"
#include "WPPuppet.hpp"
#include "WPShaderParser.hpp"

using namespace wallpaper;

// ===========================================================================
// Binary fixture builder — grows a std::vector<uint8_t> via typed appends.
// Layout matches what WPMdlParser::ParseStream expects in-order.
// ===========================================================================
namespace
{

struct Bytes {
    std::vector<uint8_t> data;

    void u8(uint8_t v) { data.push_back(v); }
    void u16(uint16_t v) {
        data.push_back(v & 0xff);
        data.push_back((v >> 8) & 0xff);
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) data.push_back((v >> (i * 8)) & 0xff);
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    // Null-terminated string
    void str(std::string_view s) {
        for (char c : s) data.push_back(static_cast<uint8_t>(c));
        data.push_back(0);
    }
    // 4x4 identity matrix (column-major), 64 bytes
    void identity_mat4() {
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) f32(r == c ? 1.0f : 0.0f);
        }
    }
    // 4x4 matrix with translation column = {tx,ty,tz}, otherwise identity.
    void translated_mat4(float tx, float ty, float tz) {
        // column 0
        f32(1);
        f32(0);
        f32(0);
        f32(0);
        // column 1
        f32(0);
        f32(1);
        f32(0);
        f32(0);
        // column 2
        f32(0);
        f32(0);
        f32(1);
        f32(0);
        // column 3 (translation)
        f32(tx);
        f32(ty);
        f32(tz);
        f32(1);
    }

    void append_mdlv(int version) {
        char buf[9] {};
        std::snprintf(buf, sizeof(buf), "MDLV%04d", version);
        for (int i = 0; i < 9; i++) data.push_back(static_cast<uint8_t>(buf[i]));
    }
};

std::vector<uint8_t> takeBuffer(Bytes&& b) { return std::move(b.data); }

} // namespace

// ===========================================================================
// Non-puppet model paths — flag 9 / 11 / 15 / 39
// ===========================================================================

TEST_SUITE("WPMdlParser.Model") {
    TEST_CASE("flag 9: pos(3) + texcoord(2) with 1 submesh, 1 triangle") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(9); // mdl_flag
        b.i32(1); // unk
        b.u32(1); // submesh_count

        // submesh 0
        b.str("mat.json");
        b.i32(0);
        // vertex_size = 3 vertices * 5 floats * 4 bytes = 60
        b.u32(3 * 5 * 4);
        // 3 verts: pos(3) + uv(2)
        for (int i = 0; i < 3; i++) {
            b.f32((float)i);       // pos.x
            b.f32((float)(i * 2)); // pos.y
            b.f32((float)(i * 3)); // pos.z
            b.f32(0.5f * i);       // u
            b.f32(0.25f * i);      // v
        }
        // indices_size = 1 triangle * 3 u16 * 2 bytes = 6
        b.u32(6);
        b.u16(0);
        b.u16(1);
        b.u16(2);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "m9.mdl", mdl));

        CHECK_FALSE(mdl.is_puppet);
        // Non-puppet branch never instantiates the WPPuppet shared_ptr.
        // Itachi Uchiha (2810492318) ships a `*_puppet.mdl` file that's
        // actually a flag=9 mesh, so callers that received the MDL via
        // a scene.json "puppet:" field must check is_puppet / puppet
        // before dereferencing puppet->bones.
        CHECK(mdl.puppet == nullptr);
        REQUIRE(mdl.submeshes.size() == 1);
        const auto& s = mdl.submeshes[0];
        CHECK(s.mat_json_file == "mat.json");
        CHECK(s.vertexs.size() == 3);
        CHECK_FALSE(s.has_normals);
        CHECK_FALSE(s.has_tangents);
        CHECK_FALSE(s.has_texcoord1);
        CHECK(s.vertexs[1].position[0] == doctest::Approx(1.0f));
        CHECK(s.vertexs[2].texcoord[0] == doctest::Approx(1.0f));
        CHECK(s.indices.size() == 1);
        CHECK(s.indices[0][2] == 2);
    }

    TEST_CASE("flag 11: pos + normal + texcoord") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(11);
        b.i32(1);
        b.u32(1);
        b.str("matN.json");
        b.i32(0);
        b.u32(2 * 8 * 4); // 2 verts * 8 floats
        for (int i = 0; i < 2; i++) {
            b.f32(i);
            b.f32(0);
            b.f32(0); // pos
            b.f32(0);
            b.f32(1);
            b.f32(0); // normal
            b.f32(0.1f * i);
            b.f32(0.2f * i); // uv
        }
        b.u32(0); // no indices

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "m11.mdl", mdl));

        REQUIRE(mdl.submeshes.size() == 1);
        const auto& s = mdl.submeshes[0];
        CHECK(s.has_normals);
        CHECK_FALSE(s.has_tangents);
        CHECK(s.vertexs.size() == 2);
        CHECK(s.vertexs[0].normal[1] == doctest::Approx(1.0f));
    }

    // Regression: mdlv=16 (3body's Hollow Cylinder.mdl) has an extra
    // per-submesh `flags_repeat` uint32 between the `int32(0)` padding and
    // vertex_size.  Without the 4-byte skip we were reading `flags_repeat`
    // (value 15, same as the top-level mdl_flag) as vertex_size → failed
    // stride check and the cylinder mesh never loaded.  Stars (mdlv=23) use
    // a larger 28-byte bbox+flags block; older mdlv only gets the 4 bytes.
    TEST_CASE("flag 15, mdlv 16: skips flags_repeat before vertex_size") {
        Bytes b;
        b.append_mdlv(16);
        b.i32(15);
        b.i32(1);
        b.u32(1);
        b.str("hollow.json");
        b.i32(0);
        b.u32(15);         // flags_repeat — must be skipped by parser
        b.u32(1 * 12 * 4); // 1 vert * 12 floats (48 bytes)
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.u32(0); // empty indices

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "hollow.mdl", mdl));
        REQUIRE(mdl.submeshes.size() == 1);
        CHECK(mdl.submeshes[0].vertexs.size() == 1);
        CHECK(mdl.submeshes[0].has_tangents);
        CHECK(mdl.submeshes[0].vertexs[0].tangent[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("flag 15: pos + normal + tangent + texcoord") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(15);
        b.i32(1);
        b.u32(1);
        b.str("matNT.json");
        b.i32(0);
        b.u32(1 * 12 * 4); // 1 vert * 12 floats
        // pos
        b.f32(1);
        b.f32(2);
        b.f32(3);
        // normal
        b.f32(0);
        b.f32(1);
        b.f32(0);
        // tangent (4)
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        // uv
        b.f32(0.5f);
        b.f32(0.5f);
        b.u32(0);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "m15.mdl", mdl));

        REQUIRE(mdl.submeshes.size() == 1);
        const auto& s = mdl.submeshes[0];
        CHECK(s.has_normals);
        CHECK(s.has_tangents);
        CHECK_FALSE(s.has_texcoord1);
        CHECK(s.vertexs[0].tangent[3] == doctest::Approx(1.0f));
    }

    TEST_CASE("flag 39: adds secondary texcoord") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(39);
        b.i32(1);
        b.u32(1);
        b.str("m.json");
        b.i32(0);
        b.u32(1 * 14 * 4); // 1 vert, 14 floats
        // pos/normal/tangent/uv/uv1
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0.1f);
        b.f32(0.2f);
        b.f32(0.3f);
        b.f32(0.4f);
        b.u32(0);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "m39.mdl", mdl));
        REQUIRE(mdl.submeshes.size() == 1);
        CHECK(mdl.submeshes[0].has_texcoord1);
        CHECK(mdl.submeshes[0].vertexs[0].texcoord1[0] == doctest::Approx(0.3f));
    }

    TEST_CASE("model v23: bbox+flags prefix and 6-byte trailing padding") {
        Bytes b;
        b.append_mdlv(23); // v23+
        b.i32(15);
        b.i32(1);
        b.u32(1);
        b.str("v23.json");
        b.i32(0);
        // v23 bbox: 6 floats + 1 uint32 flags_repeat
        for (int i = 0; i < 6; i++) b.f32((float)i);
        b.u32(15);
        // vertex block
        b.u32(1 * 12 * 4);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        // indices
        b.u32(0);
        // 6 bytes trailing padding
        for (int i = 0; i < 6; i++) b.u8(0xAB);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "v23.mdl", mdl));
        REQUIRE(mdl.submeshes.size() == 1);
        CHECK(mdl.submeshes[0].has_tangents);
    }

    TEST_CASE("model with bad vertex_size % stride fails cleanly") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(9);
        b.i32(1);
        b.u32(1);
        b.str("bad.json");
        b.i32(0);
        b.u32(7); // not a multiple of 20 (flag9 stride)
        // buffer ends here
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "bad.mdl", mdl));
    }

    TEST_CASE("model with bad indices_size % 6 fails") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(9);
        b.i32(1);
        b.u32(1);
        b.str("bad.json");
        b.i32(0);
        b.u32(5 * 4); // exactly one vertex's worth (20 bytes)
        for (int i = 0; i < 5; i++) b.f32(0);
        b.u32(5); // not a multiple of 6
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "bad.mdl", mdl));
    }

    TEST_CASE("multiple submeshes parse independently") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(11);
        b.i32(1);
        b.u32(2); // two submeshes

        // submesh 0 — 1 vertex, flag 11 stride 32 bytes
        b.str("a.json");
        b.i32(0);
        b.u32(32);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.u32(0);

        // submesh 1
        b.str("b.json");
        b.i32(0);
        b.u32(32);
        b.f32(9);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(0);
        b.f32(1);
        b.f32(0.5f);
        b.f32(0.5f);
        b.u32(0);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "multi.mdl", mdl));
        REQUIRE(mdl.submeshes.size() == 2);
        CHECK(mdl.submeshes[0].mat_json_file == "a.json");
        CHECK(mdl.submeshes[1].mat_json_file == "b.json");
        CHECK(mdl.submeshes[1].vertexs[0].position[0] == doctest::Approx(9.0f));
    }

} // Model

// ===========================================================================
// Puppet path — shared builder for minimal + rich puppets
// ===========================================================================

namespace
{

// Append MDLS skeleton header tag (9 bytes, null-terminated) so the MDLS
// scan loop in ParseStream finds it.
void appendMdls(Bytes& b, int v) {
    char buf[9] {};
    std::snprintf(buf, sizeof(buf), "MDLS%04d", v);
    for (int i = 0; i < 9; i++) b.u8(static_cast<uint8_t>(buf[i]));
}

// Standard vertex-size herald used by all non-alt puppets.
constexpr uint32_t kStdHerald = 0x01800009u;
constexpr uint32_t kAltHerald = 0x0180000Fu;

} // namespace

TEST_SUITE("WPMdlParser.Puppet") {
    TEST_CASE("minimal puppet: no vertices, no bones, no animation") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);             // mdl_flag != 9/11/15/39 → puppet path
        b.i32(1);             // unk
        b.i32(1);             // unk
        b.str("puppet.json"); // mat_json_file
        b.i32(0);
        b.u32(kStdHerald); // first uint32 is the std herald
        b.u32(0);          // vertex_size = 0
        b.u32(0);          // indices_size = 0
        appendMdls(b, 1);
        b.u32(0); // bones_file_end
        b.u16(0); // bones_num
        b.u16(0); // unk

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "minimal.mdl", mdl));

        CHECK(mdl.is_puppet);
        CHECK(mdl.mat_json_file == "puppet.json");
        REQUIRE(mdl.puppet != nullptr);
        CHECK(mdl.puppet->bones.size() == 0);
        CHECK(mdl.puppet->anims.size() == 0);
        CHECK(mdl.mdls == 1);
    }

    TEST_CASE("puppet with one bone stores parent and translation") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("mat.json");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(1); // bones_num = 1
        b.u16(0);

        // bone 0
        b.str("root");
        b.i32(0);           // unk
        b.u32(0xFFFFFFFFu); // parent = no-parent sentinel
        b.u32(64);          // matrix size
        b.translated_mat4(3.0f, 4.0f, 5.0f);
        b.str(""); // empty bone_simulation_json

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "onebone.mdl", mdl));
        REQUIRE(mdl.puppet != nullptr);
        REQUIRE(mdl.puppet->bones.size() == 1);
        CHECK(mdl.puppet->bones[0].noParent());
        auto t = mdl.puppet->bones[0].transform.translation();
        CHECK(t.x() == doctest::Approx(3.0f));
        CHECK(t.y() == doctest::Approx(4.0f));
        CHECK(t.z() == doctest::Approx(5.0f));
    }

    TEST_CASE("bone size != 64 aborts parse") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(1);
        b.u16(0);
        b.str("b");
        b.i32(0);
        b.u32(0xFFFFFFFFu);
        b.u32(32); // invalid — should be 64
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "badbone.mdl", mdl));
    }

    TEST_CASE("bone parent == own index triggers rescue (boundary)") {
        // The rescue condition is `bone.parent >= i && ! bone.noParent()`.  A
        // parent-equals-index bone exercises the `>=` boundary — under `>`
        // mutation the self-parent wouldn't be rescued and noParent() stays false.
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(1);
        b.u16(0);
        b.str("self");
        b.i32(0);
        b.u32(0); // parent = 0 == index 0 → original rescues (0 >= 0 true)
        b.u32(64);
        b.identity_mat4();
        b.str("");
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "self.mdl", mdl));
        REQUIRE(mdl.puppet->bones.size() == 1);
        CHECK(mdl.puppet->bones[0].noParent()); // rescued via >=
    }

    TEST_CASE("bone parent >= own index is rescued to no-parent") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(1);
        b.u16(0);
        b.str("b0");
        b.i32(0);
        b.u32(5); // invalid forward parent ref — should be rescued to 0xFFFFFFFF
        b.u32(64);
        b.identity_mat4();
        b.str("");
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "badparent.mdl", mdl));
        REQUIRE(mdl.puppet->bones.size() == 1);
        CHECK(mdl.puppet->bones[0].noParent());
    }

    TEST_CASE("puppet with MDAT attachment parses attachment list") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0); // no bones

        // MDAT section (9-byte tag including null), then uint32 + uint16 count
        b.str("MDAT0001"); // 9 bytes (incl. null)
        b.u32(0);          // 4 unused bytes
        b.u16(1);          // one attachment

        // The u16 immediately before the name is the bone_index this
        // attachment is rigged to.  bone_index=0 here means "rigged to root".
        b.u16(0);
        b.str("hand"); // name
        b.translated_mat4(1.0f, 2.0f, 3.0f);

        // MDLA sentinel — parser loops until mdType=="MDLA" OR EOF.
        b.str("MDLA0000"); // MDLA version 0 → skip body
        // mdla == 0 → parser does NOT read end_size/anim_num, so we stop here.

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "mdat.mdl", mdl));
        REQUIRE(mdl.puppet != nullptr);
        REQUIRE(mdl.puppet->attachments.size() == 1);
        CHECK(mdl.puppet->attachments[0].name == "hand");
        CHECK(mdl.puppet->attachments[0].bone_index == 0u);
        auto t = mdl.puppet->attachments[0].transform.translation();
        CHECK(t.x() == doctest::Approx(1.0f));
        CHECK(t.y() == doctest::Approx(2.0f));
        CHECK(t.z() == doctest::Approx(3.0f));
    }

    TEST_CASE("MDAT attachment captures non-zero bone_index") {
        // Reproduces SAO Asuna body's 'head' attachment which is rigged
        // to bone[2].  Verifies the u16 immediately before the name is
        // actually parsed (we used to skip it).
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        b.str("MDAT0001");
        b.u32(0);
        b.u16(2); // two attachments

        b.u16(0); // bone_index = 0 (root)
        b.str("Attachment");
        b.translated_mat4(0.0f, 0.0f, 0.0f);

        b.u16(2); // bone_index = 2 (head bone, like SAO asuna body)
        b.str("head");
        b.translated_mat4(-32.51f, 116.41f, 0.0f);

        b.str("MDLA0000");

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "mdat_bones.mdl", mdl));
        REQUIRE(mdl.puppet != nullptr);
        REQUIRE(mdl.puppet->attachments.size() == 2);
        CHECK(mdl.puppet->attachments[0].name == "Attachment");
        CHECK(mdl.puppet->attachments[0].bone_index == 0u);
        CHECK(mdl.puppet->attachments[1].name == "head");
        CHECK(mdl.puppet->attachments[1].bone_index == 2u);
        auto t = mdl.puppet->attachments[1].transform.translation();
        CHECK(t.x() == doctest::Approx(-32.51f));
        CHECK(t.y() == doctest::Approx(116.41f));
    }

    TEST_CASE("puppet with MDLA animation + keyframe events") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0); // no bones

        // MDLA 0001 with 1 animation
        b.str("MDLA0001");
        b.u32(0); // end_size
        b.u32(1); // anim_num = 1

        // anim padding + id (scan-for-nonzero then seek -1 then read int32)
        // For this to work simply, write a single non-zero byte representing the
        // LSB of the id, and make sure the scan finds it first.
        b.u32(42);     // non-zero low byte → id = 42 (full 32-bit)
        b.i32(0);      // unk
        b.str("walk"); // name
        b.str("loop"); // play mode
        b.f32(30.0f);  // fps
        b.i32(60);     // length
        b.i32(0);      // unk

        b.u32(1); // b_num = 1 bone-frames array
        // one bframes entry: int32 unk + uint32 byte_size + frames
        b.i32(0);
        b.u32(36); // 1 frame = 9 floats = 36 bytes
        for (int i = 0; i < 9; i++) b.f32(0);

        // event_count for non-alt, mdla != 3 → uint32 count
        b.u32(1);    // one event
        b.f32(0.5f); // event timestamp
        b.str(R"({"frame":5,"name":"footstep"})");

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "anim.mdl", mdl));
        REQUIRE(mdl.puppet != nullptr);
        REQUIRE(mdl.puppet->anims.size() == 1);
        const auto& a = mdl.puppet->anims[0];
        CHECK(a.id == 42);
        CHECK(a.name == "walk");
        CHECK(a.fps == doctest::Approx(30.0f));
        CHECK(a.length == 60);
        REQUIRE(a.events.size() == 1);
        CHECK(a.events[0].name == "footstep");
        CHECK(a.events[0].frame == 5);
    }

    TEST_CASE("puppet with negative animation id fails") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);
        b.str("MDLA0001");
        b.u32(0);
        b.u32(1);
        // id = -1 (four 0xFF bytes). First non-zero byte scan finds 0xFF → rewind and read int32.
        b.u8(0xFF);
        b.u8(0xFF);
        b.u8(0xFF);
        b.u8(0xFF);
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "negid.mdl", mdl));
    }

    TEST_CASE("puppet alt-format (vertex size herald after scan)") {
        // The scan loop must ITERATE past non-zero non-herald noise before landing
        // on the herald — if it exits early (mutation of `!=` → `==`), vertex_size
        // becomes that noise (e.g. 0xDEADBEEF) which fails the stride check.
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);

        b.u32(0);           // entry: curr=0 triggers alt scan
        b.u32(0xDEADBEEFu); // non-zero, non-herald — scan must iterate past
        b.u32(0xCAFEBABEu); // ditto
        b.u32(kAltHerald);  // scan exits here
        b.u32(0);           // vertex_size = 0
        b.u32(0);           // indices_size = 0
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "alt.mdl", mdl));
        CHECK(mdl.is_puppet);
    }

    TEST_CASE("bad puppet vertex size fails") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(7); // not multiple of singile_vertex (52)
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "badv.mdl", mdl));
    }

    TEST_CASE("bad puppet indices size fails") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0); // vertex_size = 0
        b.u32(5); // indices_size not multiple of 6
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "badi.mdl", mdl));
    }

    TEST_CASE("bone frame size != multiple of 36 aborts parse") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);
        b.str("MDLA0001");
        b.u32(0);
        b.u32(1);
        b.u32(42); // id
        b.i32(0);
        b.str("a");
        b.str("loop");
        b.f32(30);
        b.i32(10);
        b.i32(0);
        b.u32(1);
        b.i32(0);
        b.u32(5); // invalid frame byte_size
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        CHECK_FALSE(WPMdlParser::ParseStream(f, "badframe.mdl", mdl));
    }

    TEST_CASE("puppet with vertices + indices + MDLS2 extras") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("mat");
        b.i32(0);
        b.u32(kStdHerald);
        // one vertex = 52 bytes (singile_vertex)
        b.u32(52);
        // position(3) + blend_indices(4 u32) + weight(4 f) + texcoord(2 f)
        b.f32(0.5f);
        b.f32(-0.25f);
        b.f32(1.0f); // pos
        b.u32(0);
        b.u32(1);
        b.u32(2);
        b.u32(3); // blend indices
        b.f32(0.25f);
        b.f32(0.25f);
        b.f32(0.25f);
        b.f32(0.25f); // weights
        b.f32(0.1f);
        b.f32(0.9f); // uv
        // one triangle = 6 bytes
        b.u32(6);
        b.u16(0);
        b.u16(0);
        b.u16(0);

        appendMdls(b, 2); // MDLS v2 — triggers mdls > 1 extras block
        b.u32(0);
        b.u16(0);
        b.u16(0); // bones_file_end, bones_num=0, unk

        // MDLS v2 extras
        b.i16(0); // unk = 0 (expected)
        b.u8(0);  // has_trans = 0
        b.u32(0); // size_unk = 0
        b.u32(0); // unk
        b.u8(0);  // has_offset_trans = 0
        b.u8(0);  // has_index = 0

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "puppetv2.mdl", mdl));
        CHECK(mdl.vertexs.size() == 1);
        CHECK(mdl.indices.size() == 1);
        CHECK(mdl.mdls == 2);
        CHECK(mdl.vertexs[0].position[0] == doctest::Approx(0.5f));
        CHECK(mdl.vertexs[0].texcoord[1] == doctest::Approx(0.9f));
    }

    TEST_CASE("puppet with MDLS2 extras + downstream MDAT catches skip-arithmetic mutants") {
        // Fills the mdls>1 skip blocks with NON-ZERO filler so that a mutation of
        // the SeekCur multiplications (e.g. bones_num * 64 → bones_num / 64 = 0)
        // leaves the stream misaligned and the downstream MDAT tag lookup fails.
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 2);
        b.u32(0);
        b.u16(2);
        b.u16(0); // 2 bones

        // two bones
        for (int i = 0; i < 2; i++) {
            b.str(i == 0 ? "root" : "leaf");
            b.i32(0);
            b.u32(i == 0 ? 0xFFFFFFFFu : 0u);
            b.u32(64);
            b.identity_mat4();
            b.str("");
        }

        // MDLS v2 extras with all flags enabled AND non-zero filler so that a
        // mutated skip arithmetic leaves the cursor reading garbage.
        b.i16(0);
        b.u8(1);
        for (int i = 0; i < 2 * 16; i++) b.f32(99.0f); // 128 bytes filler
        b.u32(2);
        for (int i = 0; i < 2 * 3; i++) b.u32(0xDEADBEEFu);
        b.u32(0);
        b.u8(1);
        for (int i = 0; i < 2 * 19; i++) b.f32(42.0f);
        b.u8(1);
        for (int i = 0; i < 2; i++) b.u32(0xCAFEBABEu);

        // MDAT after extras — must be parsed correctly or test fails.
        b.str("MDAT0001");
        b.u32(0);
        b.u16(1);
        b.u16(0);
        b.str("bone_tip");
        b.translated_mat4(7.0f, 8.0f, 9.0f);
        b.str("MDLA0000");

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "extras.mdl", mdl));
        CHECK(mdl.puppet->bones.size() == 2);
        REQUIRE(mdl.puppet->attachments.size() == 1);
        CHECK(mdl.puppet->attachments[0].name == "bone_tip");
        CHECK(mdl.puppet->attachments[0].transform.translation().x() == doctest::Approx(7.0f));
    }

    TEST_CASE("puppet alt-format with vertices (vert stride = alt_singile_vertex=80)") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        // curr reads kAltHerald immediately → other alt-format branch
        b.u32(kAltHerald);
        // vertex_size = 80 (one alt vertex)
        b.u32(80);
        // alt vertex: position(3) + 7 extra u32 + blend_indices(4) + weight(4) + texcoord(2)
        b.f32(1);
        b.f32(2);
        b.f32(3);
        for (int i = 0; i < 7; i++) b.u32(0);
        for (int i = 0; i < 4; i++) b.u32(0);
        for (int i = 0; i < 4; i++) b.f32(1);
        b.f32(0.5f);
        b.f32(0.5f);
        b.u32(0); // indices_size = 0
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);
        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "alt_v.mdl", mdl));
        CHECK(mdl.vertexs.size() == 1);
        CHECK(mdl.vertexs[0].position[0] == doctest::Approx(1.f));
    }

    TEST_CASE("puppet v23 with bbox prefix between zero-pad and herald") {
        Bytes b;
        b.append_mdlv(23); // v23+
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        // v23 bbox (6 floats between the zero and the herald)
        for (int i = 0; i < 6; i++) b.f32((float)i);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "pv23.mdl", mdl));
        CHECK(mdl.is_puppet);
        CHECK(mdl.mdlv == 23);
    }

    // ---------------------------------------------------------------------------
    // Multi-iteration fixtures — each bone/attachment/event/animation carries
    // distinct data so that loop mutations (off-by-one, ++ ⇄ --, < ⇄ <=) which
    // change iteration count become observable rather than pure-byte-skip.
    // ---------------------------------------------------------------------------

    TEST_CASE("puppet with three bones stores distinct translations per bone") {
        // bones_num=3 with distinct translations, then assert on each bone's
        // transform individually.  Kills WPMdlParser bone-loop ++/< mutations.
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(3); // bones_num = 3
        b.u16(0);

        const float xs[3] = { 10.0f, 20.0f, 30.0f };
        for (int i = 0; i < 3; i++) {
            b.str(i == 0 ? "bone0" : i == 1 ? "bone1" : "bone2");
            b.i32(0);
            b.u32(i == 0 ? 0xFFFFFFFFu : (uint32_t)(i - 1));
            b.u32(64);
            b.translated_mat4(xs[i], (float)i, -(float)i);
            b.str("");
        }

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "3bones.mdl", mdl));
        REQUIRE(mdl.puppet->bones.size() == 3);
        for (int i = 0; i < 3; i++) {
            auto t = mdl.puppet->bones[i].transform.translation();
            CHECK(t.x() == doctest::Approx(xs[i]));
            CHECK(t.y() == doctest::Approx((float)i));
            CHECK(t.z() == doctest::Approx(-(float)i));
        }
        CHECK(mdl.puppet->bones[0].noParent());
        CHECK_FALSE(mdl.puppet->bones[1].noParent());
        CHECK(mdl.puppet->bones[1].parent == 0u);
        CHECK(mdl.puppet->bones[2].parent == 1u);
    }

    TEST_CASE("puppet with three MDAT attachments stores distinct data per entry") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        b.str("MDAT0001");
        b.u32(0);
        b.u16(3);

        const char* names[3] = { "hand_l", "hand_r", "head" };
        const float xs[3]    = { 1.0f, 2.0f, 3.0f };
        for (int i = 0; i < 3; i++) {
            b.u16(0);
            b.str(names[i]);
            b.translated_mat4(xs[i], (float)(10 * i), (float)(-i));
        }
        b.str("MDLA0000");

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "3attach.mdl", mdl));
        REQUIRE(mdl.puppet->attachments.size() == 3);
        for (int i = 0; i < 3; i++) {
            CHECK(mdl.puppet->attachments[i].name == names[i]);
            auto t = mdl.puppet->attachments[i].transform.translation();
            CHECK(t.x() == doctest::Approx(xs[i]));
            CHECK(t.y() == doctest::Approx((float)(10 * i)));
        }
    }

    TEST_CASE("MDLA with two animations preserves per-anim metadata") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        b.str("MDLA0001");
        b.u32(0);
        b.u32(2); // anim_num = 2

        const int   ids[2]     = { 7, 23 };
        const char* names[2]   = { "idle", "walk" };
        const float fps[2]     = { 30.0f, 60.0f };
        const int   lengths[2] = { 40, 80 };
        for (int a = 0; a < 2; a++) {
            b.u32((uint32_t)ids[a]);
            b.i32(0);
            b.str(names[a]);
            b.str("loop");
            b.f32(fps[a]);
            b.i32(lengths[a]);
            b.i32(0);
            b.u32(1); // b_num = 1 bframes
            b.i32(0);
            b.u32(36);
            for (int i = 0; i < 9; i++) b.f32(0);
            b.u32(0); // event_count = 0
        }

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "2anim.mdl", mdl));
        REQUIRE(mdl.puppet->anims.size() == 2);
        for (int a = 0; a < 2; a++) {
            CHECK(mdl.puppet->anims[a].id == ids[a]);
            CHECK(mdl.puppet->anims[a].name == names[a]);
            CHECK(mdl.puppet->anims[a].fps == doctest::Approx(fps[a]));
            CHECK(mdl.puppet->anims[a].length == lengths[a]);
        }
    }

    TEST_CASE("animation with three events stores all three distinctly") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        b.str("MDLA0001");
        b.u32(0);
        b.u32(1);
        b.u32(100);
        b.i32(0);
        b.str("walk");
        b.str("loop");
        b.f32(30);
        b.i32(60);
        b.i32(0);
        b.u32(1);
        b.i32(0);
        b.u32(36);
        for (int i = 0; i < 9; i++) b.f32(0);

        b.u32(3); // event_count = 3
        const char* ns[3]     = { "left_foot", "right_foot", "jump" };
        const int   frames[3] = { 5, 15, 30 };
        for (int i = 0; i < 3; i++) {
            b.f32(0.1f * i);
            std::string js = std::string(R"({"frame":)") + std::to_string(frames[i]) +
                             R"(,"name":")" + ns[i] + R"("})";
            b.str(js);
        }

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "3ev.mdl", mdl));
        REQUIRE(mdl.puppet->anims.size() == 1);
        const auto& ev = mdl.puppet->anims[0].events;
        REQUIRE(ev.size() == 3);
        for (int i = 0; i < 3; i++) {
            CHECK(ev[i].name == ns[i]);
            CHECK(ev[i].frame == frames[i]);
        }
    }

    TEST_CASE("animation with two bframes stores both") {
        Bytes b;
        b.append_mdlv(13);
        b.i32(0);
        b.i32(1);
        b.i32(1);
        b.str("");
        b.i32(0);
        b.u32(kStdHerald);
        b.u32(0);
        b.u32(0);
        appendMdls(b, 1);
        b.u32(0);
        b.u16(0);
        b.u16(0);

        b.str("MDLA0001");
        b.u32(0);
        b.u32(1);
        b.u32(42);
        b.i32(0);
        b.str("a");
        b.str("loop");
        b.f32(30);
        b.i32(10);
        b.i32(0);

        b.u32(2); // b_num = 2 bframes_arrays
        for (int k = 0; k < 2; k++) {
            b.i32(0);
            b.u32(72); // 2 frames = 18 floats = 72 bytes
            for (int fr = 0; fr < 2; fr++) {
                for (int i = 0; i < 3; i++) b.f32((float)(k * 10 + fr));
                for (int i = 0; i < 3; i++) b.f32((float)(k));
                for (int i = 0; i < 3; i++) b.f32(1.0f);
            }
        }
        b.u32(0); // no events

        fs::MemBinaryStream f(takeBuffer(std::move(b)));
        WPMdl               mdl;
        REQUIRE(WPMdlParser::ParseStream(f, "2bf.mdl", mdl));
        REQUIRE(mdl.puppet->anims.size() == 1);
        REQUIRE(mdl.puppet->anims[0].bframes_array.size() == 2);
        for (int k = 0; k < 2; k++) {
            REQUIRE(mdl.puppet->anims[0].bframes_array[k].frames.size() == 2);
            for (int fr = 0; fr < 2; fr++) {
                auto& frame = mdl.puppet->anims[0].bframes_array[k].frames[fr];
                CHECK(frame.position[0] == doctest::Approx((float)(k * 10 + fr)));
            }
        }
    }

    TEST_CASE("animation playmode mirror and single both decode") {
        auto makeWith = [](const char* mode) {
            Bytes b;
            b.append_mdlv(13);
            b.i32(0);
            b.i32(1);
            b.i32(1);
            b.str("");
            b.i32(0);
            b.u32(kStdHerald);
            b.u32(0);
            b.u32(0);
            appendMdls(b, 1);
            b.u32(0);
            b.u16(0);
            b.u16(0);
            b.str("MDLA0001");
            b.u32(0);
            b.u32(1); // end_size, anim_num
            b.u32(7); // id
            b.i32(0);
            b.str("anim");
            b.str(mode);
            b.f32(30);
            b.i32(1);
            b.i32(0);
            b.u32(1);
            b.i32(0);
            b.u32(36);
            for (int i = 0; i < 9; i++) b.f32(0);
            b.u32(0); // no events
            return takeBuffer(std::move(b));
        };

        {
            fs::MemBinaryStream f(makeWith("mirror"));
            WPMdl               mdl;
            REQUIRE(WPMdlParser::ParseStream(f, "m.mdl", mdl));
            REQUIRE(mdl.puppet->anims.size() == 1);
            CHECK(mdl.puppet->anims[0].mode == WPPuppet::PlayMode::Mirror);
        }
        {
            fs::MemBinaryStream f(makeWith("single"));
            WPMdl               mdl;
            REQUIRE(WPMdlParser::ParseStream(f, "s.mdl", mdl));
            CHECK(mdl.puppet->anims[0].mode == WPPuppet::PlayMode::Single);
        }
    }

} // Puppet

// ===========================================================================
// GenPuppetMesh / GenModelMesh / AddPuppet* — operate on a pre-built WPMdl
// ===========================================================================

TEST_SUITE("WPMdlParser.Gen") {
    static WPMdl makePuppetMdl(std::size_t nverts, std::size_t ntris, uint32_t bone_count) {
        WPMdl m;
        m.is_puppet = true;
        m.vertexs.resize(nverts);
        for (std::size_t i = 0; i < nverts; i++) {
            m.vertexs[i].position      = { (float)i, 0, 0 };
            m.vertexs[i].blend_indices = { 0, (uint32_t)(bone_count + 5), 99, 0 };
            m.vertexs[i].weight        = { 1, 0, 0, 0 };
            m.vertexs[i].texcoord      = { 0, 0 };
        }
        m.indices.resize(ntris);
        for (std::size_t i = 0; i < ntris; i++) m.indices[i] = { 0, 1, 2 };
        m.puppet = std::make_shared<WPPuppet>();
        m.puppet->bones.resize(bone_count);
        return m;
    }

    static WPMdl::Submesh makeSubmesh(std::size_t nverts, bool norm, bool tan, bool tc1) {
        WPMdl::Submesh s;
        s.has_normals   = norm;
        s.has_tangents  = tan;
        s.has_texcoord1 = tc1;
        s.vertexs.resize(nverts);
        for (std::size_t i = 0; i < nverts; i++) {
            s.vertexs[i].position  = { (float)i, 0, 0 };
            s.vertexs[i].normal    = { 0, 1, 0 };
            s.vertexs[i].tangent   = { 1, 0, 0, 1 };
            s.vertexs[i].texcoord  = { 0, 0 };
            s.vertexs[i].texcoord1 = { 0, 0 };
        }
        s.indices.push_back({ 0, 1, 2 });
        return s;
    }

    TEST_CASE("GenPuppetMesh packs position/blend/weight/texcoord at correct offsets") {
        // Exercises the stride arithmetic in WPMdlParser::to_one: pos at bytes
        // [0..12), blend_indices at [16..32), weight at [32..48), texcoord at
        // [48..56).  Mutations of the `offset++` or `4 *` in those memcpys would
        // shift the written data and this assertion would catch it.
        WPMdl mdl = makePuppetMdl(2, 1, 4);
        // Plant distinct, non-zero data on vertex[0] so misplacement is visible.
        mdl.vertexs[0].position      = { 1.25f, 2.5f, 3.75f };
        mdl.vertexs[0].blend_indices = { 1u, 2u, 3u, 0u }; // all valid
        mdl.vertexs[0].weight        = { 0.1f, 0.2f, 0.3f, 0.4f };
        mdl.vertexs[0].texcoord      = { 0.7f, 0.9f };

        SceneMesh mesh;
        WPMdlParser::GenPuppetMesh(mesh, mdl);
        REQUIRE(mesh.VertexCount() == 1);
        REQUIRE(mesh.IndexCount() == 1);

        const auto& arr  = mesh.GetVertexArray(0);
        const auto* data = arr.Data();
        REQUIRE(data != nullptr);
        // Stride = pos(FLOAT3 padded→4) + blend(UINT4→4) + weight(FLOAT4→4) +
        // tc(FLOAT2 padded→4) = 16 floats per vertex.
        CHECK(data[0] == doctest::Approx(1.25f));
        CHECK(data[1] == doctest::Approx(2.5f));
        CHECK(data[2] == doctest::Approx(3.75f));
        // blend_indices at floats [4..8); reinterpret as uint32.
        const uint32_t* bi = reinterpret_cast<const uint32_t*>(data + 4);
        CHECK(bi[0] == 1u);
        CHECK(bi[1] == 2u);
        CHECK(bi[2] == 3u);
        CHECK(bi[3] == 0u);
        CHECK(data[8] == doctest::Approx(0.1f)); // weight
        CHECK(data[9] == doctest::Approx(0.2f));
        CHECK(data[12] == doctest::Approx(0.7f)); // texcoord
        CHECK(data[13] == doctest::Approx(0.9f));
    }

    TEST_CASE("GenPuppetMesh clamps out-of-range blend_indices to 0 (GPU safety)") {
        WPMdl mdl                    = makePuppetMdl(1, 1, 3);
        mdl.vertexs[0].blend_indices = { 0u, 99u, 5u, 2u }; // slots 1,2 are OOB (bone_count=3)

        SceneMesh mesh;
        WPMdlParser::GenPuppetMesh(mesh, mdl);
        const auto*     data = mesh.GetVertexArray(0).Data();
        const uint32_t* bi   = reinterpret_cast<const uint32_t*>(data + 4);
        CHECK(bi[0] == 0u); // kept (< bone_count)
        CHECK(bi[1] == 0u); // clamped
        CHECK(bi[2] == 0u); // clamped
        CHECK(bi[3] == 2u); // kept
    }

    TEST_CASE("GenModelMesh flag-9 submesh (pos + texcoord only)") {
        auto sub                = makeSubmesh(2, false, false, false);
        sub.vertexs[0].position = { 1.f, 2.f, 3.f };
        sub.vertexs[0].texcoord = { 0.5f, 0.25f };
        SceneMesh mesh;
        WPMdlParser::GenModelMesh(mesh, sub);
        REQUIRE(mesh.VertexCount() == 1);
        REQUIRE(mesh.IndexCount() == 1);
        const auto* data = mesh.GetVertexArray(0).Data();
        // flag9 stride (unpadded): pos(3) + tc(2) = 5
        CHECK(data[0] == doctest::Approx(1.f));
        CHECK(data[1] == doctest::Approx(2.f));
        CHECK(data[2] == doctest::Approx(3.f));
        CHECK(data[3] == doctest::Approx(0.5f));
        CHECK(data[4] == doctest::Approx(0.25f));
    }

    TEST_CASE("GenModelMesh flag-11 submesh (normals)") {
        auto sub                = makeSubmesh(2, true, false, false);
        sub.vertexs[0].position = { 7, 8, 9 };
        sub.vertexs[0].normal   = { 0.1f, 0.2f, 0.3f };
        sub.vertexs[0].texcoord = { 0.6f, 0.7f };
        SceneMesh mesh;
        WPMdlParser::GenModelMesh(mesh, sub);
        const auto* data = mesh.GetVertexArray(0).Data();
        // flag11 stride: pos(3) + normal(3) + tc(2) = 8
        CHECK(data[0] == doctest::Approx(7.f));
        CHECK(data[3] == doctest::Approx(0.1f));
        CHECK(data[6] == doctest::Approx(0.6f));
    }

    TEST_CASE("GenModelMesh flag-15 submesh (normals + tangents)") {
        auto sub                = makeSubmesh(2, true, true, false);
        sub.vertexs[0].position = { 7, 0, 0 };
        sub.vertexs[0].normal   = { 0, 1, 0 };
        sub.vertexs[0].tangent  = { 1, 0, 0, 1 };
        sub.vertexs[0].texcoord = { 0.33f, 0.66f };
        SceneMesh mesh;
        WPMdlParser::GenModelMesh(mesh, sub);
        const auto* data = mesh.GetVertexArray(0).Data();
        // flag15: pos(3) + normal(3) + tangent(4) + tc(2) = 12
        CHECK(data[0] == doctest::Approx(7.f));
        CHECK(data[3] == doctest::Approx(0.f)); // normal.x
        CHECK(data[4] == doctest::Approx(1.f)); // normal.y
        CHECK(data[6] == doctest::Approx(1.f)); // tangent.x
        CHECK(data[9] == doctest::Approx(1.f)); // tangent.w
        CHECK(data[10] == doctest::Approx(0.33f));
        CHECK(data[11] == doctest::Approx(0.66f));
    }

    TEST_CASE("GenModelMesh flag-39 submesh (normals + tangents + tc1)") {
        auto sub                 = makeSubmesh(2, true, true, true);
        sub.vertexs[0].position  = { 0, 1, 2 };
        sub.vertexs[0].normal    = { 0, 1, 0 };
        sub.vertexs[0].tangent   = { 1, 0, 0, 1 };
        sub.vertexs[0].texcoord  = { 0.1f, 0.2f };
        sub.vertexs[0].texcoord1 = { 0.3f, 0.4f };
        SceneMesh mesh;
        WPMdlParser::GenModelMesh(mesh, sub);
        const auto* data = mesh.GetVertexArray(0).Data();
        // flag39 uses TEXCOORDVEC4 (padded=false FLOAT4) for BOTH uv sets.
        // Layout: pos(3) + normal(3) + tangent(4) + uv4(4) = 14
        CHECK(data[10] == doctest::Approx(0.1f));
        CHECK(data[11] == doctest::Approx(0.2f));
        CHECK(data[12] == doctest::Approx(0.3f));
        CHECK(data[13] == doctest::Approx(0.4f));
    }

    TEST_CASE("AddPuppetShaderInfo sets SKINNING=1 and BONECOUNT") {
        WPMdl        mdl = makePuppetMdl(0, 0, 7);
        WPShaderInfo info;
        WPMdlParser::AddPuppetShaderInfo(info, mdl);
        CHECK(info.combos["SKINNING"] == "1");
        CHECK(info.combos["BONECOUNT"] == "7");
    }

} // Gen
