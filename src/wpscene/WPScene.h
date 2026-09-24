#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "WPJson.hpp"
#include <nlohmann/json.hpp>

namespace wallpaper
{

namespace wpscene
{

class Orthogonalprojection {
public:
    bool    FromJson(const nlohmann::json&);
    int32_t width;
    int32_t height;
    bool    auto_ { false };
    // WE editor preset selecting the bloom/compose pipeline:
    //   "low" / "medium" / absent → LDR bloom (legacy 4-pass + raw `combine`)
    //   "ultra"                   → scene-HDR (mip-chain + `combine_hdr`)
    //   "displayhdr"              → display-HDR (mip-chain + `combine_dhdr_upsample`)
    // Gated on scene-level `bloom == true` AND `hdr == true` for the HDR/displayHDR
    // branches; otherwise the LDR path is forced regardless of this string.
    std::string postprocessing;
};

class WPSceneCamera {
public:
    bool                     FromJson(const nlohmann::json&);
    std::array<float, 3>     center { 0.0f, 0.0f, 0.0f };
    std::array<float, 3>     eye { 0.0f, 0.0f, 1.0f };
    std::array<float, 3>     up { 0.0f, 1.0f, 0.0f };
    std::vector<std::string> paths;
};

class WPSceneGeneral {
public:
    bool                 FromJson(const nlohmann::json&);
    std::array<float, 3> clearcolor { 0.0f, 0.0f, 0.0f };
    bool                 cameraparallax { false };
    // Mirror WPCameraParallax's defaults: GET_JSON_NAME_VALUE leaves the
    // destination untouched when the key is absent, and plenty of scenes ship
    // a `general` block with no cameraparallax* keys at all.
    float                cameraparallaxamount { 1.0f };
    float                cameraparallaxdelay { 0.5f };
    float                cameraparallaxmouseinfluence { 1.0f };
    bool                 isOrtho { true };
    Orthogonalprojection orthogonalprojection { 1920, 1080, false, "" };
    float                zoom { 1.0f };
    float                fov { 50.0f };
    float                nearz { 0.01f };
    float                farz { 10000.0f };
    std::array<float, 3> ambientcolor { 0.2f, 0.2f, 0.2f };
    std::array<float, 3> skylightcolor { 0.3f, 0.3f, 0.3f };

    bool  hdr { false };
    bool  bloom { false };
    float bloomstrength { 2.0f };
    float bloomthreshold { 0.65f };
    float bloomhdrstrength { 2.0f };
    float bloomhdrthreshold { 1.0f };
    // WE authors these alongside the pair above; only bloomhdrfeather is
    // currently consumed (see buildBloomAndReflection's knee constant).
    // Iterations (chain depth) and scatter stay parsed-but-unread: wiring
    // scatter 1:1 into the shader's per-level multiply compounds
    // geometrically over the upsample chain and blew up a real wallpaper's
    // bloom in a prior attempt (see the E2E bloom-chain test's comment).
    int32_t bloomhdriterations { 8 };
    float   bloomhdrscatter { 1.619f };
    float   bloomhdrfeather { 0.1f };

    bool  camerashake { false };
    float camerashakeamplitude { 0.5f };
    float camerashakespeed { 1.0f };
    float camerashakeroughness { 0.5f };

    bool camerafade { false };

    // Scene-global environmental vectors (parsed but not currently wired into
    // operators — particle `movement` operators carry their own per-preset
    // `gravity` field).  Retained so future global-force features can consume
    // them without another parse pass.  NieR 2B ships `gravitydirection:
    // "0 -1 0"` (Y-down gravity for Y-up scene) and `windenabled: false`.
    std::array<float, 3> gravitydirection { 0.0f, -1.0f, 0.0f };
    float                gravitystrength { 1.0f };
    std::array<float, 3> winddirection { 0.707f, 0.707f, 0.0f };
    bool                 windenabled { false };
    float                windstrength { 1.0f };
};

class WPScene {
public:
    bool           FromJson(const nlohmann::json&);
    WPSceneCamera  camera;
    WPSceneGeneral general;
};

} // namespace wpscene
} // namespace wallpaper
