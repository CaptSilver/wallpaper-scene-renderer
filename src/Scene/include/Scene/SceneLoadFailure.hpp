#pragma once

#include <functional>
#include <string>

namespace wallpaper
{

// Why a scene load gave up.  Each of these leaves the desktop black, and
// without a reason the only thing the user sees is the load watchdog's
// "nothing drew" -- which is true of a slow cold start too, so it tells them
// nothing about whether waiting will help.
enum class SceneLoadFailure
{
    PkgDirUnmountable,  // neither scene.pkg nor the physical directory mounted
    NoSceneJson,        // mounted fine, but holds no scene JSON we recognise
    SceneJsonMalformed, // found the JSON and the parser rejected it
};

using SceneLoadFailedCallback = std::function<void(const std::string& reason)>;

// `subject` is the package directory for the two mount-time failures and the
// scene id for a parse failure -- whichever one the user can act on.
inline std::string sceneLoadFailureReason(SceneLoadFailure kind, const std::string& subject) {
    switch (kind) {
    case SceneLoadFailure::PkgDirUnmountable:
        return "Could not open the wallpaper package: " + subject;
    case SceneLoadFailure::NoSceneJson:
        return "No scene data found in " + subject +
               " — this wallpaper may be a type the renderer does not handle";
    case SceneLoadFailure::SceneJsonMalformed:
        return "The scene data for " + subject + " is malformed and could not be parsed";
    }
    return "The wallpaper could not be loaded: " + subject;
}

inline void dispatchSceneLoadFailure(const SceneLoadFailedCallback& cb, SceneLoadFailure kind,
                                     const std::string& subject) {
    if (cb) cb(sceneLoadFailureReason(kind, subject));
}

} // namespace wallpaper
