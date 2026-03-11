#include "WPParticleRawGener.h"

#include <cstring>
#include <Eigen/Dense>
#include <array>

#include "Core/Literals.hpp"
#include "SpecTexs.hpp"
#include "ParticleModify.h"
#include "ParticleSystem.h"

#include "Utils/Logging.h"

using namespace wallpaper;
using namespace Eigen;

struct WPGOption {
    bool thick_format { false };
    bool geometry_shader { false };
};

namespace
{
inline void AssignVertexTimes(std::span<float> dst, std::span<const float> src, uint num) noexcept {
    const uint dst_one_size = dst.size() / num;
    for (uint i = 0; i < num; i++) {
        std::copy(src.begin(), src.end(), dst.begin() + i * dst_one_size);
    }
}

inline void AssignVertex(std::span<float> dst, std::span<const float> src, uint num) noexcept {
    const uint dst_one_size = dst.size() / num;
    const uint src_one_size = src.size() / num;
    for (uint i = 0; i < num; i++) {
        std::copy_n(src.begin() + i * src_one_size, src_one_size, dst.begin() + i * dst_one_size);
    }
}

inline usize GenParticleData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                             const ParticleRawGenSpecOp& specOp, WPGOption opt,
                             SceneVertexArray& sv) noexcept {
    std::array<float, 32 * 4> storage;

    float* data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = 4 * one_size;
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) {
                continue;
            }

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;

            usize offset = 0;

            // pos
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { pos[0], pos[1], pos[2] }, 4);
            offset += 4;
            // TexCoordVec4
            float      rz = p.rotation[2];
            std::array t { 0.0f, 1.0f, rz, size, 1.0f, 1.0f, rz, size,
                           1.0f, 0.0f, rz, size, 0.0f, 0.0f, rz, size };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;

            // color
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha },
                              4);
            offset += 4;

            if (opt.thick_format) {
                AssignVertexTimes(
                    { data + offset, totle_size },
                    std::array { p.velocity[0], p.velocity[1], p.velocity[2], lifetime },
                    4);
                offset += 4;
            }
            // TexCoordC2
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { p.rotation[0], p.rotation[1] }, 4);

            sv.SetVertexs((i++) * 4, { data, totle_size });
        }
    }
    return i;
}

// GS path: 1 vertex per particle (geometry shader expands to quad)
inline usize GenParticleDataGS(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                SceneVertexArray& sv) noexcept {
    std::array<float, 32> storage {};
    float* data = storage.data();

    const auto one_size = sv.OneSize();
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        for (const auto& p : inst->Particles()) {
            if (! ParticleModify::LifetimeOk(p)) continue;

            float lifetime = p.lifetime;
            specOp(p, { &lifetime });

            auto  pos  = inst->GetBoundedData().pos + p.position;
            float size = p.size / 2.0f;
            float rz   = p.rotation[2];

            usize offset = 0;

            // a_Position (vec3, padded to 4 floats in vertex layout)
            std::copy_n(std::array { pos[0], pos[1], pos[2], 0.0f }.data(), 4, data + offset);
            offset += 4;
            // a_TexCoordVec4: UV (unused by GS), rotation.z, size
            std::copy_n(std::array { 0.0f, 0.0f, rz, size }.data(), 4, data + offset);
            offset += 4;
            // a_Color
            std::copy_n(std::array { p.color[0], p.color[1], p.color[2], p.alpha }.data(), 4,
                        data + offset);
            offset += 4;

            if (opt.thick_format) {
                // a_TexCoordVec4C1: velocity + lifetime
                std::copy_n(
                    std::array { p.velocity[0], p.velocity[1], p.velocity[2], lifetime }.data(),
                    4, data + offset);
                offset += 4;
            }
            // a_TexCoordC2: rotation.x, rotation.y (padded to 4 floats)
            std::copy_n(std::array { p.rotation[0], p.rotation[1], 0.0f, 0.0f }.data(), 4,
                        data + offset);

            sv.SetVertexs(i, { data, one_size });
            i++;
        }
    }
    return i;
}

inline size_t GenRopeParticleData(std::span<const Particle> particles,
                                  const Vector3f& inst_pos, const ParticleRawGenSpecOp& specOp,
                                  WPGOption opt, SceneVertexArray& sv, size_t start_idx) {
    std::array<float, 32 * 4> storage;
    float*                    data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = one_size * 4;
    size_t     seg_count  = 0;

    for (size_t pi = 1; pi < particles.size(); pi++) {
        const auto& p = particles[pi];
        if (! ParticleModify::LifetimeOk(p)) break;

        const auto& pre_p = particles[pi - 1];
        float       size  = p.size / 2.0f;
        size_t      offset = 0;

        float lifetime = p.lifetime;
        specOp(p, { &lifetime });
        float trail_length   = (float)particles.size();
        float trail_position = (float)(pi - 1);

        // Add instance position offset
        Vector3f sp_pos  = Vector3f { pre_p.position } + inst_pos;
        Vector3f ep_pos  = Vector3f { p.position } + inst_pos;
        Vector3f pos_vec = ep_pos - sp_pos;

        Vector3f cp_vec = AngleAxisf(p.rotation[2] + (float)M_PI / 2.0f, Vector3f::UnitZ()) *
                          Vector3f { 0.0f, size / 2.0f, 0.0f };
        cp_vec = pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

        Vector3f scp = sp_pos + cp_vec;
        Vector3f ecp = ep_pos - cp_vec;

        // a_PositionVec4: start pos + size
        AssignVertexTimes(
            { data + offset, totle_size },
            std::array { sp_pos[0], sp_pos[1], sp_pos[2], size }, 4);
        offset += 4;
        // a_TexCoordVec4: end pos + trail_length
        AssignVertexTimes(
            { data + offset, totle_size },
            std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length }, 4);
        offset += 4;
        // a_TexCoordVec4C1: cp start + trail_position
        AssignVertexTimes(
            { data + offset, totle_size },
            std::array { scp[0], scp[1], scp[2], trail_position }, 4);
        offset += 4;

        if (opt.thick_format) {
            // a_TexCoordVec4C2: cp end pos, size_end
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { ecp[0], ecp[1], ecp[2], size }, 4);
            offset += 4;
            // a_TexCoordVec4C3: color_end
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha }, 4);
            offset += 4;
            // a_TexCoordC4
            std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;
        } else {
            // a_TexCoordVec3C2: cp end pos
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { ecp[0], ecp[1], ecp[2] }, 4);
            offset += 4;
            // a_TexCoordC3
            std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            AssignVertex({ data + offset, totle_size }, t, 4);
            offset += 4;
        }

        // a_Color
        AssignVertexTimes({ data + offset, totle_size },
                          std::array { p.color[0], p.color[1], p.color[2], p.alpha }, 4);

        sv.SetVertexs((start_idx + seg_count) * 4, { data, totle_size });
        seg_count++;
    }
    return seg_count;
}

// GS rope: 1 vertex per segment (geometry shader expands to triangle strip)
inline size_t GenRopeParticleDataGS(std::span<const Particle> particles, const Vector3f& inst_pos,
                                     const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                     SceneVertexArray& sv, size_t start_idx) {
    // GS vertex: same data as non-GS but 1 vertex per segment, no UV attribute
    const auto one_size = sv.OneSize();
    size_t     seg_count = 0;

    std::array<float, 32> storage;
    float*                data = storage.data();

    for (size_t pi = 1; pi < particles.size(); pi++) {
        const auto& p = particles[pi];
        if (! ParticleModify::LifetimeOk(p)) break;

        const auto& pre_p = particles[pi - 1];
        float       size  = p.size / 2.0f;

        float lifetime = p.lifetime;
        specOp(p, { &lifetime });
        float trail_length   = (float)particles.size();
        float trail_position = (float)(pi - 1);

        Vector3f sp_pos  = Vector3f { pre_p.position } + inst_pos;
        Vector3f ep_pos  = Vector3f { p.position } + inst_pos;
        Vector3f pos_vec = ep_pos - sp_pos;

        Vector3f cp_vec = AngleAxisf(p.rotation[2] + (float)M_PI / 2.0f, Vector3f::UnitZ()) *
                          Vector3f { 0.0f, size / 2.0f, 0.0f };
        cp_vec = pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

        Vector3f scp = sp_pos + cp_vec;
        Vector3f ecp = ep_pos - cp_vec;

        size_t offset = 0;

        // a_PositionVec4: start pos + size
        std::copy_n(std::array { sp_pos[0], sp_pos[1], sp_pos[2], size }.data(), 4,
                    data + offset);
        offset += 4;
        // a_TexCoordVec4: end pos + trail_length
        std::copy_n(std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length }.data(), 4,
                    data + offset);
        offset += 4;
        // a_TexCoordVec4C1: cp start + trail_position
        std::copy_n(std::array { scp[0], scp[1], scp[2], trail_position }.data(), 4,
                    data + offset);
        offset += 4;

        if (opt.thick_format) {
            // a_TexCoordVec4C2: cp end pos + size_end
            std::copy_n(std::array { ecp[0], ecp[1], ecp[2], size }.data(), 4, data + offset);
            offset += 4;
            // a_TexCoordVec4C3: color_end
            std::copy_n(std::array { p.color[0], p.color[1], p.color[2], p.alpha }.data(), 4,
                        data + offset);
            offset += 4;
        } else {
            // a_TexCoordVec3C2: cp end pos (padded to float4)
            std::copy_n(std::array { ecp[0], ecp[1], ecp[2], 0.0f }.data(), 4, data + offset);
            offset += 4;
        }

        // a_Color
        std::copy_n(std::array { p.color[0], p.color[1], p.color[2], p.alpha }.data(), 4,
                    data + offset);

        sv.SetVertexs(start_idx + seg_count, { data, one_size });
        seg_count++;
    }
    return seg_count;
}

inline void updateIndexArray(uint16_t index, size_t count, SceneIndexArray& iarray) noexcept {
    constexpr size_t single_size = 6;
    const uint16_t   cv          = index * 4;

    std::array<uint16_t, single_size> single;
    // 0 1 3
    // 1 2 3
    single[0] = cv;
    single[1] = cv + 1;
    single[2] = cv + 3;
    single[3] = cv + 1;
    single[4] = cv + 2;
    single[5] = cv + 3;
    // every particle
    for (uint16_t i = index; i < count; i++) {
        iarray.AssignHalf(i * single_size, single);
        for (auto& x : single) x += 4;
    }
}
} // namespace

void WPParticleRawGener::GenGLData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                   SceneMesh& mesh, ParticleRawGenSpecOp& specOp) {
    auto& sv = mesh.GetVertexArray(0);

    WPGOption opt;
    opt.thick_format    = sv.GetOption(WE_CB_THICK_FORMAT);
    opt.geometry_shader = sv.GetOption(WE_CB_GEOMETRY_SHADER);

    usize particle_num { 0 };

    if (sv.GetOption(WE_PRENDER_ROPE)) {
        if (opt.geometry_shader) {
            // GS path: 1 vertex per segment, no index buffer
            for (const auto& inst : instances) {
                if (inst->IsNoLiveParticle()) continue;
                particle_num += GenRopeParticleDataGS(inst->Particles(),
                                                       inst->GetBoundedData().pos, specOp, opt, sv,
                                                       particle_num);
            }
            sv.SetRenderVertexCount(particle_num);
        } else {
            for (const auto& inst : instances) {
                if (inst->IsNoLiveParticle()) continue;
                particle_num += GenRopeParticleData(inst->Particles(), inst->GetBoundedData().pos,
                                                    specOp, opt, sv, particle_num);
            }
        }
    } else {
        if (opt.geometry_shader) {
            particle_num += GenParticleDataGS(instances, specOp, opt, sv);
            sv.SetRenderVertexCount(particle_num);
        } else {
            particle_num += GenParticleData(instances, specOp, opt, sv);
        }
    }

    if (! opt.geometry_shader) {
        auto& si       = mesh.GetIndexArray(0);
        u16   indexNum = (si.DataCount() * 2) / 6;
        if (particle_num > indexNum) {
            updateIndexArray(indexNum, particle_num, si);
        }
        si.SetRenderDataCount(particle_num * 6 / 2);
    }
}
