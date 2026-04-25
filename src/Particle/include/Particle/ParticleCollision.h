#pragma once

#include "Particle/Particle.h"

#include <Eigen/Core>

namespace wallpaper
{
namespace ParticleCollision
{

// Reflect a particle's velocity around a surface normal.  Only fires when
// the particle is moving INTO the surface (`v · n < 0`); particles already
// moving away are left alone so a reflected particle does not bounce back
// across the surface on the next frame.  `restitution` defaults to 1.0
// (perfectly elastic) — higher values amplify the bounce, lower values
// dampen it.
inline void ReflectVelocity(Particle& p, const Eigen::Vector3d& n,
                            double restitution = 1.0) noexcept {
    Eigen::Vector3d v = p.velocity.cast<double>();
    const double    v_dot_n = v.dot(n);
    if (v_dot_n >= 0.0) return;
    v -= (1.0 + restitution) * v_dot_n * n;
    p.velocity = v.cast<float>();
}

} // namespace ParticleCollision
} // namespace wallpaper
