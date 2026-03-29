#include <doctest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include "SceneTimerBridge.h"

using scenebackend::SceneTimerBridge;

// Spin the Qt event loop for a given number of milliseconds.
// This lets QTimers fire within doctest test cases.
static void processEventsFor(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
}

// ------------------------------------------------------------------
// SceneTimerBridge  (C++ level)
// ------------------------------------------------------------------
TEST_SUITE("SceneTimerBridge") {

TEST_CASE("timer IDs are unique and auto-incrementing") {
    QJSEngine engine;
    SceneTimerBridge bridge(&engine);

    QJSValue noop = engine.evaluate("(function(){})");
    int id1 = bridge.createTimer(noop, 5000, false);
    int id2 = bridge.createTimer(noop, 5000, false);
    int id3 = bridge.createTimer(noop, 5000, true);

    CHECK(id1 < id2);
    CHECK(id2 < id3);
    CHECK(bridge.activeCount() == 3);

    bridge.clearAll();
}

TEST_CASE("single-shot timer fires once and auto-removes") {
    QJSEngine engine;
    engine.evaluate("var counter = 0;");
    SceneTimerBridge bridge(&engine);

    QJSValue cb = engine.evaluate("(function(){ counter++; })");
    bridge.createTimer(cb, 10, /*repeat=*/false);
    CHECK(bridge.activeCount() == 1);

    processEventsFor(200);

    int count = engine.evaluate("counter").toInt();
    CHECK(count == 1);
    CHECK(bridge.activeCount() == 0);
}

TEST_CASE("interval timer fires multiple times") {
    QJSEngine engine;
    engine.evaluate("var counter = 0;");
    SceneTimerBridge bridge(&engine);

    QJSValue cb = engine.evaluate("(function(){ counter++; })");
    bridge.createTimer(cb, 15, /*repeat=*/true);

    processEventsFor(200);

    int count = engine.evaluate("counter").toInt();
    CHECK(count >= 3);
    CHECK(bridge.activeCount() == 1);

    bridge.clearAll();
}

TEST_CASE("clearTimer stops a specific timer") {
    QJSEngine engine;
    engine.evaluate("var counter = 0;");
    SceneTimerBridge bridge(&engine);

    QJSValue cb = engine.evaluate("(function(){ counter++; })");
    int id = bridge.createTimer(cb, 50, /*repeat=*/false);
    bridge.clearTimer(id);
    CHECK(bridge.activeCount() == 0);

    processEventsFor(200);
    CHECK(engine.evaluate("counter").toInt() == 0);
}

TEST_CASE("clearTimer with invalid ID is safe") {
    QJSEngine engine;
    SceneTimerBridge bridge(&engine);
    bridge.clearTimer(999); // should not crash
    CHECK(bridge.activeCount() == 0);
}

TEST_CASE("clearAll stops all timers") {
    QJSEngine engine;
    engine.evaluate("var a = 0; var b = 0;");
    SceneTimerBridge bridge(&engine);

    bridge.createTimer(engine.evaluate("(function(){ a++; })"), 15, true);
    bridge.createTimer(engine.evaluate("(function(){ b++; })"), 15, true);
    CHECK(bridge.activeCount() == 2);

    bridge.clearAll();
    CHECK(bridge.activeCount() == 0);

    processEventsFor(100);
    CHECK(engine.evaluate("a").toInt() == 0);
    CHECK(engine.evaluate("b").toInt() == 0);
}

TEST_CASE("postFire callback is invoked") {
    QJSEngine engine;
    int fireCount = 0;
    bool sawError = false;

    SceneTimerBridge bridge(&engine, nullptr,
        [&](int, bool error, const QString&) {
            fireCount++;
            if (error) sawError = true;
        });

    QJSValue cb = engine.evaluate("(function(){})");
    bridge.createTimer(cb, 10, false);

    processEventsFor(200);
    CHECK(fireCount == 1);
    CHECK_FALSE(sawError);
}

TEST_CASE("postFire reports errors from callback") {
    QJSEngine engine;
    bool sawError = false;

    SceneTimerBridge bridge(&engine, nullptr,
        [&](int, bool error, const QString&) {
            sawError = error;
        });

    QJSValue cb = engine.evaluate("(function(){ throw new Error('boom'); })");
    bridge.createTimer(cb, 10, false);

    processEventsFor(200);
    CHECK(sawError);
}

TEST_CASE("closure captures work") {
    QJSEngine engine;
    engine.globalObject().setProperty("result", 0);
    SceneTimerBridge bridge(&engine);

    QJSValue cb = engine.evaluate(
        "(function() {"
        "  var x = 42;"
        "  return function(){ result = x; };"
        "})()");
    bridge.createTimer(cb, 10, false);

    processEventsFor(200);
    CHECK(engine.evaluate("result").toInt() == 42);
}

} // TEST_SUITE SceneTimerBridge


// ------------------------------------------------------------------
// JS-level setTimeout / setInterval / clearTimeout / clearInterval
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Timers JS API") {

// Helper: set up engine + bridge + register JS wrapper functions
struct TimerEnv {
    QJSEngine engine;
    SceneTimerBridge* bridge;

    TimerEnv() {
        bridge = new SceneTimerBridge(&engine);
        engine.globalObject().setProperty(
            "_timerBridge", engine.newQObject(bridge));
        engine.evaluate(
            "function setTimeout(fn, delay)  { return _timerBridge.createTimer(fn, delay || 0, false); }\n"
            "function setInterval(fn, delay) { return _timerBridge.createTimer(fn, delay || 0, true); }\n"
            "function clearTimeout(id)  { _timerBridge.clearTimer(id); }\n"
            "function clearInterval(id) { _timerBridge.clearTimer(id); }\n"
        );
    }
};

TEST_CASE("setTimeout calls callback") {
    TimerEnv env;
    env.engine.evaluate("var fired = false; setTimeout(function(){ fired = true; }, 10);");

    processEventsFor(200);
    CHECK(env.engine.evaluate("fired").toBool());
}

TEST_CASE("setInterval fires repeatedly") {
    TimerEnv env;
    env.engine.evaluate("var n = 0; setInterval(function(){ n++; }, 15);");

    processEventsFor(200);
    CHECK(env.engine.evaluate("n").toInt() >= 3);

    env.bridge->clearAll();
}

TEST_CASE("clearTimeout prevents firing") {
    TimerEnv env;
    env.engine.evaluate(
        "var fired = false;"
        "var id = setTimeout(function(){ fired = true; }, 50);"
        "clearTimeout(id);");

    processEventsFor(200);
    CHECK_FALSE(env.engine.evaluate("fired").toBool());
}

TEST_CASE("clearInterval stops repeating") {
    TimerEnv env;
    env.engine.evaluate(
        "var n = 0;"
        "var id = setInterval(function(){ n++; }, 15);");

    processEventsFor(100);
    env.engine.evaluate("clearInterval(id);");

    int countAfterClear = env.engine.evaluate("n").toInt();
    CHECK(countAfterClear >= 1);

    processEventsFor(200);
    CHECK(env.engine.evaluate("n").toInt() == countAfterClear);
}

TEST_CASE("setTimeout with zero delay fires on next tick") {
    TimerEnv env;
    env.engine.evaluate("var ok = false; setTimeout(function(){ ok = true; }, 0);");

    processEventsFor(50);
    CHECK(env.engine.evaluate("ok").toBool());
}

TEST_CASE("multiple independent timeouts") {
    TimerEnv env;
    env.engine.evaluate(
        "var a = false, b = false;"
        "setTimeout(function(){ a = true; }, 10);"
        "setTimeout(function(){ b = true; }, 20);");

    processEventsFor(200);
    CHECK(env.engine.evaluate("a").toBool());
    CHECK(env.engine.evaluate("b").toBool());
}

} // TEST_SUITE SceneScript Timers JS API


// ------------------------------------------------------------------
// Device detection engine stubs
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Device Detection") {

// Helper: set up engine with the same stubs as setupTextScripts()
struct DeviceEnv {
    QJSEngine engine;

    DeviceEnv() {
        QJSValue engineObj = engine.newObject();
        engine.globalObject().setProperty("engine", engineObj);

        engine.evaluate(
            "engine.isDesktopDevice = function() { return true; };\n"
            "engine.isMobileDevice = function() { return false; };\n"
            "engine.isTabletDevice = function() { return false; };\n"
            "engine.isWallpaper = function() { return true; };\n"
            "engine.isScreensaver = function() { return false; };\n"
            "engine.isRunningInEditor = function() { return false; };\n"
        );
    }

    void setOrientation(float w, float h) {
        bool portrait = h > w;
        engine.evaluate(QString(
            "engine.isPortrait = function() { return %1; };\n"
            "engine.isLandscape = function() { return %2; };\n"
        ).arg(portrait ? "true" : "false")
         .arg(portrait ? "false" : "true"));
    }
};

TEST_CASE("isDesktopDevice returns true") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isDesktopDevice()").toBool() == true);
}

TEST_CASE("isMobileDevice returns false") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isMobileDevice()").toBool() == false);
}

TEST_CASE("isTabletDevice returns false") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isTabletDevice()").toBool() == false);
}

TEST_CASE("isWallpaper returns true") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isWallpaper()").toBool() == true);
}

TEST_CASE("isScreensaver returns false") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isScreensaver()").toBool() == false);
}

TEST_CASE("isRunningInEditor returns false") {
    DeviceEnv env;
    CHECK(env.engine.evaluate("engine.isRunningInEditor()").toBool() == false);
}

TEST_CASE("landscape orientation (wide)") {
    DeviceEnv env;
    env.setOrientation(1920, 1080);
    CHECK(env.engine.evaluate("engine.isLandscape()").toBool() == true);
    CHECK(env.engine.evaluate("engine.isPortrait()").toBool() == false);
}

TEST_CASE("portrait orientation (tall)") {
    DeviceEnv env;
    env.setOrientation(1080, 1920);
    CHECK(env.engine.evaluate("engine.isLandscape()").toBool() == false);
    CHECK(env.engine.evaluate("engine.isPortrait()").toBool() == true);
}

TEST_CASE("square is landscape (not portrait)") {
    DeviceEnv env;
    env.setOrientation(1080, 1080);
    CHECK(env.engine.evaluate("engine.isLandscape()").toBool() == true);
    CHECK(env.engine.evaluate("engine.isPortrait()").toBool() == false);
}

} // TEST_SUITE SceneScript Device Detection
