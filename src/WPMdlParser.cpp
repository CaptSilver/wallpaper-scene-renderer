#include "WPMdlParser.hpp"
#include <nlohmann/json.hpp>
#include "Fs/VFS.h"
#include "Fs/IBinaryStream.h"
#include "Fs/MemBinaryStream.h"
#include "WPCommon.hpp"
#include "WPJson.hpp"
#include "Utils/Logging.h"
#include "Scene/SceneMesh.h"
#include "SpecTexs.hpp"
#include "wpscene/WPMaterial.h"
#include "WPShaderParser.hpp"

using namespace wallpaper;

namespace
{

WPPuppet::PlayMode ToPlayMode(std::string_view m) {
    if (m == "loop" || m.empty()) return WPPuppet::PlayMode::Loop;
    if (m == "mirror") return WPPuppet::PlayMode::Mirror;
    if (m == "single") return WPPuppet::PlayMode::Single;

    LOG_ERROR("unknown puppet animation play mode \"%s\"", m.data());
    assert(m == "loop");
    return WPPuppet::PlayMode::Loop;
}
} // namespace

// bytes * size
constexpr uint32_t singile_vertex                      = 4 * (3 + 4 + 4 + 2);
constexpr uint32_t singile_indices                     = 2 * 3;
constexpr uint32_t std_format_vertex_size_herald_value = 0x01800009;

// number of bytes in an MDAT attachment after the attachment name
constexpr uint32_t mdat_attachment_data_byte_length = 64;

// alternative consts for alternative mdl format
constexpr uint32_t alt_singile_vertex                  = 4 * (3 + 4 + 4 + 2 + 7);
constexpr uint32_t alt_format_vertex_size_herald_value = 0x0180000F;

// Non-puppet model vertex sizes (no blend indices/weights)
// Flag 9:  position(3) + texcoord(2) = 5 floats = 20 bytes
constexpr uint32_t model_vertex_flag9 = 4 * (3 + 2);
// Flag 11: position(3) + normal(3) + texcoord(2) = 8 floats = 32 bytes
constexpr uint32_t model_vertex_flag11 = 4 * (3 + 3 + 2);
// Flag 15: position(3) + normal(3) + tangent4(4) + texcoord(2) = 12 floats = 48 bytes
constexpr uint32_t model_vertex_flag15 = 4 * (3 + 3 + 4 + 2);
// Flag 39: position(3) + normal(3) + tangent4(4) + texcoord0(2) + texcoord1(2) = 14 floats = 56
// bytes
constexpr uint32_t model_vertex_flag39 = 4 * (3 + 3 + 4 + 2 + 2);

constexpr uint32_t singile_bone_frame = 4 * 9;

bool WPMdlParser::Parse(std::string_view path, fs::VFS& vfs, WPMdl& mdl) {
    auto str_path = std::string(path);
    auto pfile    = vfs.Open("/assets/" + str_path);
    if (! pfile) return false;
    auto memfile = fs::MemBinaryStream(*pfile);
    return ParseStream(memfile, path, mdl);
}

bool WPMdlParser::ParseStream(fs::IBinaryStream& f, std::string_view path, WPMdl& mdl) {
    mdl.mdlv = ReadMDLVesion(f);

    int32_t mdl_flag = f.ReadInt32();

    // Non-puppet model formats: simple mesh data, no skeleton
    // Flag is a bitmask: bit0=position, bit1=normal, bit2=tangent4, bit3=texcoord,
    // bit5=texcoord1 (lightmap UVs)
    if (mdl_flag == 9 || mdl_flag == 11 || mdl_flag == 15 || mdl_flag == 39) {
        f.ReadInt32(); // unk, always 1
        uint32_t submesh_count = f.ReadUint32();

        mdl.is_puppet = false;
        mdl.submeshes.resize(submesh_count);

        // MDLV0023+ adds per-submesh bounding box and flags before vertex data,
        // and 6 bytes padding after index data
        const bool v23 = (mdl.mdlv >= 23);

        for (uint32_t si = 0; si < submesh_count; si++) {
            auto& sub         = mdl.submeshes[si];
            sub.mat_json_file = f.ReadStr();
            f.ReadInt32(); // 0

            if (v23) {
                // bounding box: min(3 floats) + max(3 floats) + flags repeat(uint32)
                for (int i = 0; i < 6; i++) f.ReadFloat();
                f.ReadUint32(); // flags repeat (same as mdl_flag)
            }

            uint32_t vertex_size = f.ReadUint32();

            uint32_t vert_stride;
            if (mdl_flag == 39)
                vert_stride = model_vertex_flag39;
            else if (mdl_flag == 15)
                vert_stride = model_vertex_flag15;
            else if (mdl_flag == 11)
                vert_stride = model_vertex_flag11;
            else
                vert_stride = model_vertex_flag9;

            if (vertex_size % vert_stride != 0) {
                LOG_ERROR("unsupported model vertex size %d for flag=%d submesh=%d",
                          vertex_size,
                          mdl_flag,
                          si);
                return false;
            }

            uint32_t vertex_num = vertex_size / vert_stride;
            sub.vertexs.resize(vertex_num);

            if (mdl_flag == 39) {
                sub.has_normals   = true;
                sub.has_tangents  = true;
                sub.has_texcoord1 = true;
                for (auto& vert : sub.vertexs) {
                    for (auto& v : vert.position) v = f.ReadFloat();
                    for (auto& v : vert.normal) v = f.ReadFloat();
                    for (auto& v : vert.tangent) v = f.ReadFloat();
                    for (auto& v : vert.texcoord) v = f.ReadFloat();
                    for (auto& v : vert.texcoord1) v = f.ReadFloat();
                }
            } else if (mdl_flag == 15) {
                sub.has_normals  = true;
                sub.has_tangents = true;
                for (auto& vert : sub.vertexs) {
                    for (auto& v : vert.position) v = f.ReadFloat();
                    for (auto& v : vert.normal) v = f.ReadFloat();
                    for (auto& v : vert.tangent) v = f.ReadFloat();
                    for (auto& v : vert.texcoord) v = f.ReadFloat();
                }
            } else if (mdl_flag == 11) {
                sub.has_normals = true;
                for (auto& vert : sub.vertexs) {
                    for (auto& v : vert.position) v = f.ReadFloat();
                    for (auto& v : vert.normal) v = f.ReadFloat();
                    for (auto& v : vert.texcoord) v = f.ReadFloat();
                }
            } else {
                for (auto& vert : sub.vertexs) {
                    for (auto& v : vert.position) v = f.ReadFloat();
                    for (auto& v : vert.texcoord) v = f.ReadFloat();
                }
            }

            uint32_t indices_size = f.ReadUint32();
            if (indices_size % singile_indices != 0) {
                LOG_ERROR("unsupported model indices size %d submesh=%d", indices_size, si);
                return false;
            }

            uint32_t indices_num = indices_size / singile_indices;
            sub.indices.resize(indices_num);
            for (auto& id : sub.indices) {
                for (auto& v : id) v = f.ReadUint16();
            }

            if (v23) {
                // 6 bytes trailing padding per submesh
                for (int i = 0; i < 6; i++) f.ReadUint8();
            }

            LOG_INFO("read model: mdlv: %d, flag: %d, submesh: %d/%d, verts: %d, tris: %d, "
                     "normals: %d, tangents: %d, mat: %s",
                     mdl.mdlv,
                     mdl_flag,
                     si,
                     submesh_count,
                     vertex_num,
                     indices_num,
                     (int)sub.has_normals,
                     (int)sub.has_tangents,
                     sub.mat_json_file.c_str());
        }
        return true;
    }

    // Puppet format (flag=11, 15+herald, etc.)
    mdl.is_puppet = true;
    f.ReadInt32(); // unk, 1
    f.ReadInt32(); // unk, 1

    mdl.mat_json_file = f.ReadStr();
    // 0
    f.ReadInt32();

    // MDLV0023+ inserts bbox (6 floats) between the zero and the herald/vertex_size
    if (mdl.mdlv >= 23) {
        for (int i = 0; i < 6; i++) f.ReadFloat(); // bbox_min + bbox_max
    }

    bool     alt_mdl_format = false;
    uint32_t curr           = f.ReadUint32();

    // if the uint at the normal vertex size position is 0, then this file
    // uses the alternative MDL format, therefore the actual vertex size is
    // located after the herald value, and we'll need to account for other differences later on.
    if (curr == 0) {
        alt_mdl_format = true;
        while (curr != alt_format_vertex_size_herald_value) {
            if (f.Tell() >= f.Size()) {
                LOG_ERROR("mdl: EOF scanning for alt-format herald in '%s'", path.data());
                return false;
            }
            curr = f.ReadUint32();
        }
        curr = f.ReadUint32();
    } else if (curr == std_format_vertex_size_herald_value) {
        curr = f.ReadUint32();
    } else if (curr == alt_format_vertex_size_herald_value) {
        alt_mdl_format = true;
        curr           = f.ReadUint32();
    }

    uint32_t vertex_size = curr;
    if (vertex_size % (alt_mdl_format ? alt_singile_vertex : singile_vertex) != 0) {
        LOG_ERROR("unsupport mdl vertex size %d", vertex_size);
        return false;
    }

    // if using the alternative MDL format, vertexes contain 7 extra 32-bit chunks between
    // position and blend indices
    uint32_t vertex_num = vertex_size / (alt_mdl_format ? alt_singile_vertex : singile_vertex);
    mdl.vertexs.resize(vertex_num);
    for (auto& vert : mdl.vertexs) {
        for (auto& v : vert.position) v = f.ReadFloat();
        if (alt_mdl_format) {
            for (int i = 0; i < 7; i++) f.ReadUint32();
        }
        for (auto& v : vert.blend_indices) v = f.ReadUint32();
        for (auto& v : vert.weight) v = f.ReadFloat();
        for (auto& v : vert.texcoord) v = f.ReadFloat();
    }
    // Log mesh bounds + texcoord bounds to verify MDL vertex & UV ranges.
    if (vertex_num > 0) {
        float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
        float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
        for (const auto& vert : mdl.vertexs) {
            minX = std::min(minX, vert.position[0]);
            maxX = std::max(maxX, vert.position[0]);
            minY = std::min(minY, vert.position[1]);
            maxY = std::max(maxY, vert.position[1]);
            minU = std::min(minU, vert.texcoord[0]);
            maxU = std::max(maxU, vert.texcoord[0]);
            minV = std::min(minV, vert.texcoord[1]);
            maxV = std::max(maxV, vert.texcoord[1]);
        }
        LOG_INFO("  mdl '%.*s' vert bounds: X=[%.1f, %.1f] (w=%.1f) Y=[%.1f, %.1f] (h=%.1f) "
                 "UV=[%.3f-%.3f, %.3f-%.3f] n=%u",
                 (int)path.size(), path.data(),
                 minX, maxX, maxX - minX,
                 minY, maxY, maxY - minY,
                 minU, maxU, minV, maxV,
                 (unsigned)vertex_num);
    }

    uint32_t indices_size = f.ReadUint32();
    if (indices_size % singile_indices != 0) {
        LOG_ERROR("unsupport mdl indices size %d", indices_size);
        return false;
    }

    uint32_t indices_num = indices_size / singile_indices;
    mdl.indices.resize(indices_num);
    for (auto& id : mdl.indices) {
        for (auto& v : id) v = f.ReadUint16();
    }

    // Newer MDL formats (MDLV >= 23 / PKGV0022+) insert extra sections between
    // the mesh data and the skeleton.  Scan forward byte-by-byte for the "MDLS"
    // tag rather than assuming it immediately follows the index data.
    {
        char ring[4] = { 0, 0, 0, 0 };
        char c;
        while (f.Read(&c, 1) == 1) {
            ring[0] = ring[1];
            ring[1] = ring[2];
            ring[2] = ring[3];
            ring[3] = c;
            if (ring[0] == 'M' && ring[1] == 'D' && ring[2] == 'L' && ring[3] == 'S') {
                // Read the remaining 5 bytes of the 9-byte version tag (4 digits + NUL).
                char ver_tail[6] = {};
                f.Read(ver_tail, 5);
                int v = 0;
                std::from_chars(ver_tail, ver_tail + 4, v);
                mdl.mdls = v;
                break;
            }
        }
    }

    size_t bones_file_end = f.ReadUint32();
    (void)bones_file_end;

    uint16_t bones_num = f.ReadUint16();
    // 1 byte
    f.ReadUint16(); // unk

    mdl.puppet  = std::make_shared<WPPuppet>();
    auto& bones = mdl.puppet->bones;
    auto& anims = mdl.puppet->anims;

    bones.resize(bones_num);
    LOG_INFO("  mdl '%.*s': reading %u bones", (int)path.size(), path.data(), (unsigned)bones_num);
    for (uint i = 0; i < bones_num; i++) {
        auto&       bone = bones[i];
        std::string name = f.ReadStr();
        f.ReadInt32(); // unk

        bone.parent = f.ReadUint32();
        if (bone.parent >= i && ! bone.noParent()) {
            LOG_ERROR("mdl wrong bone parent index %d, treating as root bone", bone.parent);
            bone.parent = 0xFFFFFFFFu;
        }

        uint32_t size = f.ReadUint32();
        if (size != 64) {
            LOG_ERROR("mdl unsupport bones size: %d", size);
            return false;
        }
        for (auto row : bone.transform.matrix().colwise()) {
            for (auto& x : row) x = f.ReadFloat();
        }

        std::string bone_simulation_json = f.ReadStr();
        {
            auto trans = bone.transform.translation();
            LOG_INFO("    bone[%u] '%s' parent=%d trans=(%.1f, %.1f, %.1f)",
                     i, name.c_str(), (int)bone.parent, trans[0], trans[1], trans[2]);
        }
    }

    if (mdl.mdls > 1) {
        int16_t unk = f.ReadInt16();
        if (unk != 0) {
            LOG_INFO("puppet: one unk is not 0, may be wrong");
        }

        uint8_t has_trans = f.ReadUint8();
        if (has_trans) {
            for (uint i = 0; i < bones_num; i++)
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
        }
        uint32_t size_unk = f.ReadUint32();
        for (uint i = 0; i < size_unk; i++)
            for (int j = 0; j < 3; j++) f.ReadUint32();

        f.ReadUint32(); // unk

        uint8_t has_offset_trans = f.ReadUint8();
        if (has_offset_trans) {
            for (uint i = 0; i < bones_num; i++) {
                for (uint j = 0; j < 3; j++) f.ReadFloat();  // like pos
                for (uint j = 0; j < 16; j++) f.ReadFloat(); // mat
            }
        }

        uint8_t has_index = f.ReadUint8();
        if (has_index) {
            for (uint i = 0; i < bones_num; i++) {
                f.ReadUint32();
            }
        }
    }

    // sometimes there can be one or more zero bytes and/or MDAT sections containing
    // attachments before the MDLA section, so we need to skip them
    std::string mdType = "";
    std::string mdVersion;

    bool reached_eof_without_mdla = false;
    do {
        if (f.Tell() >= f.Size()) {
            // No animation section — valid for static deform puppets (hair
            // strands, body parts that only receive bone skinning).
            reached_eof_without_mdla = true;
            break;
        }
        auto        pos_before = f.Tell();
        std::string mdPrefix   = f.ReadStr();

        // sometimes there can be other garbage in this gap, so we need to
        // skip over that as well
        if (mdPrefix.length() == 8) {
            mdType    = mdPrefix.substr(0, 4);
            mdVersion = mdPrefix.substr(4, 4);

            if (mdType == "MDAT") {
                f.ReadUint32(); // skip 4 bytes
                uint32_t num_attachments =
                    f.ReadUint16(); // number of attachments in the MDAT section

                for (int i = 0; i < num_attachments; i++) {
                    f.ReadUint16(); // skip 2 bytes
                    WPPuppet::Attachment att;
                    att.name = f.ReadStr();
                    // 64 bytes = 4x4 float matrix (column-major) describing
                    // the attachment point's transform in puppet local space.
                    // Children that rig to this attachment (via scene.json's
                    // "attachment" field) land at this transform.
                    for (auto row : att.transform.matrix().colwise()) {
                        for (auto& x : row) x = f.ReadFloat();
                    }
                    {
                        auto m = att.transform.matrix();
                        LOG_INFO("    MDAT attach '%s' trans=(%.2f, %.2f, %.2f) "
                                 "row0=[%.3f %.3f %.3f %.3f] row1=[%.3f %.3f %.3f %.3f]",
                                 att.name.c_str(),
                                 att.transform.translation().x(),
                                 att.transform.translation().y(),
                                 att.transform.translation().z(),
                                 m(0, 0), m(0, 1), m(0, 2), m(0, 3),
                                 m(1, 0), m(1, 1), m(1, 2), m(1, 3));
                    }
                    mdl.puppet->attachments.push_back(std::move(att));
                }
            }
        }

        // Bail if the stream didn't advance — a zero-length read at EOF would loop forever.
        if (f.Tell() == pos_before) {
            LOG_ERROR("mdl: stalled scanning for MDLA section in '%s'", path.data());
            return false;
        }
    } while (mdType != "MDLA");

    if (reached_eof_without_mdla) {
        LOG_INFO("mdl: no animation section in '%s' (static skinned puppet)",
                 path.data());
    } else if (mdType == "MDLA" && mdVersion.length() > 0) {
        mdl.mdla = std::stoi(mdVersion);
        if (mdl.mdla != 0) {
            uint end_size = f.ReadUint32();
            (void)end_size;

            uint anim_num = f.ReadUint32();
            anims.resize(anim_num);
            int anim_idx = 0;
            for (auto& anim : anims) {
                // Skip zero padding bytes between animations.  Older formats
                // used 4-byte-aligned padding, but newer MDLA versions (v6+)
                // can have an odd number of padding bytes.  Scan byte-by-byte
                // to find the first non-zero byte, then seek back and read
                // the full 32-bit animation ID.
                {
                    uint8_t b = 0;
                    while (b == 0) {
                        if (f.Tell() >= f.Size()) {
                            LOG_ERROR("mdl: EOF scanning animation padding in '%s'",
                                      path.data());
                            return false;
                        }
                        b = f.ReadUint8();
                    }
                    f.SeekCur(-1);
                    anim.id = f.ReadInt32();
                }

                if (anim.id <= 0) {
                    LOG_ERROR("wrong anime id %d", anim.id);
                    return false;
                }
                f.ReadInt32();
                anim.name = f.ReadStr();
                if (anim.name.empty()) {
                    anim.name = f.ReadStr();
                }
                anim.mode   = ToPlayMode(f.ReadStr());
                anim.fps    = f.ReadFloat();
                anim.length = f.ReadInt32();
                LOG_INFO("  anim[%d]: id=%d name='%s' length=%d",
                         anim_idx,
                         anim.id,
                         anim.name.c_str(),
                         anim.length);
                anim_idx++;
                f.ReadInt32();

                uint32_t b_num = f.ReadUint32();
                anim.bframes_array.resize(b_num);
                for (auto& bframes : anim.bframes_array) {
                    f.ReadInt32();
                    uint32_t byte_size = f.ReadUint32();
                    uint32_t num       = byte_size / singile_bone_frame;
                    if (byte_size % singile_bone_frame != 0) {
                        LOG_ERROR("wrong bone frame size %d", byte_size);
                        return false;
                    }
                    bframes.frames.resize(num);
                    for (auto& frame : bframes.frames) {
                        for (auto& v : frame.position) v = f.ReadFloat();
                        for (auto& v : frame.angle) v = f.ReadFloat();
                        for (auto& v : frame.scale) v = f.ReadFloat();
                    }
                }

                // in the alternative MDL format there are 2 empty bytes followed
                // by a variable number of 32-bit 0s between animations. We'll read
                // the two bytes now so that the cursor is aligned to read through the
                // 32-bit 0s in the next iteration
                if (alt_mdl_format) {
                    f.ReadUint8();
                    f.ReadUint8();
                } else if (mdl.mdla == 3) {
                    // In MDLA version 3 there is an extra 8-bit zero between animations.
                    // This will cause the parser to be misaligned moving forward if we don't handle
                    // it here.
                    f.ReadUint8();
                } else {
                    // The trailer after each animation's bone frames is a list of
                    // keyframe event records.  Each record is:
                    //   float  — animation timestamp of the event (redundant w/ frame)
                    //   string — small JSON blob: {"frame":N,"name":"eventName",...}
                    // SceneScripts react to these via animationEvent(event,value).
                    uint32_t event_count = f.ReadUint32();
                    for (uint i = 0; i < event_count; i++) {
                        f.ReadFloat();
                        std::string evt_json = f.ReadStr();
                        if (evt_json.empty()) continue;
                        nlohmann::json j;
                        if (! PARSE_JSON(evt_json, j)) {
                            LOG_ERROR("anim %d: failed to parse event json: %s",
                                      anim.id, evt_json.c_str());
                            continue;
                        }
                        WPPuppet::Animation::Event e {};
                        GET_JSON_NAME_VALUE_NOWARN(j, "frame", e.frame);
                        GET_JSON_NAME_VALUE_NOWARN(j, "name", e.name);
                        if (e.name.empty()) continue;
                        LOG_INFO("    anim[%d] event frame=%d name='%s'",
                                 anim.id, e.frame, e.name.c_str());
                        anim.events.push_back(std::move(e));
                    }
                }
            }
        }
    }

    mdl.puppet->prepared();

    LOG_INFO("read puppet: mdlv: %d, nmdls: %d, mdla: %d, bones: %d, anims: %d",
             mdl.mdlv,
             mdl.mdls,
             mdl.mdla,
             mdl.puppet->bones.size(),
             mdl.puppet->anims.size());
    return true;
}

void WPMdlParser::GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl) {
    SceneVertexArray vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3 },
                              { WE_IN_BLENDINDICES.data(), VertexType::UINT4 },
                              { WE_IN_BLENDWEIGHTS.data(), VertexType::FLOAT4 },
                              { WE_IN_TEXCOORD.data(), VertexType::FLOAT2 } },
                            mdl.vertexs.size());

    // Clamp blend indices to [0, bone_count-1] to prevent OOB GPU array access in
    // g_Bones[a_BlendIndices.x] which causes VK_ERROR_DEVICE_LOST on strict GPU drivers.
    // Unused slots in the MDL may carry sentinel values (e.g. 0xFF) with weight=0.
    uint32_t bone_count = mdl.puppet ? (uint32_t)mdl.puppet->bones.size() : 1u;

    std::array<float, 16> one_vert;
    auto                  to_one = [bone_count](const WPMdl::Vertex& in, decltype(one_vert)& out) {
        uint offset = 0;
        memcpy(out.data() + 4 * (offset++), in.position.data(), sizeof(in.position));
        std::array<uint32_t, 4> safe_indices;
        for (int j = 0; j < 4; j++)
            safe_indices[j] = in.blend_indices[j] < bone_count ? in.blend_indices[j] : 0u;
        memcpy(out.data() + 4 * (offset++), safe_indices.data(), sizeof(safe_indices));
        memcpy(out.data() + 4 * (offset++), in.weight.data(), sizeof(in.weight));
        memcpy(out.data() + 4 * (offset++), in.texcoord.data(), sizeof(in.texcoord));
    };
    for (uint i = 0; i < mdl.vertexs.size(); i++) {
        auto& v = mdl.vertexs[i];
        to_one(v, one_vert);
        vertex.SetVertexs(i, one_vert);
    }
    std::vector<uint32_t> indices;
    size_t                u16_count = mdl.indices.size() * 3;
    indices.resize(u16_count / 2 + 1);
    memcpy(indices.data(), mdl.indices.data(), u16_count * sizeof(uint16_t));

    mesh.AddVertexArray(std::move(vertex));
    mesh.AddIndexArray(SceneIndexArray(indices));
}

void WPMdlParser::GenModelMesh(SceneMesh& mesh, const WPMdl::Submesh& sub) {
    if (sub.has_normals && sub.has_tangents && sub.has_texcoord1) {
        // Flag 39: pos + normal + tangent + texcoord0 + texcoord1 (lightmap)
        // Pack both texcoord sets into a_TexCoordVec4 (vec4) for lightmap shaders
        SceneVertexArray      vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3, false },
                                       { WE_IN_NORMAL.data(), VertexType::FLOAT3, false },
                                       { WE_IN_TANGENT4.data(), VertexType::FLOAT4, false },
                                       { WE_IN_TEXCOORDVEC4.data(), VertexType::FLOAT4, false } },
                                sub.vertexs.size());
        std::array<float, 14> one_vert;
        for (uint i = 0; i < sub.vertexs.size(); i++) {
            auto& v      = sub.vertexs[i];
            uint  offset = 0;
            memcpy(one_vert.data() + offset, v.position.data(), sizeof(v.position));
            offset += 3;
            memcpy(one_vert.data() + offset, v.normal.data(), sizeof(v.normal));
            offset += 3;
            memcpy(one_vert.data() + offset, v.tangent.data(), sizeof(v.tangent));
            offset += 4;
            memcpy(one_vert.data() + offset, v.texcoord.data(), sizeof(v.texcoord));
            offset += 2;
            memcpy(one_vert.data() + offset, v.texcoord1.data(), sizeof(v.texcoord1));
            vertex.SetVertexs(i, one_vert);
        }
        mesh.AddVertexArray(std::move(vertex));
    } else if (sub.has_normals && sub.has_tangents) {
        // padding=false: vertex data is tightly packed (no padding to vec4),
        // must match the layout used by SetVertexs() below
        SceneVertexArray      vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3, false },
                                       { WE_IN_NORMAL.data(), VertexType::FLOAT3, false },
                                       { WE_IN_TANGENT4.data(), VertexType::FLOAT4, false },
                                       { WE_IN_TEXCOORD.data(), VertexType::FLOAT2, false } },
                                sub.vertexs.size());
        std::array<float, 12> one_vert;
        for (uint i = 0; i < sub.vertexs.size(); i++) {
            auto& v      = sub.vertexs[i];
            uint  offset = 0;
            memcpy(one_vert.data() + offset, v.position.data(), sizeof(v.position));
            offset += 3;
            memcpy(one_vert.data() + offset, v.normal.data(), sizeof(v.normal));
            offset += 3;
            memcpy(one_vert.data() + offset, v.tangent.data(), sizeof(v.tangent));
            offset += 4;
            memcpy(one_vert.data() + offset, v.texcoord.data(), sizeof(v.texcoord));
            vertex.SetVertexs(i, one_vert);
        }
        mesh.AddVertexArray(std::move(vertex));
    } else if (sub.has_normals) {
        SceneVertexArray     vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3, false },
                                      { WE_IN_NORMAL.data(), VertexType::FLOAT3, false },
                                      { WE_IN_TEXCOORD.data(), VertexType::FLOAT2, false } },
                                sub.vertexs.size());
        std::array<float, 8> one_vert;
        for (uint i = 0; i < sub.vertexs.size(); i++) {
            auto& v      = sub.vertexs[i];
            uint  offset = 0;
            memcpy(one_vert.data() + offset, v.position.data(), sizeof(v.position));
            offset += 3;
            memcpy(one_vert.data() + offset, v.normal.data(), sizeof(v.normal));
            offset += 3;
            memcpy(one_vert.data() + offset, v.texcoord.data(), sizeof(v.texcoord));
            vertex.SetVertexs(i, one_vert);
            // Dump first 3 vertices for debugging
            if (i < 3) {
                LOG_INFO("  vert[%u] pos=(%.4f,%.4f,%.4f) norm=(%.4f,%.4f,%.4f) tc=(%.4f,%.4f)",
                         i,
                         v.position[0],
                         v.position[1],
                         v.position[2],
                         v.normal[0],
                         v.normal[1],
                         v.normal[2],
                         v.texcoord[0],
                         v.texcoord[1]);
            }
        }
        // Also dump first 3 index triples
        for (uint i = 0; i < std::min((uint)sub.indices.size(), 3u); i++) {
            LOG_INFO("  tri[%u] indices=(%u,%u,%u)",
                     i,
                     sub.indices[i][0],
                     sub.indices[i][1],
                     sub.indices[i][2]);
        }
        mesh.AddVertexArray(std::move(vertex));
    } else {
        // padding=false: tightly packed, matches SetVertexs() layout
        SceneVertexArray     vertex({ { WE_IN_POSITION.data(), VertexType::FLOAT3, false },
                                      { WE_IN_TEXCOORD.data(), VertexType::FLOAT2, false } },
                                sub.vertexs.size());
        std::array<float, 5> one_vert;
        for (uint i = 0; i < sub.vertexs.size(); i++) {
            auto& v      = sub.vertexs[i];
            uint  offset = 0;
            memcpy(one_vert.data() + offset, v.position.data(), sizeof(v.position));
            offset += 3;
            memcpy(one_vert.data() + offset, v.texcoord.data(), sizeof(v.texcoord));
            vertex.SetVertexs(i, one_vert);
        }
        mesh.AddVertexArray(std::move(vertex));
    }

    std::vector<uint32_t> indices;
    size_t                u16_count = sub.indices.size() * 3;
    indices.resize(u16_count / 2 + 1);
    memcpy(indices.data(), sub.indices.data(), u16_count * sizeof(uint16_t));

    mesh.AddIndexArray(SceneIndexArray(indices));
}

void WPMdlParser::AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl) {
    info.combos["SKINNING"]  = "1";
    info.combos["BONECOUNT"] = std::to_string(mdl.puppet->bones.size());
}

void WPMdlParser::AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl) {
    mat.combos["SKINNING"]  = 1;
    mat.combos["BONECOUNT"] = (i32)mdl.puppet->bones.size();
    mat.use_puppet          = true;
}
