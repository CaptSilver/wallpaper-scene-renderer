#pragma once

#include <QtQml/QJSEngine>
#include <QtQml/QJSValue>
#include <QString>

namespace scenebackend
{

// Build the JS-visible cursor event object passed to cursorDown / cursorMove /
// cursorClick / cursorUp / cursorEnter / cursorLeave handlers.
//
// worldPosition is scene-ortho ("world", Y-up) and screenPosition is widget
// pixels ("screen", Y-down, units of engine.screenResolution which
// fireResizeScreen sets to widget pixels at every resize).  Splitting the two
// matters because the common WE-Windows script idiom
//
//     (event.screenPosition.x - engine.screenResolution.x/2) / scale
//
// requires both sides to share units; before the split csp was scene-ortho and
// screenResolution was widget pixels, so the idiom only worked on the
// incidental case where the two coincide (1920x1080 widget against a
// 1920x1080-ortho scene).
//
// Real-Time Earth (3557068717) `视角控制` reads event.screenPosition.{x,y}
// to seed lastMouseX/Y for drag-delta math; Game of Life (3453251764) reads
// event.worldPosition for tooltip placement.  Keeping the two coordinate
// spaces explicit lets each handler pick the surface that matches its math.
//
// Lives in a header so scenescript_tests can exercise the same event-shape
// without pulling in QtQuick / Vulkan via SceneObject.
inline QJSValue makeCursorEvent(QJSEngine* engine,
                                float sceneX, float sceneY,
                                float screenX, float screenY) {
    QJSValue ev = engine->newObject();
    ev.setProperty("x", (double)sceneX);
    ev.setProperty("y", (double)sceneY);
    // worldPosition as Vec3 (for drag scripts that use .add()/.subtract())
    QJSValue wp =
        engine->evaluate(QString("new Vec3(%1,%2,0)").arg((double)sceneX).arg((double)sceneY));
    ev.setProperty("worldPosition", wp);
    // screenPosition as Vec2 in widget pixels — Real-Time Earth (3557068717)
    // `视角控制` reads event.screenPosition.x/.y to seed lastMouseX/Y for
    // drag-delta math; without this property the handler throws
    // "cannot read property 'x' of undefined" and drag never starts.
    QJSValue sp =
        engine->evaluate(QString("new Vec2(%1,%2)").arg((double)screenX).arg((double)screenY));
    ev.setProperty("screenPosition", sp);
    return ev;
}

// Per-tick refresh of input.cursorWorldPosition / input.cursorScreenPosition.
// Splits "world" (scene-ortho, Y-up) from "screen" (widget pixels, Y-down)
// for the same reason makeCursorEvent does.  Called from refreshEngineTickGlobals
// in SceneBackend.cpp on every text / colour / property script tick.
inline void writeCursorTickGlobals(QJSValue& cwp, QJSValue& csp,
                                   float sceneX, float sceneY,
                                   float screenX, float screenY) {
    cwp.setProperty("x", (double)sceneX);
    cwp.setProperty("y", (double)sceneY);
    csp.setProperty("x", (double)screenX);
    csp.setProperty("y", (double)screenY);
}

} // namespace scenebackend
