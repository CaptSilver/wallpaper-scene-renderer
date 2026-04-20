#pragma once

#include <QtQml/QJSValue>
#include <cmath>

namespace scenebackend
{

// AABB hit-test against a layer proxy built by _makeLayerProxy (SceneBackend
// embeds its state under `_state` with origin/scale/size subobjects).  Centered
// on origin, width/height scaled by |scale|.  Returns false when size is zero
// or the proxy lacks the expected shape — text layers that don't route
// through WPImageObject used to fall into that case; WPSceneParser now feeds
// text dimensions through nameToObjState so the hitbox matches the rendered
// text.
inline bool hitTestLayerProxy(const QJSValue& thisLayerProxy,
                              float sceneX, float sceneY) {
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

    float halfW = sw * std::abs(sx) / 2.0f;
    float halfH = sh * std::abs(sy) / 2.0f;
    return std::abs(sceneX - ox) < halfW && std::abs(sceneY - oy) < halfH;
}

} // namespace scenebackend
