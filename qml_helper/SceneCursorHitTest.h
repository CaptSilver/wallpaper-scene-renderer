#pragma once

#include <QtQml/QJSValue>
#include <cmath>

namespace scenebackend
{

// Camera-parallax parameters, mirror of WPShaderValueUpdater::m_parallax +
// m_mousePos.  The shader MVP path shifts a node's model matrix by
// `(nodePos - camPos + mouseVec) * parallaxDepth * amount` for main-camera
// draws, and we mirror the same offset here so clicks land on the layer's
// rendered position.
//
// Layer eligibility is already baked into the proxy's _state.parallaxDepth:
// WPSceneParser zeros it for layers whose final main-camera draw is a
// compose node that doesn't inherit parallax (see ParseImageObj's
// `!hasEffect` branch).  That means you can always pass the global config
// through `para.enable = true` — the shift degenerates to zero per-layer
// when the depth is zero, so layers not visibly shifted are hit-tested
// at their authored origin.
struct CursorParallax {
    bool  enable { false };
    float amount { 0.5f };
    float mouseInfluence { 0.1f };
    float camX { 0.0f }; // camera position X in scene units (ortho center)
    float camY { 0.0f };
    float mouseNx { 0.5f }; // mouse X widget-normalized 0..1
    float mouseNy { 0.5f }; // mouse Y widget-normalized 0..1 (top-down)
    float orthoW { 1920.0f };
    float orthoH { 1080.0f };
};

// AABB hit-test against a layer proxy built by _makeLayerProxy (SceneBackend
// embeds its state under `_state` with origin/scale/size subobjects).
// Centered on origin, width/height scaled by |scale|.  Returns false when
// size is zero or the proxy lacks the expected shape — text layers that
// don't route through WPImageObject used to fall into that case;
// WPSceneParser now feeds text dimensions through nameToObjState so the
// hitbox matches the rendered text.
//
// When `para.enable` is true and the proxy carries a non-zero `parallaxDepth`,
// the hit-test centre is shifted by the same amount the shader MVP shifts the
// rendered quad, so a mouse-driven parallax offset doesn't take cursor clicks
// out from under the layer they're supposed to hit.
inline bool hitTestLayerProxy(const QJSValue& thisLayerProxy, float sceneX, float sceneY,
                              const CursorParallax& para = {}) {
    if (! thisLayerProxy.isObject()) return false;
    QJSValue state = thisLayerProxy.property("_state");
    if (! state.isObject()) return false;

    // WE `solid` field: when explicitly false the layer opts out of cursor
    // hit-testing.  Wallpaper 2866203962's playerplay alpha script toggles
    // `element.solid = false` when media-control buttons fade out so the
    // invisible quads don't swallow clicks meant for the layers below.
    // Undefined means "on" by default — scripts only set it explicitly.
    QJSValue solid = state.property("solid");
    if (solid.isBool() && solid.toBool() == false) return false;

    QJSValue origin = state.property("origin");
    QJSValue scale  = state.property("scale");
    QJSValue size   = state.property("size");

    float ox = (float)origin.property("x").toNumber();
    float oy = (float)origin.property("y").toNumber();
    float sx = (float)scale.property("x").toNumber();
    float sy = (float)scale.property("y").toNumber();
    float sw = (float)size.property("x").toNumber();
    float sh = (float)size.property("y").toNumber();
    if (sw <= 0 || sh <= 0) return false;

    // Apply parallax shift so the hit-test matches the rendered visual.
    // Mirrors WPShaderValueUpdater.cpp's MVP branch.
    if (para.enable) {
        QJSValue pd = state.property("parallaxDepth");
        float    dx = (float)pd.property("x").toNumber();
        float    dy = (float)pd.property("y").toNumber();
        if (dx != 0.0f || dy != 0.0f) {
            float mouseVx = (0.5f - para.mouseNx) * para.orthoW * para.mouseInfluence;
            float mouseVy = (para.mouseNy - 0.5f) * para.orthoH * para.mouseInfluence;
            float paraX   = (ox - para.camX + mouseVx) * dx * para.amount;
            float paraY   = (oy - para.camY + mouseVy) * dy * para.amount;
            ox += paraX;
            oy += paraY;
        }
    }

    float halfW = sw * std::abs(sx) / 2.0f;
    float halfH = sh * std::abs(sy) / 2.0f;
    return std::abs(sceneX - ox) < halfW && std::abs(sceneY - oy) < halfH;
}

} // namespace scenebackend
