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
// When GS_ENABLED, the vertex shader reads all 3 rotation components from a_TexCoordVec4.xyz
// and particle size from a_TexCoordVec4.w. a_TexCoordC2 is not used.
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

            usize offset = 0;

            // a_Position (vec3, padded to 4 floats in vertex layout)
            std::copy_n(std::array { pos[0], pos[1], pos[2], 0.0f }.data(), 4, data + offset);
            offset += 4;
            // a_TexCoordVec4: rotation.xyz + size (GS reads all 3 rotation components)
            std::copy_n(
                std::array { p.rotation[0], p.rotation[1], p.rotation[2], size }.data(), 4,
                data + offset);
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

    // Collect alive particle indices to connect consecutive alive ones
    std::vector<size_t> alive;
    alive.reserve(particles.size());
    for (size_t i = 0; i < particles.size(); i++) {
        if (ParticleModify::LifetimeOk(particles[i])) alive.push_back(i);
    }

    float trail_length = (float)alive.size();

    for (size_t ai = 1; ai < alive.size(); ai++) {
        const auto& pre_p = particles[alive[ai - 1]];
        const auto& p     = particles[alive[ai]];

        float       size  = p.size / 2.0f;
        size_t      offset = 0;

        float lifetime = p.lifetime;
        specOp(p, { &lifetime });
        float trail_position = (float)(ai - 1);

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
    const auto one_size = sv.OneSize();
    size_t     seg_count = 0;

    std::array<float, 32> storage;
    float*                data = storage.data();

    // Collect alive particle indices to connect consecutive alive ones
    // (dead particles in the middle don't break the rope chain)
    std::vector<size_t> alive;
    alive.reserve(particles.size());
    for (size_t i = 0; i < particles.size(); i++) {
        if (ParticleModify::LifetimeOk(particles[i])) alive.push_back(i);
    }

    float trail_length = (float)alive.size();

    // Precompute positions for Catmull-Rom tangent calculation
    std::vector<Vector3f> positions(alive.size());
    for (size_t i = 0; i < alive.size(); i++)
        positions[i] = Vector3f { particles[alive[i]].position } + inst_pos;

    for (size_t ai = 1; ai < alive.size(); ai++) {
        const auto& pre_p = particles[alive[ai - 1]];
        const auto& p     = particles[alive[ai]];

        float size = p.size / 2.0f;

        float lifetime = p.lifetime;
        specOp(p, { &lifetime });
        float trail_position = (float)(ai - 1);

        Vector3f sp_pos  = positions[ai - 1];
        Vector3f ep_pos  = positions[ai];
        Vector3f pos_vec = ep_pos - sp_pos;

        // Catmull-Rom tangents for smooth Bézier curves
        Vector3f tan_start = pos_vec;
        if (ai >= 2) {
            tan_start = (positions[ai] - positions[ai - 2]) * 0.5f;
        }
        Vector3f tan_end = pos_vec;
        if (ai + 1 < alive.size()) {
            tan_end = (positions[ai + 1] - positions[ai - 1]) * 0.5f;
        }

        Vector3f scp = tan_start;
        Vector3f ecp = tan_end;

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

// Spritetrail: each particle's trail history rendered as mini-rope segments
inline size_t GenSpriteTrailData(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv) noexcept {
    std::array<float, 32 * 4> storage;
    float*                    data = storage.data();

    const auto one_size   = sv.OneSize();
    const auto totle_size = one_size * 4;
    size_t     total_segs = 0;

    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        auto  particles = inst->Particles();
        auto& trails    = inst->TrailHistories();
        auto  inst_pos  = inst->GetBoundedData().pos;

        for (size_t pi = 0; pi < particles.size(); pi++) {
            const auto& p = particles[pi];
            if (! ParticleModify::LifetimeOk(p)) continue;
            if (pi >= trails.size()) continue;

            const auto& trail  = trails[pi];
            u32         active = trail.ActiveCount();
            if (active < 2) continue;

            float trail_length = (float)active;

            for (u32 ti = 1; ti < active; ti++) {
                const auto& tp_new = trail.At(ti - 1); // newer
                const auto& tp_old = trail.At(ti);     // older

                float    trail_position = (float)(ti - 1);
                float    size           = tp_new.size / 2.0f;
                Vector3f sp_pos         = tp_new.position + inst_pos;
                Vector3f ep_pos         = tp_old.position + inst_pos;
                Vector3f pos_vec        = ep_pos - sp_pos;

                Vector3f cp_vec =
                    AngleAxisf(p.rotation[2] + (float)M_PI / 2.0f, Vector3f::UnitZ()) *
                    Vector3f { 0.0f, size / 2.0f, 0.0f };
                if (pos_vec.norm() > 0.001f)
                    cp_vec =
                        pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

                Vector3f scp = sp_pos + cp_vec;
                Vector3f ecp = ep_pos - cp_vec;

                size_t offset = 0;

                // a_PositionVec4: start pos + size
                AssignVertexTimes({ data + offset, totle_size },
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
                    float size_end = tp_old.size / 2.0f;
                    // a_TexCoordVec4C2: cp end pos + size_end
                    AssignVertexTimes({ data + offset, totle_size },
                                      std::array { ecp[0], ecp[1], ecp[2], size_end }, 4);
                    offset += 4;
                    // a_TexCoordVec4C3: color_end
                    AssignVertexTimes(
                        { data + offset, totle_size },
                        std::array { tp_old.color[0], tp_old.color[1], tp_old.color[2],
                                     tp_old.alpha },
                        4);
                    offset += 4;
                    // a_TexCoordC4: UV seam
                    std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                   1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                    AssignVertex({ data + offset, totle_size }, t, 4);
                    offset += 4;
                } else {
                    // a_TexCoordVec3C2: cp end pos
                    AssignVertexTimes({ data + offset, totle_size },
                                      std::array { ecp[0], ecp[1], ecp[2] }, 4);
                    offset += 4;
                    // a_TexCoordC3
                    std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                   1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                    AssignVertex({ data + offset, totle_size }, t, 4);
                    offset += 4;
                }

                // a_Color
                AssignVertexTimes(
                    { data + offset, totle_size },
                    std::array { tp_new.color[0], tp_new.color[1], tp_new.color[2],
                                 tp_new.alpha },
                    4);

                sv.SetVertexs(total_segs * 4, { data, totle_size });
                total_segs++;
            }
        }
    }
    return total_segs;
}

// GS spritetrail: 1 vertex per trail segment
inline size_t GenSpriteTrailDataGS(std::span<const std::unique_ptr<ParticleInstance>> instances,
                                    const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                    SceneVertexArray& sv) noexcept {
    const auto            one_size = sv.OneSize();
    size_t                total_segs = 0;
    std::array<float, 32> storage;
    float*                data = storage.data();

    static int s_gen_log_counter = 0;
    bool do_log = (s_gen_log_counter++ % 6000 == 1);

    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;

        auto  particles = inst->Particles();
        auto& trails    = inst->TrailHistories();
        auto  inst_pos  = inst->GetBoundedData().pos;

        for (size_t pi = 0; pi < particles.size(); pi++) {
            const auto& p = particles[pi];
            if (! ParticleModify::LifetimeOk(p)) continue;
            if (pi >= trails.size()) continue;

            const auto& trail  = trails[pi];
            u32         active = trail.ActiveCount();
            if (active < 2) continue;

            float trail_length = (float)active;

            // Linear alpha fade along the trail: head (newest) stays bright,
            // tail (oldest) fades to transparent.  Without this the trail is a
            // uniformly bright line that just vanishes at the tail — matches WE
            // Windows behaviour, which fades the per-vertex alpha along trail index.
            const float fade_denom = active > 1 ? (float)(active - 1) : 1.0f;

            for (u32 ti = 1; ti < active; ti++) {
                const auto& tp_new = trail.At(ti - 1);
                const auto& tp_old = trail.At(ti);

                // Per-endpoint fade (0 = oldest endpoint, 1 = newest endpoint)
                const float fade_new = 1.0f - (float)(ti - 1) / fade_denom;
                const float fade_old = 1.0f - (float)ti / fade_denom;

                float    trail_position = (float)(ti - 1);
                float    size           = tp_new.size / 2.0f;
                Vector3f sp_pos         = tp_new.position + inst_pos;
                Vector3f ep_pos         = tp_old.position + inst_pos;
                Vector3f pos_vec        = ep_pos - sp_pos;

                Vector3f cp_vec =
                    AngleAxisf(p.rotation[2] + (float)M_PI / 2.0f, Vector3f::UnitZ()) *
                    Vector3f { 0.0f, size / 2.0f, 0.0f };
                if (pos_vec.norm() > 0.001f)
                    cp_vec =
                        pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

                Vector3f scp = sp_pos + cp_vec;
                Vector3f ecp = ep_pos - cp_vec;

                size_t offset = 0;

                // a_PositionVec4: start pos + size
                std::copy_n(std::array { sp_pos[0], sp_pos[1], sp_pos[2], size }.data(), 4,
                            data + offset);
                offset += 4;
                // a_TexCoordVec4: end pos + trail_length
                std::copy_n(
                    std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length }.data(), 4,
                    data + offset);
                offset += 4;
                // a_TexCoordVec4C1: cp start + trail_position
                std::copy_n(
                    std::array { scp[0], scp[1], scp[2], trail_position }.data(), 4,
                    data + offset);
                offset += 4;

                if (opt.thick_format) {
                    float size_end = tp_old.size / 2.0f;
                    // a_TexCoordVec4C2: cp end pos + size_end
                    std::copy_n(
                        std::array { ecp[0], ecp[1], ecp[2], size_end }.data(), 4,
                        data + offset);
                    offset += 4;
                    // a_TexCoordVec4C3: color_end (older endpoint, fades toward tail)
                    std::copy_n(std::array { tp_old.color[0], tp_old.color[1],
                                             tp_old.color[2], tp_old.alpha * fade_old }
                                    .data(),
                                4, data + offset);
                    offset += 4;
                } else {
                    // a_TexCoordVec3C2: cp end pos
                    std::copy_n(
                        std::array { ecp[0], ecp[1], ecp[2], 0.0f }.data(), 4,
                        data + offset);
                    offset += 4;
                }

                // a_Color (newer endpoint, fades toward head but stays brightest)
                std::copy_n(std::array { tp_new.color[0], tp_new.color[1],
                                         tp_new.color[2], tp_new.alpha * fade_new }
                                .data(),
                            4, data + offset);

                sv.SetVertexs(total_segs, { data, one_size });
                total_segs++;
            }
        }
    }
    if (do_log && total_segs > 0) {
        LOG_INFO("GenSpriteTrailDataGS: total_segs=%zu one_size=%zu", total_segs, one_size);
    }
    return total_segs;
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

    if (sv.GetOption(WE_PRENDER_SPRITETRAIL)) {
        if (opt.geometry_shader) {
            particle_num = GenSpriteTrailDataGS(instances, specOp, opt, sv);
            sv.SetRenderVertexCount(particle_num);
        } else {
            particle_num = GenSpriteTrailData(instances, specOp, opt, sv);
        }
        static int s_st_log = 0;
        if (++s_st_log % 6000 == 1) {
            LOG_INFO("spritetrail GenGLData: segs=%zu instances=%zu gs=%d",
                     particle_num, instances.size(), (int)opt.geometry_shader);
        }
    } else if (sv.GetOption(WE_PRENDER_ROPE)) {
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
            static int s_gs_sprite_log = 0;
            if (++s_gs_sprite_log % 600 == 1) {
                LOG_INFO("GenParticleDataGS: particle_num=%zu instances=%zu one_size=%zu thick=%d",
                         particle_num, instances.size(), sv.OneSize(), (int)opt.thick_format);
            }
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
