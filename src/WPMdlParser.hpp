#pragma once
#include <string>
#include <cstdint>
#include <array>
#include <vector>
#include <memory>
#include <Eigen/Dense>

#include "WPPuppet.hpp"

namespace wallpaper
{

class WPShaderInfo;

namespace wpscene
{
class WPMaterial;
};
namespace fs
{
class VFS;
class IBinaryStream;
}; // namespace fs

struct WPMdl {
    i32 mdlv { 13 };
    i32 mdls { 1 };
    i32 mdla { 1 };

    struct Vertex {
        std::array<float, 3>    position;
        std::array<float, 3>    normal;
        std::array<float, 4>    tangent;
        std::array<uint32_t, 4> blend_indices;
        std::array<float, 4>    weight;
        std::array<float, 2>    texcoord;
        std::array<float, 2>    texcoord1;
    };

    // A single submesh within an MDL (each has its own material, vertices, indices)
    struct Submesh {
        std::string                          mat_json_file;
        std::vector<Vertex>                  vertexs;
        std::vector<std::array<uint16_t, 3>> indices;
        bool                                 has_normals { false };
        bool                                 has_tangents { false };
        bool                                 has_texcoord1 { false };
    };

    // Model path: multiple submeshes, each with own material
    std::vector<Submesh> submeshes;

    // Puppet path: single material + mesh (legacy flat fields)
    std::string                          mat_json_file;
    std::vector<Vertex>                  vertexs;
    std::vector<std::array<uint16_t, 3>> indices;

    bool is_puppet { false };

    // std::vector<Eigen::Matrix<float, 3, 4>> bones;
    std::shared_ptr<WPPuppet> puppet;
    // combo
    // SKINNING = 1
    // BONECOUNT

    // input
    // uvec4 a_BlendIndices
    // vec4 a_BlendWeights
    // uniform mat4x3 g_Bones[BONECOUNT]
};

class SceneMesh;

// Compute the u32 buffer slots needed to hold `triangle_count` triangles worth
// of packed u16 indices.  Each triangle is 3 u16 indices; two u16s fit in one
// u32.  The `+ 1` handles the odd u16 from odd triangle counts (plus keeps
// the buffer a full slot when triangle_count==0).  Pure arithmetic, exposed
// for unit testing so `*`/`/` mutants on the expression can be pinned down.
inline std::size_t U32SlotsForU16Triangles(std::size_t triangle_count) noexcept {
    std::size_t u16_count = triangle_count * 3;
    return u16_count / 2 + 1;
}

// Number of bytes memcpy'd when packing `triangle_count` triangles of u16
// indices into the u32 buffer produced by U32SlotsForU16Triangles().
inline std::size_t U16BytesForTriangles(std::size_t triangle_count) noexcept {
    return triangle_count * 3 * sizeof(uint16_t);
}

class WPMdlParser {
public:
    static bool Parse(std::string_view path, fs::VFS&, WPMdl&);
    // Parse from an already-loaded binary stream (exposed for tests).
    static bool ParseStream(fs::IBinaryStream& f, std::string_view path, WPMdl&);

    static void AddPuppetShaderInfo(WPShaderInfo& info, const WPMdl& mdl);
    static void AddPuppetMatInfo(wpscene::WPMaterial& mat, const WPMdl& mdl);

    static void GenPuppetMesh(SceneMesh& mesh, const WPMdl& mdl);
    static void GenModelMesh(SceneMesh& mesh, const WPMdl::Submesh& submesh);
};

} // namespace wallpaper
