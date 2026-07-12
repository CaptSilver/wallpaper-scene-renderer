#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest.h>
#include <QCoreApplication>
#include <cstdlib>

int main(int argc, char** argv) {
    // Headless dispatch tests construct a SceneObject with no SceneWallpaper /
    // Vulkan device.  This env var makes the ctor leave m_scene null so the
    // test can inject its own IPropertyDispatchSink (test_SceneScriptDispatch).
    // Set before QCoreApplication so it's live for any static/early ctor too.
    qputenv("WEKDE_TEST_NO_SCENE", "1");
    QCoreApplication app(argc, argv);
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);
    return ctx.run();
}
