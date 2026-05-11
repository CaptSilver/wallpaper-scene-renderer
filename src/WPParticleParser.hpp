#pragma once
#include "Particle/ParticleEmitter.h"
#include "wpscene/WPParticleObject.h"

namespace wallpaper
{
class WPParticleParser {
public:
    static ParticleInitOp
                              genParticleInitOp(const nlohmann::json&,
                                                std::span<const ParticleControlpoint> controlpoints = {});
    static ParticleOperatorOp genParticleOperatorOp(const nlohmann::json&,
                                                    const wpscene::ParticleInstanceoverride&);
    static ParticleEmittOp    genParticleEmittOp(const wpscene::Emitter&, bool sort = false,
                                                 u32 batch_size = 1, float burst_rate = 0.0f);
    static ParticleInitOp     genOverrideInitOp(const wpscene::ParticleInstanceoverride&);
};
} // namespace wallpaper
