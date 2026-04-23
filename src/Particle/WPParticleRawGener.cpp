#include "WPParticleRawGener.h"

#include <algorithm>
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

// Parent-alpha inheritance: walk up the bounded_data chain and multiply each ancestor
// particle's current alpha into a running factor.  This is WE's implicit semantic for
// nested particles — a child is only as visible as its parent chain.
//
// Critical for NieR 2B thunderbolt_glow and thunderbolt_beam_child: the main bolt
// particle has a full alpha pipeline (alphafade fadeouttime=0.5, alphachange 10→1,
// oscillatealpha, remapvalue sine on opacity) but the glow/beam_child subsystems
// compute their own alpha from their own operators — without inheritance they don't
// know the parent bolt is fading, and their short-lifetime sprites "pop off" at
// end-of-life instead of riding down with the parent.
//
// Multiplicative because visibility composes: P(child seen) = P(child) × P(parent).
// Invisible intermediate spawners (child_spawner has no alphafade, p.alpha≈1) are a
// no-op in the product, so the beam_child inherits the grandparent bolt's fade without
// special-casing.  Depth cap is a safety net against runaway chains.
inline float AncestorAlphaFactor(const ParticleInstance& inst) noexcept {
    float                         factor = 1.0f;
    const ParticleInstance*       cur    = &inst;
    constexpr int                 kMaxDepth = 8;
    for (int d = 0; d < kMaxDepth; d++) {
        const auto& bd = cur->GetBoundedData();
        if (bd.parent == nullptr || bd.particle_idx < 0) break;
        auto parent_particles = bd.parent->Particles();
        if ((std::size_t)bd.particle_idx >= parent_particles.size()) break;
        factor *= parent_particles[bd.particle_idx].alpha;
        cur = bd.parent;
    }
    return factor;
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
        const float anc_alpha = AncestorAlphaFactor(*inst);

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

            // color (alpha inherits multiplicatively from bounded ancestor chain)
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha * anc_alpha },
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
    float*                data = storage.data();

    const auto one_size = sv.OneSize();
    usize      i { 0 };
    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;
        const float anc_alpha = AncestorAlphaFactor(*inst);

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
            std::copy_n(std::array { p.rotation[0], p.rotation[1], p.rotation[2], size }.data(),
                        4,
                        data + offset);
            offset += 4;
            // a_Color (alpha inherits multiplicatively from bounded ancestor chain)
            std::copy_n(
                std::array { p.color[0], p.color[1], p.color[2], p.alpha * anc_alpha }.data(),
                4,
                data + offset);
            offset += 4;

            if (opt.thick_format) {
                // a_TexCoordVec4C1: velocity + lifetime
                std::copy_n(
                    std::array { p.velocity[0], p.velocity[1], p.velocity[2], lifetime }.data(),
                    4,
                    data + offset);
                offset += 4;
            }

            sv.SetVertexs(i, { data, one_size });
            i++;
        }
    }
    return i;
}

inline size_t GenRopeParticleData(std::span<const Particle> particles, const Vector3f& inst_pos,
                                  const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                  SceneVertexArray& sv, size_t start_idx,
                                  float anc_alpha = 1.0f) {
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

        float  size   = p.size / 2.0f;
        size_t offset = 0;

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
            { data + offset, totle_size }, std::array { sp_pos[0], sp_pos[1], sp_pos[2], size }, 4);
        offset += 4;
        // a_TexCoordVec4: end pos + trail_length
        AssignVertexTimes({ data + offset, totle_size },
                          std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length },
                          4);
        offset += 4;
        // a_TexCoordVec4C1: cp start + trail_position
        AssignVertexTimes({ data + offset, totle_size },
                          std::array { scp[0], scp[1], scp[2], trail_position },
                          4);
        offset += 4;

        if (opt.thick_format) {
            // a_TexCoordVec4C2: cp end pos, size_end
            AssignVertexTimes(
                { data + offset, totle_size }, std::array { ecp[0], ecp[1], ecp[2], size }, 4);
            offset += 4;
            // a_TexCoordVec4C3: color_end (ancestor-alpha inherit)
            AssignVertexTimes({ data + offset, totle_size },
                              std::array { p.color[0], p.color[1], p.color[2], p.alpha * anc_alpha },
                              4);
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

        // a_Color (ancestor-alpha inherit)
        AssignVertexTimes({ data + offset, totle_size },
                          std::array { p.color[0], p.color[1], p.color[2], p.alpha * anc_alpha },
                          4);

        sv.SetVertexs((start_idx + seg_count) * 4, { data, totle_size });
        seg_count++;
    }
    return seg_count;
}

// GS rope: 1 vertex per segment (geometry shader expands to triangle strip)
inline size_t GenRopeParticleDataGS(std::span<const Particle> particles, const Vector3f& inst_pos,
                                    const ParticleRawGenSpecOp& specOp, WPGOption opt,
                                    SceneVertexArray& sv, size_t start_idx,
                                    float anc_alpha = 1.0f) {
    const auto one_size  = sv.OneSize();
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

    // Reused-particle anti-trail: when a rope particle dies and respawns at a
    // different sequence position (mapsequencebetweencontrolpoints advances
    // its seq counter every spawn), the alive[] array still orders particles
    // by their storage index — so the newly-teleported slot sits between its
    // stable array neighbors and the ribbon draws a long diagonal streak to
    // its new home.  Skip segments whose length is a strong outlier vs. the
    // surrounding neighborhood.
    std::vector<float> seg_lens(alive.size() > 1 ? alive.size() - 1 : 0);
    for (size_t ai = 1; ai < alive.size(); ai++) {
        seg_lens[ai - 1] = (positions[ai] - positions[ai - 1]).norm();
    }
    float median_len = 0.0f;
    if (! seg_lens.empty()) {
        auto sorted = seg_lens;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        median_len = sorted[sorted.size() / 2];
    }
    // 4x median catches the respawn-jump artifact (a brand-new particle on the
    // far end of the CP line vs. its stable neighbor hops a full rope length)
    // without clipping legitimate oscillation/turbulence jitter.
    const float outlier_len = std::max(median_len * 4.0f, 1e-3f);

    for (size_t ai = 1; ai < alive.size(); ai++) {
        if (! seg_lens.empty() && seg_lens[ai - 1] > outlier_len) continue;

        const auto& pre_p = particles[alive[ai - 1]];
        const auto& p     = particles[alive[ai]];

        // Pre-discharge straight-line suppression: when a rope-particle burst
        // has just fired, every particle sits at its mapsequence CP-line
        // position with no turbulence (blendinstart > 0), so the ribbon
        // renders as a solid cable along the sword before the lightning
        // "discharges" visibly.  Alphafade ramps the alpha from 0 over the
        // same window, so skipping segments whose endpoints are still below
        // a visible alpha threshold hides the straight phase without any
        // lifetime hardcoding and naturally catches dying-phase fadeouts too.
        // Threshold picked below one frame of alphafade over a 1s lifetime
        // (1/60s * 10 = 0.017 per frame at fadeintime=0.1).
        if (std::max(pre_p.alpha, p.alpha) < 0.05f) continue;

        // WE rope-ribbon width convention: pass quarter-size so shader's
        // 2*sizeStart ribbon width comes out at p.size/2 (matches reference
        // NieR 2B thunderbolt thickness at 1440p; previously rendered 2x
        // wider than intended).
        float size_start = pre_p.size / 4.0f;
        float size_end   = p.size / 4.0f;

        float lifetime = p.lifetime;
        specOp(p, { &lifetime });
        float trail_position = (float)(ai - 1);

        Vector3f sp_pos  = positions[ai - 1];
        Vector3f ep_pos  = positions[ai];
        Vector3f pos_vec = ep_pos - sp_pos;

        // Tangents for Catmull-Rom
        Vector3f tan_start = pos_vec;
        if (ai >= 2) {
            tan_start = (positions[ai] - positions[ai - 2]) * 0.5f;
        }
        Vector3f tan_end = pos_vec;
        if (ai + 1 < alive.size()) {
            tan_end = (positions[ai + 1] - positions[ai - 1]) * 0.5f;
        }

        // Add lightning-like jaggedness using rotation property.
        // We use rotation.x and rotation.y as noise sources.
        // Independent jitter for start and end to make it look "alive".
        Vector3f jitter_start(pre_p.rotation[0], pre_p.rotation[1], pre_p.rotation[2]);
        Vector3f jitter_end(p.rotation[0], p.rotation[1], p.rotation[2]);

        // The vertex shader expects C_1 and C_2 to be defined as:
        // C_1 = P_0 + offset
        // C_2 = P_1 - offset
        // The vertex shader then decodes them into relative tangents (dt - offset).
        // This ensures the geometry shader generates a perfectly facing ribbon that 
        // follows the segment path while adding the desired Bezier jitter.
        Vector3f scp = sp_pos + jitter_start;
        Vector3f ecp = ep_pos - jitter_end;

        size_t offset = 0;

        // a_PositionVec4: start pos + size (start)
        std::copy_n(std::array { sp_pos[0], sp_pos[1], sp_pos[2], size_start }.data(),
                    4,
                    data + offset);
        offset += 4;
        // a_TexCoordVec4: end pos + trail_length
        std::copy_n(
            std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length }.data(), 4, data + offset);
        offset += 4;
        // a_TexCoordVec4C1: cp start + trail_position
        std::copy_n(std::array { scp[0], scp[1], scp[2], trail_position }.data(), 4, data + offset);
        offset += 4;

        // The shader ALWAYS expects a_TexCoordVec4C2.w to contain the size of the end particle.
        // It also unconditionally reads a_TexCoordVec4C3 for the end color gradient.
        // a_TexCoordVec4C2: cp end pos + size_end
        std::copy_n(std::array { ecp[0], ecp[1], ecp[2], size_end }.data(), 4, data + offset);
        offset += 4;
        // a_TexCoordVec4C3: color_end (ancestor-alpha inherit)
        std::copy_n(
            std::array { p.color[0], p.color[1], p.color[2], p.alpha * anc_alpha }.data(),
            4,
            data + offset);
        offset += 4;

        // a_Color (start color, ancestor-alpha inherit)
        std::copy_n(std::array { pre_p.color[0],
                                 pre_p.color[1],
                                 pre_p.color[2],
                                 pre_p.alpha * anc_alpha }
                        .data(),
                    4,
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
        const float anc_alpha = AncestorAlphaFactor(*inst);

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
                    cp_vec = pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

                Vector3f scp = sp_pos + cp_vec;
                Vector3f ecp = ep_pos - cp_vec;

                size_t offset = 0;

                // a_PositionVec4: start pos + size
                AssignVertexTimes({ data + offset, totle_size },
                                  std::array { sp_pos[0], sp_pos[1], sp_pos[2], size },
                                  4);
                offset += 4;
                // a_TexCoordVec4: end pos + trail_length
                AssignVertexTimes({ data + offset, totle_size },
                                  std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length },
                                  4);
                offset += 4;
                // a_TexCoordVec4C1: cp start + trail_position
                AssignVertexTimes({ data + offset, totle_size },
                                  std::array { scp[0], scp[1], scp[2], trail_position },
                                  4);
                offset += 4;

                if (opt.thick_format) {
                    float size_end = tp_old.size / 2.0f;
                    // a_TexCoordVec4C2: cp end pos + size_end
                    AssignVertexTimes({ data + offset, totle_size },
                                      std::array { ecp[0], ecp[1], ecp[2], size_end },
                                      4);
                    offset += 4;
                    // a_TexCoordVec4C3: color_end (ancestor-alpha inherit)
                    AssignVertexTimes({ data + offset, totle_size },
                                      std::array { tp_old.color[0],
                                                   tp_old.color[1],
                                                   tp_old.color[2],
                                                   tp_old.alpha * anc_alpha },
                                      4);
                    offset += 4;
                    // a_TexCoordC4: UV seam
                    std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                   1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                    AssignVertex({ data + offset, totle_size }, t, 4);
                    offset += 4;
                } else {
                    // a_TexCoordVec3C2: cp end pos
                    AssignVertexTimes(
                        { data + offset, totle_size }, std::array { ecp[0], ecp[1], ecp[2] }, 4);
                    offset += 4;
                    // a_TexCoordC3
                    std::array t { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                                   1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                    AssignVertex({ data + offset, totle_size }, t, 4);
                    offset += 4;
                }

                // a_Color (ancestor-alpha inherit)
                AssignVertexTimes({ data + offset, totle_size },
                                  std::array { tp_new.color[0],
                                               tp_new.color[1],
                                               tp_new.color[2],
                                               tp_new.alpha * anc_alpha },
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
    const auto            one_size   = sv.OneSize();
    size_t                total_segs = 0;
    std::array<float, 32> storage;
    float*                data = storage.data();

    static int s_gen_log_counter = 0;
    bool       do_log            = (s_gen_log_counter++ % 6000 == 1);

    for (const auto& inst : instances) {
        if (inst->IsNoLiveParticle()) continue;
        const float anc_alpha = AncestorAlphaFactor(*inst);

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
                    cp_vec = pos_vec.normalized().dot(cp_vec) > 0 ? cp_vec : -1.0f * cp_vec;

                Vector3f scp = sp_pos + cp_vec;
                Vector3f ecp = ep_pos - cp_vec;

                size_t offset = 0;

                // a_PositionVec4: start pos + size
                std::copy_n(
                    std::array { sp_pos[0], sp_pos[1], sp_pos[2], size }.data(), 4, data + offset);
                offset += 4;
                // a_TexCoordVec4: end pos + trail_length
                std::copy_n(std::array { ep_pos[0], ep_pos[1], ep_pos[2], trail_length }.data(),
                            4,
                            data + offset);
                offset += 4;
                // a_TexCoordVec4C1: cp start + trail_position
                std::copy_n(
                    std::array { scp[0], scp[1], scp[2], trail_position }.data(), 4, data + offset);
                offset += 4;

                if (opt.thick_format) {
                    float size_end = tp_old.size / 2.0f;
                    // a_TexCoordVec4C2: cp end pos + size_end
                    std::copy_n(
                        std::array { ecp[0], ecp[1], ecp[2], size_end }.data(), 4, data + offset);
                    offset += 4;
                    // a_TexCoordVec4C3: color_end (older endpoint, fades toward tail;
                    // ancestor-alpha inherit)
                    std::copy_n(std::array { tp_old.color[0],
                                             tp_old.color[1],
                                             tp_old.color[2],
                                             tp_old.alpha * fade_old * anc_alpha }
                                    .data(),
                                4,
                                data + offset);
                    offset += 4;
                } else {
                    // a_TexCoordVec3C2: cp end pos
                    std::copy_n(
                        std::array { ecp[0], ecp[1], ecp[2], 0.0f }.data(), 4, data + offset);
                    offset += 4;
                }

                // a_Color (newer endpoint, fades toward head but stays brightest;
                // ancestor-alpha inherit)
                std::copy_n(std::array { tp_new.color[0],
                                         tp_new.color[1],
                                         tp_new.color[2],
                                         tp_new.alpha * fade_new * anc_alpha }
                                .data(),
                            4,
                            data + offset);

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
                     particle_num,
                     instances.size(),
                     (int)opt.geometry_shader);
        }
    } else if (sv.GetOption(WE_PRENDER_ROPE)) {
        if (opt.geometry_shader) {
            // GS path: 1 vertex per segment, no index buffer
            for (const auto& inst : instances) {
                if (inst->IsNoLiveParticle()) continue;
                particle_num += GenRopeParticleDataGS(inst->Particles(),
                                                      inst->GetBoundedData().pos,
                                                      specOp,
                                                      opt,
                                                      sv,
                                                      particle_num,
                                                      AncestorAlphaFactor(*inst));
            }
            sv.SetRenderVertexCount(particle_num);
        } else {
            for (const auto& inst : instances) {
                if (inst->IsNoLiveParticle()) continue;
                particle_num += GenRopeParticleData(inst->Particles(),
                                                    inst->GetBoundedData().pos,
                                                    specOp,
                                                    opt,
                                                    sv,
                                                    particle_num,
                                                    AncestorAlphaFactor(*inst));
            }
        }
    } else {
        if (opt.geometry_shader) {
            particle_num += GenParticleDataGS(instances, specOp, opt, sv);
            sv.SetRenderVertexCount(particle_num);
            static int s_gs_sprite_log = 0;
            if (++s_gs_sprite_log % 600 == 1) {
                LOG_INFO("GenParticleDataGS: particle_num=%zu instances=%zu one_size=%zu thick=%d",
                         particle_num,
                         instances.size(),
                         sv.OneSize(),
                         (int)opt.thick_format);
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
