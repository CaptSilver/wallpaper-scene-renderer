#pragma once
#include "Core/Literals.hpp"
#include "Core/NoCopyMove.hpp"
#include "Core/MapSet.hpp"

#include <array>
#include <functional>
#include <string>
#include <string_view>

namespace wallpaper
{
class SceneNode;
class SceneShader;
class SceneLight;
class ShaderValue;
class SpriteAnimation;

using sprite_map_t    = Map<usize, SpriteAnimation>;
using UpdateUniformOp = std::function<void(std::string_view, const ShaderValue&)>;
using ExistsUniformOp = std::function<bool(std::string_view)>;

// Per-light material-instance uniform writer for the volumetric upload loop.
// `candidate_idx` is the running index (0..N-1) of volumetric-emitter
// candidates in scene.lights order — aligns with WPSceneParser's
// volumetricsConfig.per_light vector by construction (both populated by
// walking scene.lights with the same isVolumetricEmitterCandidate() filter).
// `slot` is the index 0..4 of the `g_RenderVar*` uniform on the per-light
// volumetrics_front / volumetrics_back material instance.
using WritePerLightVarOp = std::function<void(SceneLight*, int /*candidate_idx*/, int /*slot*/,
                                              const std::array<float, 4>&)>;

class IShaderValueUpdater : NoCopy, NoMove {
public:
    IShaderValueUpdater()          = default;
    virtual ~IShaderValueUpdater() = default;

    virtual void FrameBegin()                                                      = 0;
    virtual void InitUniforms(SceneNode*, const ExistsUniformOp&)                  = 0;
    virtual void UpdateUniforms(SceneNode*, sprite_map_t&, const UpdateUniformOp&) = 0;
    virtual void UpdateUniforms(SceneNode* n, sprite_map_t& s, const UpdateUniformOp& op,
                                const std::string& /*camera_override*/) {
        UpdateUniforms(n, s, op);
    }
    virtual void FrameEnd() = 0;

    virtual void MouseInput(double x, double y) = 0;
    virtual void SetTexelSize(float x, float y) = 0;
    virtual void SetScreenSize(i32 w, i32 h)    = 0;
};
} // namespace wallpaper
