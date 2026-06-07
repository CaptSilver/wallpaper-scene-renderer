#pragma once

#include <QJSValue>

namespace wek::qml_helper {

// Wallpaper Engine exposes two distinct script globals:
//
//   engine.canvasSize        — the scene's orthographic *design* canvas
//                              (scene.json general.orthogonalprojection).
//                              Constant for the lifetime of the scene.
//   engine.screenResolution  — the actual output/widget pixel size, which
//                              changes whenever the wallpaper is resized.
//
// Scripts position layers in canvas space, e.g. a puppet head origin of
// `scriptProperties.x * engine.canvasSize.x` lands at the canvas centre when
// x = 0.5.  The renderer then maps canvas space onto the screen through the
// ortho camera.  So canvasSize MUST stay pinned to the design canvas; only
// screenResolution tracks the widget size.
//
// Earlier this also rewrote canvasSize on every resize.  On any screen whose
// size differs from the design canvas that shifted every script-positioned
// layer (head, eyes, clock text) away from the static layers — e.g. a puppet
// face whose eyes/nose are script-driven would drift off the fixed face base.
// Update screenResolution only; leave canvasSize untouched.
inline void applyScreenResize(QJSValue& engineObj, int width, int height) {
    QJSValue sr = engineObj.property("screenResolution");
    sr.setProperty("x", width);
    sr.setProperty("y", height);
}

} // namespace wek::qml_helper
