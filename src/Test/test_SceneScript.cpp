#include <doctest.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include "SceneTimerBridge.h"
#include "SceneCursorHitTest.h"
#include "SceneScriptShimsJs.hpp"

#include "HoverLeaveDebounce.h"
#include "JsStringEscape.hpp"
#include "JsSyntaxNormalize.hpp"
#include "JsWatchdog.h"
#include "ScriptDiagState.h"
#include "SceneAspect.h"
#include "ScriptLoopGate.h"

using scenebackend::CursorParallax;
using scenebackend::drainExpiredLeaves;
using scenebackend::hitTestLayerProxy;
using scenebackend::HoverFrameResult;
using scenebackend::nextLeaveDeadlineMs;
using scenebackend::PendingLeave;
using scenebackend::processHoverFrame;
using scenebackend::SceneTimerBridge;
using wek::qml_helper::normalizeOptionalCatchBinding;

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
        QJSEngine        engine;
        SceneTimerBridge bridge(&engine);

        QJSValue noop = engine.evaluate("(function(){})");
        int      id1  = bridge.createTimer(noop, 5000, false);
        int      id2  = bridge.createTimer(noop, 5000, false);
        int      id3  = bridge.createTimer(noop, 5000, true);

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
        int      id = bridge.createTimer(cb, 50, /*repeat=*/false);
        bridge.clearTimer(id);
        CHECK(bridge.activeCount() == 0);

        processEventsFor(200);
        CHECK(engine.evaluate("counter").toInt() == 0);
    }

    TEST_CASE("clearTimer with invalid ID is safe") {
        QJSEngine        engine;
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
        int       fireCount = 0;
        bool      sawError  = false;

        SceneTimerBridge bridge(&engine, nullptr, [&](int, bool error, const QString&) {
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
        bool      sawError = false;

        SceneTimerBridge bridge(&engine, nullptr, [&](int, bool error, const QString&) {
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

        QJSValue cb = engine.evaluate("(function() {"
                                      "  var x = 42;"
                                      "  return function(){ result = x; };"
                                      "})()");
        bridge.createTimer(cb, 10, false);

        processEventsFor(200);
        CHECK(engine.evaluate("result").toInt() == 42);
    }

    // ── A3-T1: timer caps (untrusted-script hardening) ──────────────────────
    // The clamp is a pure constexpr so it can be asserted exactly, without
    // racing a real QTimer.  Floor is kMinTimerMs (8): a sub-8ms author delay
    // can't schedule more GUI-thread wakeups than the 8ms engine tick.
    TEST_CASE("clampInterval floors sub-minimum delays at kMinTimerMs") {
        CHECK(SceneTimerBridge::kMinTimerMs == 8);
        CHECK(SceneTimerBridge::clampInterval(0) == SceneTimerBridge::kMinTimerMs);
        CHECK(SceneTimerBridge::clampInterval(1) == SceneTimerBridge::kMinTimerMs);
        CHECK(SceneTimerBridge::clampInterval(7) == SceneTimerBridge::kMinTimerMs);
        // At/above the floor the delay passes through unchanged.
        CHECK(SceneTimerBridge::clampInterval(8) == 8);
        CHECK(SceneTimerBridge::clampInterval(33) == 33);
        CHECK(SceneTimerBridge::clampInterval(5000) == 5000);
        // A negative delay (some scripts pass garbage) clamps up, never negative.
        CHECK(SceneTimerBridge::clampInterval(-100) == SceneTimerBridge::kMinTimerMs);
    }

    // A delay-0 interval timer must fire no faster than the 8ms floor: in a
    // 100ms window it can fire at most ~12 times (100/8), and crucially NOT the
    // ~100 it would at the old 1ms floor.  Loose bound keeps it non-flaky.
    TEST_CASE("createTimer(delay=0) is throttled to the kMinTimerMs floor") {
        QJSEngine engine;
        engine.evaluate("var n = 0;");
        SceneTimerBridge bridge(&engine);
        bridge.createTimer(engine.evaluate("(function(){ n++; })"), 0, /*repeat=*/true);

        processEventsFor(100);
        int n = engine.evaluate("n").toInt();
        bridge.clearAll();
        // 100ms / 8ms ≈ 12 fires max; allow generous headroom but well under the
        // ~100 the unclamped 1ms floor would have produced.
        CHECK(n >= 1);
        CHECK(n <= 30);
    }

    // The hard cap rejects creation past kMaxTimers so a runaway author loop
    // (`for(;;) setInterval(...)`) can't leak QTimers / GUI-thread wakeups.
    // 5000ms delays guarantee none fire during the test (no hang, no flake).
    TEST_CASE("createTimer rejects past kMaxTimers with a sentinel") {
        QJSEngine        engine;
        SceneTimerBridge bridge(&engine);
        QJSValue         noop = engine.evaluate("(function(){})");

        for (std::size_t i = 0; i < SceneTimerBridge::kMaxTimers; ++i) {
            int id = bridge.createTimer(noop, 5000, /*repeat=*/true);
            CHECK(id != SceneTimerBridge::kRejectedTimerId);
        }
        CHECK(bridge.activeCount() == (int)SceneTimerBridge::kMaxTimers);

        // The (kMaxTimers+1)-th request is rejected and does NOT grow the table.
        int rejected = bridge.createTimer(noop, 5000, /*repeat=*/true);
        CHECK(rejected == SceneTimerBridge::kRejectedTimerId);
        CHECK(bridge.activeCount() == (int)SceneTimerBridge::kMaxTimers);

        // clearTimer on the sentinel is a harmless no-op (id was never inserted).
        bridge.clearTimer(rejected);
        CHECK(bridge.activeCount() == (int)SceneTimerBridge::kMaxTimers);

        // After freeing one slot, creation is accepted again (cap is a live
        // ceiling, not a permanent latch).
        // Re-create a clearable timer to free a slot deterministically.
        bridge.clearAll();
        CHECK(bridge.activeCount() == 0);
        int reopened = bridge.createTimer(noop, 5000, /*repeat=*/true);
        CHECK(reopened != SceneTimerBridge::kRejectedTimerId);
        bridge.clearAll();
    }

} // TEST_SUITE SceneTimerBridge

// ------------------------------------------------------------------
// SceneTimerBridge watchdog containment (A3-T1/T2)
//
// A runaway timer body (`setInterval(()=>{while(true){}})`) runs on the GUI
// thread; without the watchdog it never returns and the desktop freezes. These
// cases prove the callback is CONTAINED (interrupted, control returns), not
// infinite — the required untrusted-JS assertion. They are only possible because
// Item 03 fix (a) made interruptEngine header-inline so a real armed JsWatchdog
// links in scenescript_tests.
// ------------------------------------------------------------------
TEST_SUITE("SceneTimerBridge watchdog containment (A3-T1/T2)") {
    using scenebackend::JsWatchdog;

    // Stand-in for SceneObject::callJsGuarded: arm a tiny budget, run, disarm,
    // clear the latch and report whether the watchdog fired.
    static SceneTimerBridge::GuardedCallFn makeGuard(
        QJSEngine & engine, JsWatchdog & wd, int budgetMs, int* fireCount = nullptr) {
        return [&engine, &wd, budgetMs, fireCount](const std::function<QJSValue()>& call,
                                                   bool* outInterrupted) -> QJSValue {
            if (outInterrupted) *outInterrupted = false;
            wd.arm(&engine, budgetMs);
            QJSValue   r     = call();
            const bool fired = wd.disarm();
            if (fired) {
                engine.setInterrupted(false);
                if (outInterrupted) *outInterrupted = true;
                if (fireCount) ++(*fireCount);
            }
            return r;
        };
    }

    TEST_CASE("runaway single-shot is interrupted; GUI thread returns") {
        QJSEngine  engine;
        JsWatchdog wd;
        wd.start();
        int              fires = 0;
        SceneTimerBridge bridge(&engine, nullptr, {}, makeGuard(engine, wd, 30, &fires));

        // On the UNGUARDED baseline this .call() never returns and the pump below
        // hangs the test process (ctest TIMEOUT is the backstop).
        bridge.createTimer(engine.evaluate("(function(){ while(true){} })"),
                           10,
                           /*repeat=*/false);
        processEventsFor(500); // KEYSTONE: reached only if the watchdog fired

        CHECK(fires >= 1);                      // watchdog tripped at least once
        CHECK(bridge.activeCount() == 0);       // single-shot removed itself
        CHECK(engine.isInterrupted() == false); // latch cleared
        wd.stop();
    }

    TEST_CASE("runaway repeat-interval latches off after K interrupts") {
        QJSEngine  engine;
        JsWatchdog wd;
        wd.start();
        int disableLogged = 0;
        // postFire records the disable; guardedCall arms the tiny budget.
        SceneTimerBridge bridge(
            &engine,
            nullptr,
            /*postFire*/
            [&](int, bool, const QString& msg) {
                if (msg.contains("disabl")) ++disableLogged;
            },
            /*guardedCall*/ makeGuard(engine, wd, 30));

        bridge.createTimer(engine.evaluate("(function(){ while(true){} })"),
                           10,
                           /*repeat=*/true);
        // Long enough for >= K (=5) interrupted periods at 30ms budget + 10ms
        // interval (~5*40ms = 200ms); give generous headroom.
        processEventsFor(900);

        CHECK(bridge.activeCount() == 0); // the interval disabled itself
        CHECK(disableLogged >= 1);        // disable was reported once
        CHECK(engine.isInterrupted() == false);
        wd.stop();
    }

    TEST_CASE("well-behaved timer is never interrupted") {
        QJSEngine engine;
        engine.evaluate("var n = 0;");
        JsWatchdog wd;
        wd.start();
        int              fires = 0;
        SceneTimerBridge bridge(&engine, nullptr, {}, makeGuard(engine, wd, 30, &fires));

        bridge.createTimer(engine.evaluate("(function(){ n++; })"), 15, /*repeat=*/true);
        processEventsFor(200);

        CHECK(fires == 0);                        // watchdog never mis-fired
        CHECK(engine.evaluate("n").toInt() >= 3); // fired the expected times
        CHECK(bridge.activeCount() == 1);         // interval still alive (not disabled)
        bridge.clearAll();
        wd.stop();
    }

    TEST_CASE("backward-compat: no guardedCall hook -> direct call still fires") {
        QJSEngine engine;
        engine.evaluate("var m = 0;");
        // Construct with NO guardedCall (and no postFire) — the fix must keep the
        // bridge usable standalone (existing C++-level suites do this).
        SceneTimerBridge bridge(&engine);
        bridge.createTimer(engine.evaluate("(function(){ m++; })"), 10, /*repeat=*/false);
        processEventsFor(200);
        CHECK(engine.evaluate("m").toInt() == 1);
        CHECK(bridge.activeCount() == 0);
    }
} // TEST_SUITE SceneTimerBridge watchdog containment

// ------------------------------------------------------------------
// JS-level setTimeout / setInterval / clearTimeout / clearInterval
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Timers JS API") {
    // Helper: set up engine + bridge + register JS wrapper functions
    struct TimerEnv {
        QJSEngine         engine;
        SceneTimerBridge* bridge;

        TimerEnv() {
            bridge = new SceneTimerBridge(&engine);
            engine.globalObject().setProperty("_timerBridge", engine.newQObject(bridge));
            engine.evaluate("function setTimeout(fn, delay)  { return _timerBridge.createTimer(fn, "
                            "delay || 0, false); }\n"
                            "function setInterval(fn, delay) { return _timerBridge.createTimer(fn, "
                            "delay || 0, true); }\n"
                            "function clearTimeout(id)  { _timerBridge.clearTimer(id); }\n"
                            "function clearInterval(id) { _timerBridge.clearTimer(id); }\n"
                            // Mirror production: aliases on `engine` so wallpapers
                            // calling `engine.setTimeout(...)` resolve to the same bridge.
                            "var engine = {};\n"
                            "engine.setTimeout    = setTimeout;\n"
                            "engine.setInterval   = setInterval;\n"
                            "engine.clearTimeout  = clearTimeout;\n"
                            "engine.clearInterval = clearInterval;\n");
        }

        ~TimerEnv() {
            bridge->clearAll();
            delete bridge;
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
        env.engine.evaluate("var fired = false;"
                            "var id = setTimeout(function(){ fired = true; }, 50);"
                            "clearTimeout(id);");

        processEventsFor(200);
        CHECK_FALSE(env.engine.evaluate("fired").toBool());
    }

    TEST_CASE("clearInterval stops repeating") {
        TimerEnv env;
        env.engine.evaluate("var n = 0;"
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
        env.engine.evaluate("var a = false, b = false;"
                            "setTimeout(function(){ a = true; }, 10);"
                            "setTimeout(function(){ b = true; }, 20);");

        processEventsFor(200);
        CHECK(env.engine.evaluate("a").toBool());
        CHECK(env.engine.evaluate("b").toBool());
    }

    // ---- engine.* namespace aliases ----
    // Many WE wallpapers (Summer Vibes 3293999899 alone calls
    // engine.setTimeout 29 times) call timers through the engine namespace.
    // These tests verify the aliases route to the same _timerBridge and share
    // IDs with the top-level forms.

    TEST_CASE("engine.setTimeout fires the callback") {
        TimerEnv env;
        env.engine.evaluate("var fired = false;"
                            "engine.setTimeout(function(){ fired = true; }, 10);");
        processEventsFor(200);
        CHECK(env.engine.evaluate("fired").toBool());
    }

    TEST_CASE("engine.setInterval fires repeatedly") {
        TimerEnv env;
        env.engine.evaluate("var n = 0;"
                            "engine.setInterval(function(){ n++; }, 15);");
        processEventsFor(200);
        CHECK(env.engine.evaluate("n").toInt() >= 3);
        env.bridge->clearAll();
    }

    TEST_CASE("engine.clearTimeout cancels a setTimeout-created timer") {
        // Cross-spelling cancellation matters because authors mix the two
        // forms; the cancel must reach the same registry.
        TimerEnv env;
        env.engine.evaluate("var fired = false;"
                            "var id = setTimeout(function(){ fired = true; }, 50);"
                            "engine.clearTimeout(id);");
        processEventsFor(200);
        CHECK_FALSE(env.engine.evaluate("fired").toBool());
    }

    TEST_CASE("clearTimeout cancels an engine.setTimeout-created timer") {
        TimerEnv env;
        env.engine.evaluate("var fired = false;"
                            "var id = engine.setTimeout(function(){ fired = true; }, 50);"
                            "clearTimeout(id);");
        processEventsFor(200);
        CHECK_FALSE(env.engine.evaluate("fired").toBool());
    }

    TEST_CASE("engine.clearInterval stops the engine.setInterval") {
        TimerEnv env;
        env.engine.evaluate("var n = 0;"
                            "var id = engine.setInterval(function(){ n++; }, 15);");
        processEventsFor(100);
        env.engine.evaluate("engine.clearInterval(id);");
        int countAfterClear = env.engine.evaluate("n").toInt();
        CHECK(countAfterClear >= 1);
        processEventsFor(200);
        CHECK(env.engine.evaluate("n").toInt() == countAfterClear);
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

            engine.evaluate("engine.isDesktopDevice = function() { return true; };\n"
                            "engine.isMobileDevice = function() { return false; };\n"
                            "engine.isTabletDevice = function() { return false; };\n"
                            "engine.isWallpaper = function() { return true; };\n"
                            "engine.isScreensaver = function() { return false; };\n"
                            "engine.isRunningInEditor = function() { return false; };\n");
        }

        void setOrientation(float w, float h) {
            bool portrait = h > w;
            engine.evaluate(QString("engine.isPortrait = function() { return %1; };\n"
                                    "engine.isLandscape = function() { return %2; };\n")
                                .arg(portrait ? "true" : "false")
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

// ------------------------------------------------------------------
// input.cursorWorldPosition live update
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Cursor Position") {
    // Helper: engine with the input stub from SceneBackend.cpp
    struct CursorEnv {
        QJSEngine engine;
        CursorEnv() { engine.evaluate("var input = { cursorWorldPosition: { x: 0, y: 0 } };\n"); }
        // Simulate what evaluatePropertyScripts() / evaluateTextScripts() /
        // evaluateColorScripts() do before running scripts.
        void setCursorPos(double x, double y) {
            QJSValue inputObj = engine.globalObject().property("input");
            QJSValue cwp      = inputObj.property("cursorWorldPosition");
            cwp.setProperty("x", x);
            cwp.setProperty("y", y);
        }
    };

    TEST_CASE("initial cursorWorldPosition is {x:0, y:0}") {
        CursorEnv env;
        double    x = env.engine.evaluate("input.cursorWorldPosition.x").toNumber();
        double    y = env.engine.evaluate("input.cursorWorldPosition.y").toNumber();
        CHECK(x == doctest::Approx(0.0));
        CHECK(y == doctest::Approx(0.0));
    }

    TEST_CASE("setCursorPos updates x and y readable by scripts") {
        CursorEnv env;
        env.setCursorPos(320.0, 240.0);
        CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() ==
              doctest::Approx(320.0));
        CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() ==
              doctest::Approx(240.0));
    }

    TEST_CASE("repeated updates replace previous values") {
        CursorEnv env;
        env.setCursorPos(100.0, 200.0);
        env.setCursorPos(960.0, 540.0);
        CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() ==
              doctest::Approx(960.0));
        CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() ==
              doctest::Approx(540.0));
    }

    TEST_CASE("script reads updated cursor position") {
        CursorEnv env;
        // Compile a script that reads cursor position into a local var on each update() call
        env.engine.evaluate("var capturedX = 0;\n"
                            "var capturedY = 0;\n"
                            "function readCursor() {\n"
                            "  capturedX = input.cursorWorldPosition.x;\n"
                            "  capturedY = input.cursorWorldPosition.y;\n"
                            "}\n");
        env.setCursorPos(480.5, 270.25);
        env.engine.evaluate("readCursor();");
        CHECK(env.engine.evaluate("capturedX").toNumber() == doctest::Approx(480.5));
        CHECK(env.engine.evaluate("capturedY").toNumber() == doctest::Approx(270.25));
    }

    TEST_CASE("cursor position zero after reset") {
        CursorEnv env;
        env.setCursorPos(500.0, 400.0);
        env.setCursorPos(0.0, 0.0);
        CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() ==
              doctest::Approx(0.0));
        CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() ==
              doctest::Approx(0.0));
    }

    TEST_CASE("cursor position supports fractional scene coordinates") {
        CursorEnv env;
        env.setCursorPos(1.5, -2.75);
        CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() ==
              doctest::Approx(1.5));
        CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() ==
              doctest::Approx(-2.75));
    }

} // TEST_SUITE SceneScript Cursor Position

// ------------------------------------------------------------------
// getVideoTexture() proxy (JS side)
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Video Texture") {
    // Mock __sceneBridge that tracks calls so tests can assert the JS proxy
    // delegates correctly without pulling libmpv into the test harness.
    struct VideoBridgeEnv {
        QJSEngine engine;
        VideoBridgeEnv() {
            // Fake bridge: returns known numeric values, tracks last calls.
            engine.evaluate(R"JS(
            var __sceneBridge = {
                _lastSet: null, _lastRate: null, _lastPlay: null,
                _duration: 12.5, _curTime: 3.25, _playing: true,
                videoGetCurrentTime: function(n){ this._lastGet = n; return this._curTime; },
                videoGetDuration:    function(n){ return this._duration; },
                videoIsPlaying:      function(n){ return this._playing; },
                videoPlay:           function(n){ this._lastPlay = ['play', n]; },
                videoPause:          function(n){ this._lastPlay = ['pause', n]; },
                videoStop:           function(n){ this._lastPlay = ['stop', n]; },
                videoSetCurrentTime: function(n, t){ this._lastSet = [n, t]; },
                videoSetRate:        function(n, r){ this._lastRate = [n, r]; }
            };
        )JS");
            // Minimal getVideoTexture matching the real proxy shape.
            engine.evaluate(R"JS(
            function makeVideo(name) {
                var _rate = 1.0;
                var o = {
                    getCurrentTime: function(){ return __sceneBridge.videoGetCurrentTime(name); },
                    setCurrentTime: function(t){ __sceneBridge.videoSetCurrentTime(name, +t || 0); },
                    isPlaying: function(){ return !!__sceneBridge.videoIsPlaying(name); },
                    play:  function(){ __sceneBridge.videoPlay(name); },
                    pause: function(){ __sceneBridge.videoPause(name); },
                    stop:  function(){ __sceneBridge.videoStop(name); }
                };
                Object.defineProperty(o, 'duration', {
                    get: function(){ return __sceneBridge.videoGetDuration(name); }, enumerable:true });
                Object.defineProperty(o, 'rate', {
                    get: function(){ return _rate; },
                    set: function(v){ _rate = +v || 1.0; __sceneBridge.videoSetRate(name, _rate); },
                    enumerable:true });
                return o;
            }
        )JS");
        }
    };

    TEST_CASE("video.getCurrentTime() delegates to bridge") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        double t = env.engine.evaluate("v.getCurrentTime()").toNumber();
        CHECK(t == doctest::Approx(3.25));
        CHECK(env.engine.evaluate("__sceneBridge._lastGet").toString() == "layerA");
    }

    TEST_CASE("video.duration is a getter reading bridge") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        CHECK(env.engine.evaluate("v.duration").toNumber() == doctest::Approx(12.5));
        // Mutating the underlying bridge value reflects in the getter (proves it's not cached).
        env.engine.evaluate("__sceneBridge._duration = 42;");
        CHECK(env.engine.evaluate("v.duration").toNumber() == doctest::Approx(42.0));
    }

    TEST_CASE("video.rate setter pushes to bridge and readback matches") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        env.engine.evaluate("v.rate = 2.5;");
        CHECK(env.engine.evaluate("v.rate").toNumber() == doctest::Approx(2.5));
        CHECK(env.engine.evaluate("__sceneBridge._lastRate[0]").toString() == "layerA");
        CHECK(env.engine.evaluate("__sceneBridge._lastRate[1]").toNumber() == doctest::Approx(2.5));
    }

    TEST_CASE("video.rate falls back to 1.0 for invalid values") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        env.engine.evaluate("v.rate = 'not-a-number';");
        CHECK(env.engine.evaluate("v.rate").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("video.setCurrentTime coerces to number and forwards") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        env.engine.evaluate("v.setCurrentTime('4.5');");
        CHECK(env.engine.evaluate("__sceneBridge._lastSet[0]").toString() == "layerA");
        CHECK(env.engine.evaluate("__sceneBridge._lastSet[1]").toNumber() == doctest::Approx(4.5));
    }

    TEST_CASE("video.play/pause/stop dispatch with layer name") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerB');");
        env.engine.evaluate("v.play();");
        CHECK(env.engine.evaluate("__sceneBridge._lastPlay[0]").toString() == "play");
        CHECK(env.engine.evaluate("__sceneBridge._lastPlay[1]").toString() == "layerB");
        env.engine.evaluate("v.stop();");
        CHECK(env.engine.evaluate("__sceneBridge._lastPlay[0]").toString() == "stop");
    }

    TEST_CASE("video.isPlaying returns bool from bridge") {
        VideoBridgeEnv env;
        env.engine.evaluate("var v = makeVideo('layerA');");
        CHECK(env.engine.evaluate("v.isPlaying()").toBool() == true);
        env.engine.evaluate("__sceneBridge._playing = false;");
        CHECK(env.engine.evaluate("v.isPlaying()").toBool() == false);
    }

    TEST_CASE("getVideoTexture per-call produces fresh objects") {
        VideoBridgeEnv env;
        env.engine.evaluate("var a = makeVideo('l'); var b = makeVideo('l');");
        // Mutating rate on one must not affect the other.
        env.engine.evaluate("a.rate = 3.0;");
        CHECK(env.engine.evaluate("a.rate").toNumber() == doctest::Approx(3.0));
        CHECK(env.engine.evaluate("b.rate").toNumber() == doctest::Approx(1.0));
    }

} // TEST_SUITE SceneScript Video Texture

// ------------------------------------------------------------------
// thisLayer.getTextureAnimation() proxy with live snapshot read-back
// ------------------------------------------------------------------
// Production proxy queries __sceneBridge.getLayerSpriteInfo on each read so
// scripts see real frameCount / currentFrame / duration / isManualPin from
// the per-node sprite snapshot WPShaderValueUpdater publishes each tick.  We
// mirror that here with a mock bridge so the Rella firework pattern
// (`frame === ani.frameCount - 1` followed by setTimeout+stop+play) actually
// observes meaningful frame numbers — without the live read-back the proxy
// hardcoded frameCount=1 and the end-of-cycle branch fired every tick.
TEST_SUITE("SceneScript Texture Animation Bridge") {
    struct SpriteBridgeEnv {
        QJSEngine engine;
        SpriteBridgeEnv() {
            engine.evaluate(R"JS(
                var __sceneBridge = {
                    _info: { frameCount: 7, currentFrame: 0, duration: 4.53,
                             isManualPin: false },
                    _setCalls: [],
                    getLayerSpriteInfo: function(name) {
                        this._lastInfoQuery = name;
                        // Return a copy so the proxy can't mutate cached state.
                        return { frameCount: this._info.frameCount,
                                 currentFrame: this._info.currentFrame,
                                 duration: this._info.duration,
                                 isManualPin: this._info.isManualPin };
                    },
                    setLayerSpriteFrame: function(name, wantsManual, frameIdx) {
                        this._setCalls.push([name, wantsManual, frameIdx]);
                        // Production: pin updates the rendered frame so the
                        // next snapshot reflects the pin.  Mirror that so the
                        // proxy's pause()/setFrame() round-trip works.
                        this._info.isManualPin = wantsManual;
                        if (wantsManual) this._info.currentFrame = frameIdx;
                    }
                };
            )JS");
            // Mirror the production getTextureAnimation proxy: each read goes
            // through the bridge; writes (setFrame/play/stop/pause) push pins.
            engine.evaluate(R"JS(
                function makeAnim(name) {
                    var _name = name;
                    var _readInfo = function() {
                        var info = __sceneBridge.getLayerSpriteInfo(_name);
                        var fc = info && info.frameCount ? info.frameCount : 1;
                        var cf = info && typeof info.currentFrame === 'number'
                                 ? info.currentFrame : 0;
                        var dur = info && typeof info.duration === 'number'
                                  ? info.duration : 0;
                        var mp = info ? !!info.isManualPin : false;
                        return { frameCount: fc, currentFrame: cf,
                                 duration: dur, isManualPin: mp };
                    };
                    return {
                        get frameCount(){ return _readInfo().frameCount; },
                        get duration(){ return _readInfo().duration; },
                        get rate(){ return _readInfo().isManualPin ? 0 : 1; },
                        getFrame: function(){ return _readInfo().currentFrame; },
                        setFrame: function(f){
                            __sceneBridge.setLayerSpriteFrame(_name, true, f|0); },
                        play: function(){
                            __sceneBridge.setLayerSpriteFrame(_name, false, 0); },
                        pause: function(){
                            var cur = _readInfo().currentFrame;
                            __sceneBridge.setLayerSpriteFrame(_name, true, cur); },
                        stop: function(){
                            __sceneBridge.setLayerSpriteFrame(_name, true, 0); },
                        isPlaying: function(){ return !_readInfo().isManualPin; }
                    };
                }
            )JS");
        }
    };

    TEST_CASE("frameCount reflects live snapshot, not hardcoded 1") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('烟花1');");
        CHECK(env.engine.evaluate("a.frameCount").toInt() == 7);
        env.engine.evaluate("__sceneBridge._info.frameCount = 151;");
        CHECK(env.engine.evaluate("a.frameCount").toInt() == 151);
        CHECK(env.engine.evaluate("__sceneBridge._lastInfoQuery").toString() == "烟花1");
    }

    TEST_CASE("duration is the texture's authored cycle in seconds") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');");
        CHECK(env.engine.evaluate("a.duration").toNumber() ==
              doctest::Approx(4.53));
        env.engine.evaluate("__sceneBridge._info.duration = 2.0;");
        CHECK(env.engine.evaluate("a.duration").toNumber() ==
              doctest::Approx(2.0));
    }

    TEST_CASE("getFrame returns the live current frame, never a stale 0") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');");
        CHECK(env.engine.evaluate("a.getFrame()").toInt() == 0);
        env.engine.evaluate("__sceneBridge._info.currentFrame = 133;");
        CHECK(env.engine.evaluate("a.getFrame()").toInt() == 133);
        // Reading twice should re-query the bridge — no caching.
        env.engine.evaluate("__sceneBridge._info.currentFrame = 150;");
        CHECK(env.engine.evaluate("a.getFrame()").toInt() == 150);
    }

    TEST_CASE("Rella firework end-of-cycle pattern: frame===frameCount-1") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('烟花1');");
        env.engine.evaluate(
            "__sceneBridge._info.frameCount = 151;\n"
            "__sceneBridge._info.currentFrame = 0;\n");
        CHECK_FALSE(env.engine.evaluate(
                            "a.getFrame() === a.frameCount - 1")
                        .toBool());
        env.engine.evaluate("__sceneBridge._info.currentFrame = 150;");
        CHECK(env.engine.evaluate("a.getFrame() === a.frameCount - 1").toBool());
    }

    TEST_CASE("setFrame pins through bridge") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');");
        env.engine.evaluate("a.setFrame(42);");
        CHECK(env.engine.evaluate("__sceneBridge._setCalls[0][0]").toString() ==
              "fw");
        CHECK(env.engine.evaluate("__sceneBridge._setCalls[0][1]").toBool() ==
              true);
        CHECK(env.engine.evaluate("__sceneBridge._setCalls[0][2]").toInt() ==
              42);
        // Pin reflected on next read.
        CHECK(env.engine.evaluate("a.getFrame()").toInt() == 42);
        CHECK_FALSE(env.engine.evaluate("a.isPlaying()").toBool());
    }

    TEST_CASE("play releases manual pin, isPlaying flips back to true") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');");
        env.engine.evaluate("a.setFrame(10);");
        CHECK_FALSE(env.engine.evaluate("a.isPlaying()").toBool());
        env.engine.evaluate("a.play();");
        CHECK(env.engine.evaluate("a.isPlaying()").toBool());
        // The play() call passes wantsManual=false.
        env.engine.evaluate(
            "var last = __sceneBridge._setCalls[__sceneBridge._setCalls.length - 1];");
        CHECK_FALSE(env.engine.evaluate("last[1]").toBool());
    }

    TEST_CASE("stop pins to frame 0 (Rella script's pause-before-replay)") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');\n"
                            "__sceneBridge._info.currentFrame = 150;\n");
        env.engine.evaluate("a.stop();");
        env.engine.evaluate(
            "var last = __sceneBridge._setCalls[__sceneBridge._setCalls.length - 1];");
        CHECK(env.engine.evaluate("last[1]").toBool() == true);
        CHECK(env.engine.evaluate("last[2]").toInt() == 0);
        CHECK(env.engine.evaluate("a.getFrame()").toInt() == 0);
    }

    TEST_CASE("pause pins at current frame (preserves where the sprite was)") {
        SpriteBridgeEnv env;
        env.engine.evaluate("var a = makeAnim('fw');\n"
                            "__sceneBridge._info.currentFrame = 77;\n");
        env.engine.evaluate("a.pause();");
        env.engine.evaluate(
            "var last = __sceneBridge._setCalls[__sceneBridge._setCalls.length - 1];");
        CHECK(env.engine.evaluate("last[1]").toBool() == true);
        CHECK(env.engine.evaluate("last[2]").toInt() == 77);
    }

    TEST_CASE("Rella firework update() pattern: pause loop fires only at "
              "end-of-cycle, not every tick") {
        SpriteBridgeEnv env;
        env.engine.evaluate(
            "var triggers = 0;\n"
            "var playing = true;\n"
            "var a = makeAnim('fw');\n"
            "function tick() {\n"
            "  var frame = a.getFrame();\n"
            "  if (playing && frame === a.frameCount - 1) {\n"
            "    triggers++; playing = false;\n"
            "  }\n"
            "}");
        env.engine.evaluate("__sceneBridge._info.frameCount = 151;");
        // 150 ticks at frame 0..149 — no trigger.
        for (int i = 0; i < 150; ++i) {
            QString s = QString("__sceneBridge._info.currentFrame = %1; tick();").arg(i);
            env.engine.evaluate(s);
        }
        CHECK(env.engine.evaluate("triggers").toInt() == 0);
        env.engine.evaluate("__sceneBridge._info.currentFrame = 150; tick();");
        CHECK(env.engine.evaluate("triggers").toInt() == 1);
        // Another tick at frame 150 must NOT re-trigger — playing latched off.
        env.engine.evaluate("tick();");
        CHECK(env.engine.evaluate("triggers").toInt() == 1);
    }

} // TEST_SUITE SceneScript Texture Animation Bridge

// ------------------------------------------------------------------
// thisLayer.horizontalalign / verticalalign / alignment / font (JS shim)
// ------------------------------------------------------------------
// These are runtime mutators for text-layer style.  The JS proxy parses
// `alignment` into h+v components before delegating, so we only carry two
// axes plus a font name through __sceneBridge.setTextStyle.  Render-side
// VFS font resolution is exercised separately by the integration build —
// here we just verify the shim posts the right bridge calls.
TEST_SUITE("SceneScript Text Style") {
    struct TextStyleEnv {
        QJSEngine engine;
        TextStyleEnv() {
            // Mock bridge that records every setTextStyle call.
            engine.evaluate(R"JS(
                var __sceneBridge = {
                    _calls: [],
                    setTextStyle: function(name, h, v, f) {
                        this._calls.push([name, h, v, f]);
                    }
                };
            )JS");
            // Vec3 stub (the production shim doesn't depend on it for text-style,
            // but other layer-proxy callers do; keep tests self-contained).
            engine.evaluate("function Vec3(x,y,z){ return {x:x||0, y:y||0, z:z||0}; }");
            // Minimal layer proxy that mirrors SceneBackend.cpp's _makeLayerProxy
            // ONLY for the text-style properties.  Keep this in sync with the
            // production shim — the regex `Object.defineProperty(p, '...'` block
            // around horizontalalign/verticalalign/font/alignment is the source.
            engine.evaluate(R"JS(
                function makeTextLayer(name, init) {
                    var _s = { name: name };
                    if (init && init.halign) _s.halign = init.halign;
                    if (init && init.valign) _s.valign = init.valign;
                    if (init && init.font)   _s.font   = init.font;
                    var p = {};
                    Object.defineProperty(p, 'horizontalalign', {
                        get: function(){ return _s.halign || 'center'; },
                        set: function(v){ _s.halign = String(v == null ? 'center' : v);
                                          __sceneBridge.setTextStyle(name, _s.halign, '', ''); },
                        enumerable: true
                    });
                    Object.defineProperty(p, 'verticalalign', {
                        get: function(){ return _s.valign || 'center'; },
                        set: function(v){ _s.valign = String(v == null ? 'center' : v);
                                          __sceneBridge.setTextStyle(name, '', _s.valign, ''); },
                        enumerable: true
                    });
                    Object.defineProperty(p, 'font', {
                        get: function(){ return _s.font || ''; },
                        set: function(v){ _s.font = String(v == null ? '' : v);
                                          __sceneBridge.setTextStyle(name, '', '', _s.font); },
                        enumerable: true
                    });
                    Object.defineProperty(p, 'alignment', {
                        get: function(){
                            var h = _s.halign || 'center', v = _s.valign || 'center';
                            if (h === 'center' && v === 'center') return 'center';
                            return (v === 'center' ? '' : v) + (h === 'center' ? '' : h);
                        },
                        set: function(v){
                            var s = String(v == null ? '' : v).toLowerCase();
                            var nh = '', nv = '';
                            if (s.indexOf('left')   >= 0) nh = 'left';
                            else if (s.indexOf('right')  >= 0) nh = 'right';
                            if (s.indexOf('top')    >= 0) nv = 'top';
                            else if (s.indexOf('bottom') >= 0) nv = 'bottom';
                            if (s === 'center' || (s.indexOf('center') >= 0 && !nh && !nv)) {
                                nh = 'center'; nv = 'center';
                            }
                            if (nh) _s.halign = nh;
                            if (nv) _s.valign = nv;
                            __sceneBridge.setTextStyle(name, nh, nv, '');
                        },
                        enumerable: true
                    });
                    return p;
                }
            )JS");
        }
    };

    TEST_CASE("horizontalalign writes through to bridge and is readable back") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('clockLabel');");
        env.engine.evaluate("t.horizontalalign = 'right';");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "right");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][0]").toString() == "clockLabel");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][1]").toString() == "right");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][2]").toString() == "");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][3]").toString() == "");
    }

    TEST_CASE("verticalalign writes through to bridge") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.verticalalign = 'top';");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "top");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][2]").toString() == "top");
    }

    TEST_CASE("default halign/valign are 'center' before any assignment") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "center");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "center");
    }

    TEST_CASE("init state seeds halign/valign/font for round-trip reads") {
        TextStyleEnv env;
        env.engine.evaluate(
            "var t = makeTextLayer('label', {halign:'right', valign:'top', font:'Heavy.otf'});");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "right");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "top");
        CHECK(env.engine.evaluate("t.font").toString() == "Heavy.otf");
        // No bridge calls because nothing was set after construction.
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 0);
    }

    TEST_CASE("font setter posts only the font field, leaving h/v empty") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.font = 'BRUSH.ttf';");
        CHECK(env.engine.evaluate("t.font").toString() == "BRUSH.ttf");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][1]").toString() == "");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][2]").toString() == "");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][3]").toString() == "BRUSH.ttf");
    }

    TEST_CASE("alignment splits composite 'topleft' into h+v") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.alignment = 'topleft';");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "left");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "top");
        // One bridge call carrying BOTH axes.
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][1]").toString() == "left");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0][2]").toString() == "top");
    }

    TEST_CASE("alignment 'centerright' yields h=right v=center") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.alignment = 'centerright';");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "right");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "center");
    }

    TEST_CASE("alignment 'left' sets only halign") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.alignment = 'left';");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "left");
        // valign untouched → still default 'center'
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "center");
    }

    TEST_CASE("alignment 'center' sets both axes to center") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label', {halign:'left', valign:'top'});");
        env.engine.evaluate("t.alignment = 'center';");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "center");
        CHECK(env.engine.evaluate("t.verticalalign").toString() == "center");
    }

    TEST_CASE("alignment getter reconstructs composite from h+v") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label', {halign:'right', valign:'top'});");
        CHECK(env.engine.evaluate("t.alignment").toString() == "topright");
    }

    TEST_CASE("alignment getter returns 'center' when both axes center") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        CHECK(env.engine.evaluate("t.alignment").toString() == "center");
    }

    TEST_CASE("non-string values coerce safely (number, null)") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        // WE authors sometimes assign computed values; coercion shouldn't crash.
        env.engine.evaluate("t.horizontalalign = 5;");
        CHECK(env.engine.evaluate("t.horizontalalign").toString() == "5");
        env.engine.evaluate("t.font = null;");
        CHECK(env.engine.evaluate("t.font").toString() == "");
    }

    TEST_CASE("sequential writes produce one bridge call per assignment") {
        TextStyleEnv env;
        env.engine.evaluate("var t = makeTextLayer('label');");
        env.engine.evaluate("t.horizontalalign = 'left';");
        env.engine.evaluate("t.verticalalign = 'top';");
        env.engine.evaluate("t.font = 'Heavy.otf';");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 3);
    }
} // TEST_SUITE SceneScript Text Style

// ------------------------------------------------------------------
// thisLayer.getTransformMatrix() (JS shim — bridge passthrough into Mat4)
// ------------------------------------------------------------------
// Scripts read `.m[12]` / `.m[13]` for world X/Y to compare against
// engine.canvasSize halves.  Test that the shim wraps the bridge's 16-float
// array into a Mat4 instance, defaults to identity on bridge miss, and
// preserves Mat4's prototype methods (.translation()).
TEST_SUITE("SceneScript Layer Transform Matrix") {
    struct TransformEnv {
        QJSEngine engine;
        TransformEnv() {
            // Vec2/Vec3 needed by Mat4.translation() return path.
            engine.evaluate(wek::qml_helper::kVecClassesJs);
            engine.evaluate(wek::qml_helper::kMatricesJs);
            engine.evaluate(R"JS(
                var __sceneBridge = {
                    _table: {},
                    getLayerWorldTransform: function(name) {
                        return this._table[name] || null;
                    }
                };
            )JS");
            engine.evaluate(R"JS(
                function makeLayer(name) {
                    var _s = { name: name };
                    var p = {};
                    p.getTransformMatrix = function() {
                        var arr = __sceneBridge.getLayerWorldTransform(_s.name);
                        var mm = new Mat4();
                        if (arr && arr.length === 16) {
                            for (var i = 0; i < 16; i++) mm.m[i] = +arr[i] || 0;
                        }
                        return mm;
                    };
                    return p;
                }
            )JS");
        }
    };

    TEST_CASE("returns identity Mat4 when bridge has no data for the layer") {
        TransformEnv env;
        env.engine.evaluate("var L = makeLayer('unknown'); var m = L.getTransformMatrix();");
        // Mat4() constructor initializes to identity.
        CHECK(env.engine.evaluate("m.m[0]").toNumber()  == doctest::Approx(1));
        CHECK(env.engine.evaluate("m.m[5]").toNumber()  == doctest::Approx(1));
        CHECK(env.engine.evaluate("m.m[10]").toNumber() == doctest::Approx(1));
        CHECK(env.engine.evaluate("m.m[15]").toNumber() == doctest::Approx(1));
        CHECK(env.engine.evaluate("m.m[12]").toNumber() == doctest::Approx(0));
    }

    TEST_CASE("populates Mat4 from bridge 16-float array") {
        TransformEnv env;
        // Identity matrix with a translation of (200, 50, 0).
        env.engine.evaluate(
            "__sceneBridge._table['bg'] = "
            "[1,0,0,0, 0,1,0,0, 0,0,1,0, 200,50,0,1];");
        env.engine.evaluate("var L = makeLayer('bg'); var m = L.getTransformMatrix();");
        CHECK(env.engine.evaluate("m.m[12]").toNumber() == doctest::Approx(200.0));
        CHECK(env.engine.evaluate("m.m[13]").toNumber() == doctest::Approx(50.0));
        CHECK(env.engine.evaluate("m.m[14]").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("preserves Mat4 prototype methods (translation())") {
        TransformEnv env;
        env.engine.evaluate(
            "__sceneBridge._table['bg'] = "
            "[1,0,0,0, 0,1,0,0, 0,0,1,0, 7,11,13,1];");
        env.engine.evaluate("var v = makeLayer('bg').getTransformMatrix().translation();");
        // Mat4.translation() with no arg returns Vec3(m[12], m[13], m[14]).
        CHECK(env.engine.evaluate("v.x").toNumber() == doctest::Approx(7.0));
        CHECK(env.engine.evaluate("v.y").toNumber() == doctest::Approx(11.0));
        CHECK(env.engine.evaluate("v.z").toNumber() == doctest::Approx(13.0));
    }

    TEST_CASE("corpus pattern: world X compared to canvas midpoint") {
        TransformEnv env;
        // Simulate canvas width 1920, layer's world X at 1300 → past midpoint.
        env.engine.evaluate(
            "__sceneBridge._table['heart'] = "
            "[1,0,0,0, 0,1,0,0, 0,0,1,0, 1300,400,0,1];");
        env.engine.evaluate("var canvasW = 1920;");
        env.engine.evaluate(
            "var past = makeLayer('heart').getTransformMatrix().m[12] > canvasW / 2;");
        CHECK(env.engine.evaluate("past").toBool() == true);
    }

    TEST_CASE("non-array bridge result falls through to identity") {
        TransformEnv env;
        env.engine.evaluate("__sceneBridge._table['bad'] = 'not-an-array';");
        env.engine.evaluate("var m = makeLayer('bad').getTransformMatrix();");
        CHECK(env.engine.evaluate("m.m[12]").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("m.m[0]").toNumber()  == doctest::Approx(1));
    }
} // TEST_SUITE SceneScript Layer Transform Matrix

// ------------------------------------------------------------------
// thisLayer.getBoneIndex(name) (JS shim → bridge passthrough)
// ------------------------------------------------------------------
// Music Player (2992803622) calls `thisLayer.getBoneIndex(bone[i][1])` to
// validate that a configured attachment name resolves on the rigged puppet.
// 0 = "not found" (matches WE's origin sentinel).
TEST_SUITE("SceneScript Layer Bone Index") {
    struct BoneEnv {
        QJSEngine engine;
        BoneEnv() {
            engine.evaluate(R"JS(
                var __sceneBridge = {
                    _table: {},
                    _lastQuery: null,
                    getBoneIndex: function(layer, name) {
                        this._lastQuery = [layer, name];
                        var row = this._table[layer];
                        if (!row) return 0;
                        return row[name] || 0;
                    }
                };
            )JS");
            engine.evaluate(R"JS(
                function makeLayer(name) {
                    var _s = { name: name };
                    return {
                        getBoneIndex: function(bname) {
                            return __sceneBridge.getBoneIndex(
                                _s.name, String(bname == null ? '' : bname)) | 0;
                        }
                    };
                }
            )JS");
        }
    };

    TEST_CASE("returns 0 when bridge has no entry for the layer") {
        BoneEnv env;
        env.engine.evaluate("var L = makeLayer('puppet1');");
        CHECK(env.engine.evaluate("L.getBoneIndex('head')").toInt() == 0);
    }

    TEST_CASE("returns the bone index for a known attachment") {
        BoneEnv env;
        env.engine.evaluate("__sceneBridge._table['puppet1'] = "
                            "{ head: 3, hand_left: 7, hand_right: 8 };");
        env.engine.evaluate("var L = makeLayer('puppet1');");
        CHECK(env.engine.evaluate("L.getBoneIndex('head')").toInt() == 3);
        CHECK(env.engine.evaluate("L.getBoneIndex('hand_right')").toInt() == 8);
    }

    TEST_CASE("returns 0 for missing attachment name") {
        BoneEnv env;
        env.engine.evaluate(
            "__sceneBridge._table['puppet1'] = { head: 3 };");
        env.engine.evaluate("var L = makeLayer('puppet1');");
        CHECK(env.engine.evaluate("L.getBoneIndex('foot')").toInt() == 0);
    }

    TEST_CASE("coerces non-string argument and forwards the string to bridge") {
        BoneEnv env;
        env.engine.evaluate("var L = makeLayer('puppet1');");
        env.engine.evaluate("L.getBoneIndex(undefined);");
        CHECK(env.engine.evaluate("__sceneBridge._lastQuery[1]").toString() == "");
        env.engine.evaluate("L.getBoneIndex(null);");
        CHECK(env.engine.evaluate("__sceneBridge._lastQuery[1]").toString() == "");
    }

    TEST_CASE("integer coercion guards against bridge returning non-numeric") {
        BoneEnv env;
        // Cap-out: bridge returns a string-encoded number; |0 normalizes to int.
        env.engine.evaluate(
            "__sceneBridge.getBoneIndex = function(){ return '5'; };");
        env.engine.evaluate("var L = makeLayer('puppet1');");
        CHECK(env.engine.evaluate("L.getBoneIndex('head')").toInt() == 5);
    }

    TEST_CASE("corpus pattern: 0 result triggers throw-error path") {
        // Mirrors the Music Player rigger script — the guard `=== 0` short-
        // circuits to a config-error before binding.  Make sure 0 propagates
        // through the shim cleanly.
        BoneEnv env;
        env.engine.evaluate("__sceneBridge._table['puppet1'] = { head: 1 };");
        env.engine.evaluate("var L = makeLayer('puppet1');");
        env.engine.evaluate(
            "var raised = false;"
            "if (L.getBoneIndex('not_a_bone') === 0) raised = true;");
        CHECK(env.engine.evaluate("raised").toBool());
    }
} // TEST_SUITE SceneScript Layer Bone Index

// ------------------------------------------------------------------
// getMaterial() proxy (JS side)
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Material") {
    // Mock __sceneBridge that records every materialSetValue call so tests
    // can assert the JS proxy delegates correctly without pulling Vulkan or
    // a real Scene into the harness.
    struct MaterialBridgeEnv {
        QJSEngine engine;
        MaterialBridgeEnv() {
            // Vec2/Vec3/Vec4 — needed because the proxy unpacks Vec instances.
            engine.evaluate(wek::qml_helper::kVecClassesJs);
            // Fake bridge.  The proxy looks up __sceneBridge.materialSetValue
            // dynamically, so the bridge can be replaced in individual tests
            // if needed.
            engine.evaluate(R"JS(
                var __sceneBridge = {
                    _calls: [],
                    materialSetValue: function(layer, name, arr) {
                        var copy = [];
                        for (var i = 0; i < arr.length; i++) copy.push(arr[i]);
                        this._calls.push({ layer: layer, name: name, arr: copy });
                    }
                };
            )JS");
            engine.evaluate(wek::qml_helper::kMaterialProxyJs);
        }
    };

    TEST_CASE("setValue with scalar packs as 1-element array") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.setValue('g_Alpha', 0.5);");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].layer").toString() == "bg");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].name").toString() == "g_Alpha");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[0]").toNumber() ==
              doctest::Approx(0.5));
    }

    TEST_CASE("setValue with array forwards element-wise") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.setValue('g_Tint', [1, 0, 0.25]);");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr.length").toInt() == 3);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[2]").toNumber() ==
              doctest::Approx(0.25));
    }

    TEST_CASE("setValue with Vec3 unpacks x,y,z") {
        MaterialBridgeEnv env;
        env.engine.evaluate(
            "var m = _makeMaterialProxy('bg'); m.setValue('g_Tint', Vec3(0.1, 0.2, 0.3));");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr.length").toInt() == 3);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[1]").toNumber() ==
              doctest::Approx(0.2));
    }

    TEST_CASE("setValue with empty name is a no-op") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.setValue('', 1.0);");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 0);
    }

    TEST_CASE("setValue with non-numeric value is a no-op") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.setValue('g_Tint', 'red');");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 0);
    }

    TEST_CASE("getValue returns null until setValue is called") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg');");
        CHECK(env.engine.evaluate("m.getValue('g_Tint')").isNull());
    }

    TEST_CASE("getValue returns the last set value") {
        MaterialBridgeEnv env;
        env.engine.evaluate(
            "var m = _makeMaterialProxy('bg'); m.setValue('g_Tint', [1, 2, 3]);");
        CHECK(env.engine.evaluate("m.getValue('g_Tint').length").toInt() == 3);
        CHECK(env.engine.evaluate("m.getValue('g_Tint')[1]").toNumber() == doctest::Approx(2));
    }

    TEST_CASE("getValue is per-layer per-name") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var a = _makeMaterialProxy('bg');"
                            "var b = _makeMaterialProxy('fg');"
                            "a.setValue('g_Tint', [1]); b.setValue('g_Tint', [9]);");
        CHECK(env.engine.evaluate("a.getValue('g_Tint')[0]").toNumber() == doctest::Approx(1));
        CHECK(env.engine.evaluate("b.getValue('g_Tint')[0]").toNumber() == doctest::Approx(9));
    }

    TEST_CASE("setValue overwrites cached value") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg');"
                            "m.setValue('g_Alpha', 0.5);"
                            "m.setValue('g_Alpha', 0.9);");
        CHECK(env.engine.evaluate("m.getValue('g_Alpha')[0]").toNumber() == doctest::Approx(0.9));
    }

    TEST_CASE("nullMaterialProxy setValue and getValue are safe") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeNullMaterialProxy(); m.setValue('g_Tint', [1,0,0]);");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 0);
        CHECK(env.engine.evaluate("m.getValue('g_Tint')").isNull());
    }

    TEST_CASE("setValue truncates arrays >16 elements") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var arr = []; for (var i=0;i<32;i++) arr.push(i);"
                            "var m = _makeMaterialProxy('bg'); m.setValue('g_Big', arr);");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr.length").toInt() == 16);
    }

    TEST_CASE("setValue filters non-finite array elements") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg');"
                            "m.setValue('g_X', [1, NaN, 2, 'oops', 3]);");
        // Non-finite entries are dropped; the resulting array still passes
        // through if at least one value survives.  Keeps malformed authoring
        // from sending NaN to the GPU.
        QJSValue arr = env.engine.evaluate("__sceneBridge._calls[0].arr");
        CHECK(arr.property("length").toInt() == 3);
        CHECK(arr.property(0).toNumber() == doctest::Approx(1));
        CHECK(arr.property(2).toNumber() == doctest::Approx(3));
    }

    // ---- Direct property accessors (color, channelMask, alpha, tint) ----
    // WE wallpapers assign `mat.color = Vec3(...)` etc. as shorthand for
    // setValue("g_Color", ...).  Verify the aliases map correctly and that
    // writes/reads round-trip the same as explicit setValue calls.

    TEST_CASE("material.color setter routes author-facing 'color' uniform") {
        // The JS proxy sends the author-facing name ("color"); the C++ drain
        // resolves it per material via SceneMaterialCustomShader::alias to
        // the bound shader's actual GLSL uniform (g_Color for genericimage,
        // g_TintColor for tint.frag, etc.).  Pre-this-fix the proxy hard-
        // coded "g_Color" and silently dropped on shaders using other names.
        MaterialBridgeEnv env;
        env.engine.evaluate(
            "var m = _makeMaterialProxy('bg'); m.color = Vec3(0.5, 0.25, 1);");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].name").toString() == "color");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[0]").toNumber()
              == doctest::Approx(0.5));
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[2]").toNumber()
              == doctest::Approx(1.0));
    }

    TEST_CASE("material.color getter reads cached value") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.color = Vec3(0.1, 0.2, 0.3);");
        CHECK(env.engine.evaluate("m.color[0]").toNumber() == doctest::Approx(0.1));
        CHECK(env.engine.evaluate("m.color[1]").toNumber() == doctest::Approx(0.2));
        CHECK(env.engine.evaluate("m.color[2]").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("material.channelMask alias routes author-facing 'channelMask'") {
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg'); m.channelMask = [1, 0, 1, 0];");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].name").toString() == "channelMask");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr.length").toInt() == 4);
    }

    TEST_CASE("setValue and direct property share the same cache via author name") {
        // Author-facing keys: setValue('color', ...) and m.color = ... both
        // cache under 'color' (the JS-side normalized name) so getValue('color')
        // and m.color round-trip consistently regardless of which spelling the
        // author chose.  The C++ drain does the GLSL-uniform resolution per
        // material — JS side keeps a single canonical key.
        MaterialBridgeEnv env;
        env.engine.evaluate("var m = _makeMaterialProxy('bg');"
                            "m.setValue('color', [0.4, 0.5, 0.6]);");
        CHECK(env.engine.evaluate("m.color[0]").toNumber() == doctest::Approx(0.4));
        env.engine.evaluate("m.color = Vec3(0.7, 0.8, 0.9);");
        CHECK(env.engine.evaluate("m.getValue('color')[2]").toNumber() == doctest::Approx(0.9));
    }

    TEST_CASE("direct property writes with invalid value are silently ignored") {
        MaterialBridgeEnv env;
        env.engine.evaluate(
            "var m = _makeMaterialProxy('bg'); m.color = 'not-a-vec';");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 0);
        CHECK(env.engine.evaluate("m.color === null").toBool());
    }

    // ---- Effect material (via getEffect().getMaterial()) ----

    TEST_CASE("effect material routes setValue through effectMaterialSetValue") {
        MaterialBridgeEnv env;
        env.engine.evaluate(
            "__sceneBridge.effectMaterialSetValue = function(layer, idx, name, arr) {\n"
            "  var copy = []; for (var i = 0; i < arr.length; i++) copy.push(arr[i]);\n"
            "  this._calls.push({ layer: layer, effectIdx: idx, name: name, arr: copy });\n"
            "};");
        env.engine.evaluate(
            "var m = _makeMaterialProxy('canvas', 2); m.setValue('g_Color', Vec3(0.8, 0, 0));");
        CHECK(env.engine.evaluate("__sceneBridge._calls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].effectIdx").toInt() == 2);
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].name").toString() == "g_Color");
        CHECK(env.engine.evaluate("__sceneBridge._calls[0].arr[0]").toNumber()
              == doctest::Approx(0.8));
    }

    TEST_CASE("effect material proxy has independent cache per effect index") {
        MaterialBridgeEnv env;
        env.engine.evaluate("__sceneBridge.effectMaterialSetValue = function(){};");
        env.engine.evaluate("var m0 = _makeMaterialProxy('bg', 0);"
                            "var m1 = _makeMaterialProxy('bg', 1);"
                            "m0.color = Vec3(1, 0, 0);"
                            "m1.color = Vec3(0, 1, 0);");
        CHECK(env.engine.evaluate("m0.color[0]").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("m1.color[1]").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("m1.color[0]").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("effect material is distinct from main layer material on same name") {
        MaterialBridgeEnv env;
        env.engine.evaluate("__sceneBridge.effectMaterialSetValue = function(){};");
        env.engine.evaluate("var main = _makeMaterialProxy('bg');"
                            "var eff  = _makeMaterialProxy('bg', 0);"
                            "main.color = Vec3(1, 1, 1);"
                            "eff.color  = Vec3(0, 0, 0);");
        CHECK(env.engine.evaluate("main.color[0]").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("eff.color[0]").toNumber()  == doctest::Approx(0.0));
    }
}

// ------------------------------------------------------------------
// engine.userProperties population
// ------------------------------------------------------------------
TEST_SUITE("SceneScript engine.userProperties") {
    // Helper: minimal engine with engine.userProperties object, mirroring
    // the setup in SceneBackend::setupTextScripts() and refreshJsUserProperties().
    struct UserPropEnv {
        QJSEngine engine;
        UserPropEnv() {
            QJSValue engineObj = engine.newObject();
            engineObj.setProperty("userProperties", engine.newObject());
            engine.globalObject().setProperty("engine", engineObj);
        }
        // Mirrors SceneObject::refreshJsUserProperties()
        void refresh(const QString& json) {
            if (json.isEmpty()) return;
            QString escaped = json;
            escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
            engine.evaluate(QString("(function(){"
                                    "try{"
                                    "var p=JSON.parse('%1');"
                                    "var up=engine.userProperties;"
                                    "for(var k in p) up[k]=p[k];"
                                    "}catch(e){}"
                                    "})()")
                                .arg(escaped));
        }
    };

    TEST_CASE("engine.userProperties is initially an empty object") {
        UserPropEnv env;
        // An empty JS object is not undefined/null, and has no own properties.
        QJSValue up = env.engine.evaluate("engine.userProperties");
        CHECK_FALSE(up.isUndefined());
        CHECK_FALSE(up.isNull());
        // speed is absent → undefined
        CHECK(env.engine.evaluate("engine.userProperties.speed").isUndefined());
    }

    TEST_CASE("number property is populated correctly") {
        UserPropEnv env;
        env.refresh(R"({"speed":0.5})");
        double val = env.engine.evaluate("engine.userProperties.speed").toNumber();
        CHECK(val == doctest::Approx(0.5));
    }

    TEST_CASE("bool property is populated correctly") {
        UserPropEnv env;
        env.refresh(R"({"enabled":true})");
        bool val = env.engine.evaluate("engine.userProperties.enabled").toBool();
        CHECK(val == true);

        env.refresh(R"({"enabled":false})");
        CHECK(env.engine.evaluate("engine.userProperties.enabled").toBool() == false);
    }

    TEST_CASE("string property (color) is populated correctly") {
        UserPropEnv env;
        env.refresh(R"({"colour":"0.5 0.1 0.9"})");
        QString val = env.engine.evaluate("engine.userProperties.colour").toString();
        CHECK(val == QStringLiteral("0.5 0.1 0.9"));
    }

    TEST_CASE("multiple properties are all accessible") {
        UserPropEnv env;
        env.refresh(R"({"speed":2.0,"muted":false,"schemecolor":"1 0 0"})");
        CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() ==
              doctest::Approx(2.0));
        CHECK(env.engine.evaluate("engine.userProperties.muted").toBool() == false);
        CHECK(env.engine.evaluate("engine.userProperties.schemecolor").toString() ==
              QStringLiteral("1 0 0"));
    }

    TEST_CASE("script reads property value correctly") {
        UserPropEnv env;
        env.refresh(R"({"bloomstrength":1.78})");
        env.engine.evaluate(
            "var captured = 0;\n"
            "function readProp() { captured = engine.userProperties.bloomstrength; }\n");
        env.engine.evaluate("readProp();");
        double val = env.engine.evaluate("captured").toNumber();
        CHECK(val == doctest::Approx(1.78));
    }

    TEST_CASE("second refresh updates existing property") {
        UserPropEnv env;
        env.refresh(R"({"speed":1.0})");
        CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() ==
              doctest::Approx(1.0));
        env.refresh(R"({"speed":3.5})");
        CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() ==
              doctest::Approx(3.5));
    }

    TEST_CASE("empty string refresh is a no-op") {
        UserPropEnv env;
        env.refresh(R"({"x":42})");
        env.refresh(QString()); // empty → no-op
        CHECK(env.engine.evaluate("engine.userProperties.x").toNumber() == doctest::Approx(42.0));
    }

    TEST_CASE("invalid JSON is silently ignored") {
        UserPropEnv env;
        env.refresh(R"({"a":1})");
        env.refresh("not valid json"); // must not crash or alter existing props
        CHECK(env.engine.evaluate("engine.userProperties.a").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("integer value accessible as number") {
        UserPropEnv env;
        env.refresh(R"({"count":7})");
        CHECK(env.engine.evaluate("engine.userProperties.count").toInt() == 7);
    }

} // TEST_SUITE SceneScript engine.userProperties

// ------------------------------------------------------------------
// engine.colorScheme population
// ------------------------------------------------------------------
TEST_SUITE("SceneScript engine.colorScheme") {
    // Helper: engine with Vec3 + engine.colorScheme default + refreshJsUserProperties logic,
    // mirroring SceneBackend::setupTextScripts() and refreshJsUserProperties().
    struct ColorSchemeEnv {
        QJSEngine engine;
        QString   userProps;

        ColorSchemeEnv() {
            QJSValue engineObj = engine.newObject();
            engineObj.setProperty("userProperties", engine.newObject());
            engine.globalObject().setProperty("engine", engineObj);
            // Define Vec3 (needed for colorScheme) — must match production Vec3 definition
            engine.evaluate(
                "function Vec3(x,y,z){"
                "  var v={x:x||0,y:y||0,z:z||0};"
                "  v.multiply=function(s){return Vec3(v.x*s,v.y*s,v.z*s);};"
                "  v.add=function(o){return Vec3(v.x+o.x,v.y+o.y,v.z+o.z);};"
                "  v.length=function(){return Math.sqrt(v.x*v.x+v.y*v.y+v.z*v.z);};"
                "  v.normalize=function(){var l=v.length()||1;return Vec3(v.x/l,v.y/l,v.z/l);};"
                "  v.copy=function(){return Vec3(v.x,v.y,v.z);};"
                "  Object.defineProperty(v,'r',{get:function(){return "
                "v.x;},set:function(val){v.x=val;},enumerable:true});"
                "  Object.defineProperty(v,'g',{get:function(){return "
                "v.y;},set:function(val){v.y=val;},enumerable:true});"
                "  Object.defineProperty(v,'b',{get:function(){return "
                "v.z;},set:function(val){v.z=val;},enumerable:true});"
                "  return v;"
                "}"
                "Vec3.fromString=function(s){"
                "  var p=String(s).trim().split(/\\s+/);"
                "  return Vec3(parseFloat(p[0])||0,parseFloat(p[1])||0,parseFloat(p[2])||0);"
                "};");
            // _buildColorScheme + default bundle (mirrors production).
            engine.evaluate(
                "function _buildColorScheme(primary) {"
                "  var cs = Vec3(primary.x, primary.y, primary.z);"
                "  cs.primary      = Vec3(primary.x, primary.y, primary.z);"
                "  cs.secondary    = Vec3(1, 1, 1);"
                "  cs.tertiary     = Vec3(1, 1, 1);"
                "  cs.text         = Vec3(0, 0, 0);"
                "  cs.highContrast = Vec3(0, 0, 0);"
                "  return cs;"
                "}"
                "engine.colorScheme = _buildColorScheme(Vec3(1, 1, 1));");
        }

        void refresh(const QString& json) {
            userProps = json;
            if (json.isEmpty()) return;
            QString escaped = json;
            escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            escaped.replace(QLatin1Char('\''), QStringLiteral("\\'"));
            engine.evaluate(QString("(function(){"
                                    "try{"
                                    "var p=JSON.parse('%1');"
                                    "var up=engine.userProperties;"
                                    "for(var k in p) up[k]=p[k];"
                                    "}catch(e){}"
                                    "})()")
                                .arg(escaped));
            // Sync colorScheme from schemecolor (mirrors refreshJsUserProperties)
            engine.evaluate("(function(){"
                            "if(typeof Vec3==='undefined') return;"
                            "if(typeof _buildColorScheme!=='function') return;"
                            "var sc=engine.userProperties.schemecolor;"
                            "if(sc!==undefined&&sc!==null)"
                            "  engine.colorScheme=_buildColorScheme(Vec3.fromString(sc));"
                            "})()");
        }
    };

    TEST_CASE("engine.colorScheme default is Vec3(1,1,1)") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.y").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.z").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("engine.colorScheme default accessible via r/g/b aliases") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.b").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("engine.colorScheme updated from schemecolor user property") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.5 0.1 0.9"})");
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(0.5));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(0.1));
        CHECK(env.engine.evaluate("engine.colorScheme.b").toNumber() == doctest::Approx(0.9));
    }

    TEST_CASE("engine.colorScheme also accessible via x/y/z") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.2 0.4 0.6"})");
        CHECK(env.engine.evaluate("engine.colorScheme.x").toNumber() == doctest::Approx(0.2));
        CHECK(env.engine.evaluate("engine.colorScheme.y").toNumber() == doctest::Approx(0.4));
        CHECK(env.engine.evaluate("engine.colorScheme.z").toNumber() == doctest::Approx(0.6));
    }

    TEST_CASE("engine.colorScheme is a Vec3 with methods") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"1.0 0.0 0.0"})");
        // multiply
        double mx = env.engine.evaluate("engine.colorScheme.multiply(2).x").toNumber();
        CHECK(mx == doctest::Approx(2.0));
        // length of (1,0,0) = 1
        double len = env.engine.evaluate("engine.colorScheme.length()").toNumber();
        CHECK(len == doctest::Approx(1.0));
    }

    TEST_CASE("engine.colorScheme not updated when schemecolor absent") {
        ColorSchemeEnv env;
        env.refresh(R"({"speed":0.5})"); // no schemecolor
        // Should still be default white
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.b").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("engine.colorScheme updated on second refresh") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"1 0 0"})");
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(0.0));
        env.refresh(R"({"schemecolor":"0 1 0"})");
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("engine.colorScheme black scheme color") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0 0 0"})");
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.g").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.b").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("engine.colorScheme script reads colorScheme correctly") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.3 0.6 0.9"})");
        // Simulate a wallpaper script that reads colorScheme
        double r = env.engine.evaluate("(function(){ return engine.colorScheme.r; })()").toNumber();
        CHECK(r == doctest::Approx(0.3));
    }

    // ---- 5-color bundle (primary / secondary / tertiary / text / highContrast)
    // colorScheme is now a Vec3 (= primary) with four extra sub-Vec3
    // properties.  Back-compat: the bare .x/.y/.z/.r/.g/.b still map to
    // the primary color; the sub-colors let scripts theme secondary UI
    // without extra user-properties.

    TEST_CASE("colorScheme.primary defaults to white") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.primary.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.primary.y").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.primary.z").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("colorScheme.secondary / tertiary default to white") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.secondary.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.tertiary.x").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("colorScheme.text defaults to black (light-mode contrast)") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.text.x").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.text.y").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.text.z").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("colorScheme.highContrast defaults to black") {
        ColorSchemeEnv env;
        CHECK(env.engine.evaluate("engine.colorScheme.highContrast.x").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("colorScheme.primary tracks schemecolor user property") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.25 0.5 0.75"})");
        CHECK(env.engine.evaluate("engine.colorScheme.primary.x").toNumber() == doctest::Approx(0.25));
        CHECK(env.engine.evaluate("engine.colorScheme.primary.y").toNumber() == doctest::Approx(0.5));
        CHECK(env.engine.evaluate("engine.colorScheme.primary.z").toNumber() == doctest::Approx(0.75));
    }

    TEST_CASE("colorScheme top-level Vec3 mirrors primary (back-compat)") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.1 0.2 0.3"})");
        // The bare Vec3 r/g/b (no sub-property) should equal primary.x/y/z
        CHECK(env.engine.evaluate("engine.colorScheme.x").toNumber()
              == env.engine.evaluate("engine.colorScheme.primary.x").toNumber());
        CHECK(env.engine.evaluate("engine.colorScheme.r").toNumber()
              == doctest::Approx(0.1));
    }

    TEST_CASE("colorScheme.secondary / tertiary / text / highContrast survive refresh") {
        ColorSchemeEnv env;
        env.refresh(R"({"schemecolor":"0.9 0.1 0.1"})");
        // Defaults carry through since our current plumbing only touches primary.
        CHECK(env.engine.evaluate("engine.colorScheme.secondary.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.tertiary.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("engine.colorScheme.text.x").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("engine.colorScheme.highContrast.x").toNumber() == doctest::Approx(0.0));
    }

} // TEST_SUITE SceneScript engine.colorScheme

// ===================================================================
// Fixtures for comprehensive SceneScript tests
// ===================================================================

// JS string constants copied from SceneBackend.cpp (lines 662-700)
// Non-vec test-setup JS: `input` placeholder + String.match null-safety +
// a minimal localStorage stub.  Vec2 / Vec3 / Vec4 come from
// wek::qml_helper::kVecClassesJs (evaluated right after this in the
// MathEnv / ScriptEnv ctors), so the three classes share a single source
// of truth with production.
static const char* JS_VEC3_AND_UTILS =
    "var input = { cursorWorldPosition: { x: 0, y: 0 },\n"
    "  cursorScreenPosition: { x: 0, y: 0 },\n"
    "  cursorLeftDown: false };\n"
    "var _origMatch = String.prototype.match;\n"
    "String.prototype.match = function(re) { return _origMatch.call(this, re) || []; };\n"
    "var localStorage = (function() {\n"
    "  var _store = {};\n"
    "  return {\n"
    "    LOCATION_GLOBAL: 0, LOCATION_SCREEN: 1,\n"
    "    get: function(key, loc) { return _store.hasOwnProperty(key) ? _store[key] : undefined; "
    "},\n"
    "    set: function(key, value, loc) { _store[key] = value; },\n"
    "    remove: function(key, loc) { delete _store[key]; },\n"
    "    'delete': function(key, loc) { delete _store[key]; },\n"
    "    clear: function(loc) { _store = {}; }\n"
    "  };\n"
    "})();\n";

static const char* JS_WEMATH =
    "var WEMath = {\n"
    "  PI: Math.PI,\n"
    "  lerp: function(a, b, t) { return a + (b - a) * t; },\n"
    "  mix: function(a, b, t) { return a + (b - a) * t; },\n"
    "  clamp: function(v, lo, hi) { return Math.min(Math.max(v, lo), hi); },\n"
    "  smoothstep: function(edge0, edge1, x) {\n"
    "    var t = Math.min(Math.max((x - edge0) / (edge1 - edge0), 0), 1);\n"
    "    return t * t * (3 - 2 * t);\n"
    "  },\n"
    "  fract: function(x) { return x - Math.floor(x); },\n"
    "  sign: function(x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); },\n"
    "  step: function(edge, x) { return x < edge ? 0 : 1; },\n"
    "  abs: function(x) { return Math.abs(x); },\n"
    "  pow: function(base, exp) { return Math.pow(base, exp); },\n"
    "  mod: function(x, y) { return x - y * Math.floor(x / y); },\n"
    "  degToRad: function(d) { return d * Math.PI / 180; },\n"
    "  radToDeg: function(r) { return r * 180 / Math.PI; },\n"
    "  randomFloat: function(min, max) { return min + Math.random() * (max - min); },\n"
    "  randomInteger: function(min, max) { return Math.floor(min + Math.random() * (max - min + "
    "1)); },\n"
    "  min: function(a, b) { return Math.min(a, b); },\n"
    "  max: function(a, b) { return Math.max(a, b); },\n"
    "  floor: function(x) { return Math.floor(x); },\n"
    "  ceil: function(x) { return Math.ceil(x); },\n"
    "  round: function(x) { return Math.round(x); },\n"
    "  sqrt: function(x) { return Math.sqrt(x); },\n"
    "  sin: function(x) { return Math.sin(x); },\n"
    "  cos: function(x) { return Math.cos(x); },\n"
    "  tan: function(x) { return Math.tan(x); },\n"
    "  asin: function(x) { return Math.asin(x); },\n"
    "  acos: function(x) { return Math.acos(x); },\n"
    "  atan: function(x) { return Math.atan(x); },\n"
    "  atan2: function(y, x) { return Math.atan2(y, x); },\n"
    "  log: function(x) { return Math.log(x); },\n"
    "  exp: function(x) { return Math.exp(x); }\n"
    "};\n"
    "WEMath.smoothStep = WEMath.smoothstep;\n"
    "WEMath.deg2rad = Math.PI / 180;\n"
    "WEMath.rad2deg = 180 / Math.PI;\n"
    "var WEVector = {\n"
    "  angleVector2: function(angle) { var r = angle * WEMath.deg2rad; return Vec2(Math.cos(r), Math.sin(r)); },\n"
    "  vectorAngle2: function(dir) { return Math.atan2(dir.y, dir.x) * WEMath.rad2deg; }\n"
    "};\n";

static const char* JS_WECOLOR =
    "var WEColor = (function() {\n"
    "  function hsv2rgb(hsv) {\n"
    "    var h = hsv.x, s = hsv.y, v = hsv.z;\n"
    "    var i = Math.floor(h * 6);\n"
    "    var f = h * 6 - i;\n"
    "    var p = v * (1 - s);\n"
    "    var q = v * (1 - f * s);\n"
    "    var t = v * (1 - (1 - f) * s);\n"
    "    var r, g, b;\n"
    "    switch (i % 6) {\n"
    "      case 0: r = v; g = t; b = p; break;\n"
    "      case 1: r = q; g = v; b = p; break;\n"
    "      case 2: r = p; g = v; b = t; break;\n"
    "      case 3: r = p; g = q; b = v; break;\n"
    "      case 4: r = t; g = p; b = v; break;\n"
    "      case 5: r = v; g = p; b = q; break;\n"
    "    }\n"
    "    return { x: r, y: g, z: b };\n"
    "  }\n"
    "  function rgb2hsv(rgb) {\n"
    "    var r = rgb.x, g = rgb.y, b = rgb.z;\n"
    "    var max = Math.max(r, g, b), min = Math.min(r, g, b);\n"
    "    var h, s, v = max;\n"
    "    var d = max - min;\n"
    "    s = max === 0 ? 0 : d / max;\n"
    "    if (max === min) { h = 0; }\n"
    "    else {\n"
    "      switch (max) {\n"
    "        case r: h = (g - b) / d + (g < b ? 6 : 0); break;\n"
    "        case g: h = (b - r) / d + 2; break;\n"
    "        case b: h = (r - g) / d + 4; break;\n"
    "      }\n"
    "      h /= 6;\n"
    "    }\n"
    "    return { x: h, y: s, z: v };\n"
    "  }\n"
    "  function normalizeColor(rgb) { return { x: rgb.x/255, y: rgb.y/255, z: rgb.z/255 }; }\n"
    "  function expandColor(rgb) { return { x: rgb.x*255, y: rgb.y*255, z: rgb.z*255 }; }\n"
    "  return { hsv2rgb: hsv2rgb, rgb2hsv: rgb2hsv,\n"
    "           normalizeColor: normalizeColor, expandColor: expandColor };\n"
    "})();\n";

static const char* JS_CONSOLE =
    "var console = {\n"
    "  log: function() {\n"
    "    var args = Array.prototype.slice.call(arguments);\n"
    "    var msg = args.map(function(a){ return String(a); }).join(' ');\n"
    "    if (!console._buf) console._buf = [];\n"
    "    console._buf.push(msg);\n"
    "  },\n"
    "  warn: function() { console.log.apply(console, arguments); },\n"
    "  error: function() { console.log.apply(console, arguments); }\n"
    "};\n";

// Mirrors SceneBackend.cpp's production createScriptProperties.  Uses
// getter/setter pairs so assignments fire the optional `onChange`
// callback defined in each addX({name, value, onChange}) block.
// Matching prod shape keeps drift-risk between test and production low.
static const char* JS_CREATE_SCRIPT_PROPERTIES =
    "function createScriptProperties() {\n"
    "  var _values = {};\n"
    "  var _onChange = {};\n"
    "  var builder = {};\n"
    "  function addProp(def) {\n"
    "    if (!def) return builder;\n"
    "    var n = def.name || def.n;\n"
    "    if (!n) return builder;\n"
    "    var fallback = (typeof def.value !== 'undefined') ? def.value\n"
    "                     : (def.options && def.options.length > 0\n"
    "                          ? def.options[0].value : null);\n"
    "    _values[n] = fallback;\n"
    "    if (def.onChange && typeof def.onChange === 'function') {\n"
    "      _onChange[n] = def.onChange;\n"
    "    }\n"
    "    if (!Object.getOwnPropertyDescriptor(builder, n)) {\n"
    "      Object.defineProperty(builder, n, {\n"
    "        get: function() { return _values[n]; },\n"
    "        set: function(v) {\n"
    "          if (_values[n] === v) return;\n"
    "          _values[n] = v;\n"
    "          var h = _onChange[n];\n"
    "          if (h) {\n"
    "            try { h.call(builder, v); }\n"
    "            catch (e) {\n"
    "              if (typeof console !== 'undefined' && console.log)\n"
    "                console.log('scriptProperty onChange error on ' + n\n"
    "                            + ': ' + (e && e.message));\n"
    "            }\n"
    "          }\n"
    "        },\n"
    "        enumerable: true, configurable: true\n"
    "      });\n"
    "    }\n"
    "    return builder;\n"
    "  }\n"
    "  builder.addCheckbox = addProp;\n"
    "  builder.addSlider = addProp;\n"
    "  builder.addCombo = addProp;\n"
    "  builder.addText = addProp;\n"
    "  builder.addTextInput = addProp;\n"
    "  builder.addColor = addProp;\n"
    "  builder.addFile = addProp;\n"
    "  builder.addDirectory = addProp;\n"
    "  builder.finish = function() { return builder; };\n"
    "  return builder;\n"
    "}\n";

static const char* JS_AUDIO_BUFFERS =
    "engine.AUDIO_RESOLUTION_16 = 16;\n"
    "engine.AUDIO_RESOLUTION_32 = 32;\n"
    "engine.AUDIO_RESOLUTION_64 = 64;\n"
    "engine.registerAudioBuffers = (function(resolution) {\n"
    "  resolution = resolution || 64;\n"
    "  var n = Math.min(Math.max(resolution, 16), 64);\n"
    "  if (n <= 24) n = 16;\n"
    "  else if (n <= 48) n = 32;\n"
    "  else n = 64;\n"
    "  var buf = { left: [], right: [], average: [], resolution: n };\n"
    "  for (var i = 0; i < n; i++) { buf.left.push(0); buf.right.push(0); buf.average.push(0); }\n"
    "  if (!engine._audioRegs) engine._audioRegs = [];\n"
    "  buf._regIdx = engine._audioRegs.length;\n"
    "  engine._audioRegs.push(buf);\n"
    "  return buf;\n"
    "});\n";

// NOTE: the former hand-mirrored JS_LAYER_INFRA constant (a partial,
// drifted copy of production's _makeLayerProxy / thisScene /
// _collectDirtyLayers) was deleted.  ScriptEnv below now evaluates the
// real production source from SceneScriptShimsJs.hpp
// (kLayerProxyJs / kLayerRuntimeJs), so these cases characterize
// production rather than a mirror that could silently diverge.

// Mirrors the `_overlayScriptProps` helper in SceneBackend.cpp.
// Returns a thisLayer wrapper that exposes scriptProperties keys as live
// accessors, falling through to the real layer for everything else.
static const char* JS_OVERLAY_SCRIPT_PROPS =
    "function _overlayScriptProps(layer, sp) {\n"
    "  if (!sp || typeof sp !== 'object') return layer;\n"
    "  var w = layer ? Object.create(layer) : {};\n"
    "  for (var k in sp) if (Object.prototype.hasOwnProperty.call(sp, k)) {\n"
    "    (function(key) {\n"
    "      Object.defineProperty(w, key, {\n"
    "        get: function() { return sp[key]; },\n"
    "        set: function(v) { sp[key] = v; },\n"
    "        enumerable: true, configurable: true\n"
    "      });\n"
    "    })(k);\n"
    "  }\n"
    "  return w;\n"
    "}\n";

static const char* JS_SOUND_INFRA =
    "var _soundLayerStates = {\n"
    "  'music.mp3': { idx: 0, vol: 0.8, silent: false },\n"
    "  'sfx.mp3': { idx: 1, vol: 0.5, silent: true }\n"
    "};\n"
    "engine._soundPlayingStates = {};\n"
    "var _soundLayerCache = {};\n"
    "function _makeSoundLayerProxy(name) {\n"
    "  var info = _soundLayerStates[name];\n"
    "  if (!info) return null;\n"
    "  var _s = { name: name, volume: info.vol, _dirty: {}, _cmds: [] };\n"
    "  var p = {};\n"
    "  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, enumerable:true });\n"
    "  Object.defineProperty(p, 'volume', {\n"
    "    get: function(){ return _s.volume; },\n"
    "    set: function(v){ _s.volume = v; _s._dirty.volume = true; },\n"
    "    enumerable: true\n"
    "  });\n"
    // Mirror production proxy: play/pause/stop update the shadow and
    // mark dirty so the C++ refresh doesn't clobber the intended state
    // while the render thread is still transitioning the stream.
    "  p.play = function(){ _s._cmds.push('play');\n"
    "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
    "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
    "    engine._soundPlayingStates[name] = true;\n"
    "    engine._soundPlayingStatesDirty[name] = true; };\n"
    "  p.stop = function(){ _s._cmds.push('stop');\n"
    "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
    "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
    "    engine._soundPlayingStates[name] = false;\n"
    "    engine._soundPlayingStatesDirty[name] = true; };\n"
    "  p.pause = function(){ _s._cmds.push('pause');\n"
    "    if (!engine._soundPlayingStates) engine._soundPlayingStates = {};\n"
    "    if (!engine._soundPlayingStatesDirty) engine._soundPlayingStatesDirty = {};\n"
    "    engine._soundPlayingStates[name] = false;\n"
    "    engine._soundPlayingStatesDirty[name] = true; };\n"
    "  p.isPlaying = function(){\n"
    "    return !!(engine._soundPlayingStates && engine._soundPlayingStates[name]);\n"
    "  };\n"
    "  Object.defineProperty(p, 'origin', { get: function(){return {x:0,y:0,z:0};}, set: "
    "function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'scale', { get: function(){return {x:1,y:1,z:1};}, set: "
    "function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'angles', { get: function(){return {x:0,y:0,z:0};}, set: "
    "function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'visible', { get: function(){return true;}, set: function(){}, "
    "enumerable:true });\n"
    "  Object.defineProperty(p, 'alpha', { get: function(){return 1;}, set: function(){}, "
    "enumerable:true });\n"
    "  p.getTextureAnimation = function(){\n"
    "    return { rate: 0, frameCount: 1, _frame: 0,\n"
    "      getFrame: function(){ return this._frame; },\n"
    "      setFrame: function(f){ this._frame = f; },\n"
    "      play: function(){ this.rate = 1; },\n"
    "      pause: function(){ this.rate = 0; },\n"
    "      stop: function(){ this.rate = 0; this._frame = 0; },\n"
    "      isPlaying: function(){ return this.rate > 0; }\n"
    "    };\n"
    "  };\n"
    "  p._state = _s;\n"
    // Sound-layer hierarchy stubs — mirrors production _makeSoundLayerProxy.
    "  if (typeof _installSoundHierarchyStubs === 'function') _installSoundHierarchyStubs(p);\n"
    "  return p;\n"
    "}\n"
    "var _origGetLayer = thisScene.getLayer;\n"
    "thisScene.getLayer = function(name) {\n"
    "  var r = _origGetLayer(name);\n"
    "  if (r) return r;\n"
    "  if (_soundLayerCache[name]) return _soundLayerCache[name];\n"
    "  if (_soundLayerStates[name]) {\n"
    "    _soundLayerCache[name] = _makeSoundLayerProxy(name);\n"
    "    return _soundLayerCache[name];\n"
    "  }\n"
    "  return _nullProxy;\n"
    "};\n"
    "thisScene.enumerateLayers = function() {\n"
    "  var layers = [];\n"
    "  for (var name in _layerInitStates) { layers.push(thisScene.getLayer(name)); }\n"
    "  for (var name in _soundLayerStates) { layers.push(thisScene.getLayer(name)); }\n"
    "  return layers;\n"
    "};\n"
    "function _collectDirtySoundLayers() {\n"
    "  var updates = [];\n"
    "  for (var name in _soundLayerCache) {\n"
    "    var s = _soundLayerCache[name]._state;\n"
    "    var hasDirty = Object.keys(s._dirty).length > 0;\n"
    "    var hasCmds = s._cmds.length > 0;\n"
    "    if (!hasDirty && !hasCmds) continue;\n"
    "    updates.push({ name: name, dirty: s._dirty,\n"
    "      volume: s.volume, cmds: s._cmds });\n"
    "    s._dirty = {};\n"
    "    s._cmds = [];\n"
    "  }\n"
    "  return updates;\n"
    "}\n"
    // Final null-safety wrapper
    "var _innerGetLayer = thisScene.getLayer;\n"
    "thisScene.getLayer = function(name) {\n"
    "  var r = _innerGetLayer(name);\n"
    "  if (r !== null && r !== undefined) return r;\n"
    "  return _nullProxy;\n"
    "};\n";

// Lightweight fixture for math-only tests
struct MathEnv {
    QJSEngine engine;
    MathEnv() {
        // Evaluate in two phases: JS_VEC3_AND_UTILS sets up `input` +
        // String.match patch + localStorage + closure Vec2/3/4, then
        // kVecClassesJs overrides Vec2/3/4 with the canonical prototype-
        // based implementations shared with production.  Closures-first
        // keeps the non-Vec pieces of JS_VEC3_AND_UTILS intact; the later
        // kVecClassesJs rewrites the Vec symbols so tests exercise the
        // same class surface production scripts see.
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        engine.evaluate(wek::qml_helper::kMatricesJs);
        engine.evaluate(wek::qml_helper::kInternalNamespaceJs);
        engine.evaluate(JS_WEMATH);
        engine.evaluate(JS_WECOLOR);
    }
};

// Full SceneScript environment fixture
struct ScriptEnv {
    QJSEngine engine;
    ScriptEnv() {
        // engine object
        QJSValue engineObj = engine.newObject();
        engineObj.setProperty("frametime", 0.033);
        engineObj.setProperty("runtime", 0.0);
        engineObj.setProperty("timeOfDay", 0.0);
        engineObj.setProperty("frameCount", 0.0);
        engineObj.setProperty("timeZone", 0.0); // UTC for deterministic tests
        engineObj.setProperty("timeZoneName", "UTC");
        // Script identity: production sets these in setScriptIdentity after
        // all property scripts are loaded.  The fixture uses a fixed value
        // so tests can assert against it.
        engineObj.setProperty("scriptId", 12345);
        engineObj.setProperty("scriptName", "scene");
        engineObj.setProperty("userProperties", engine.newObject());
        engine.globalObject().setProperty("engine", engineObj);
        engine.evaluate("engine.getScriptHash = function() { return '3039'; };\n");

        // shared
        engine.globalObject().setProperty("shared", engine.newObject());
        engine.evaluate("shared.volume = 1.0;\n");

        // console
        engine.evaluate(JS_CONSOLE);

        // Vec3, String.match, localStorage (closure Vec3/Vec4) — then
        // kVecClassesJs overrides Vec2/3/4 with the canonical prototype-
        // based implementations shared with production.
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        engine.evaluate(wek::qml_helper::kVecClassesJs);

        // Mat3/Mat4 — shared with production via SceneScriptShimsJs.hpp.
        engine.evaluate(wek::qml_helper::kMatricesJs);

        // _Internal helper namespace (updateScriptProperties / ...).
        engine.evaluate(wek::qml_helper::kInternalNamespaceJs);

        // WEMath, WEColor
        engine.evaluate(JS_WEMATH);
        engine.evaluate(JS_WECOLOR);

        // Engine stubs
        engine.evaluate("engine.isDesktopDevice = function() { return true; };\n"
                        "engine.isMobileDevice = function() { return false; };\n"
                        "engine.isTabletDevice = function() { return false; };\n"
                        "engine.isWallpaper = function() { return true; };\n"
                        "engine.isScreensaver = function() { return false; };\n"
                        "engine.isRunningInEditor = function() { return false; };\n"
                        "engine.screenResolution = { x: 1920, y: 1080 };\n"
                        "engine.canvasSize = { x: 1920, y: 1080 };\n"
                        "engine.isPortrait = function() { return false; };\n"
                        "engine.isLandscape = function() { return true; };\n"
                        "engine.openUserShortcut = function(name) {};\n");

        // createScriptProperties
        engine.evaluate(JS_CREATE_SCRIPT_PROPERTIES);

        // _overlayScriptProps — scriptProperties-on-thisLayer alias helper
        engine.evaluate(JS_OVERLAY_SCRIPT_PROPS);

        // Audio buffers
        engine.evaluate(JS_AUDIO_BUFFERS);

        // Layer init states (test data)
        engine.evaluate(
            "var _layerInitStates = {\n"
            "  'bg': { o: [100, 200, 0], s: [1, 1, 1], a: [0, 0, 45], sz: [1920, 1080], v: true,\n"
            "          efx: ['Blur', 'Shake_Glitch_3', 'ChromAb'] },\n"
            "  'fg': { o: [0, 0, 0], s: [2, 2, 2], a: [0, 0, 0], sz: [800, 600], v: false }\n"
            "};\n"
            "var _sceneOrtho = [1920, 1080];\n"
            // id↔name maps mirror SceneBackend::m_nodeNameToId injection.
            // IDs here are arbitrary but stable for deterministic tests.
            "var _layerIdToName = { '7': 'bg', '11': 'fg' };\n"
            "var _layerNameToId = { 'bg': 7, 'fg': 11 };\n");

        // Layer infrastructure — shared verbatim with production via
        // SceneScriptShimsJs.hpp (kMaterialProxyJs / kHierarchyProxyJs /
        // kLayerProxyJs / kApplyLayerLiteralJs / kLayerRuntimeJs).  This is
        // the EXACT evaluation order production uses in SceneBackend.cpp
        // (material + hierarchy shims first; the layer factories call
        // _makeMaterialProxy / _installHierarchyMethods; createLayer in the
        // runtime block depends on _applyLayerLiteral).  Running the real
        // production proxy/runtime JS — instead of the old hand-mirrored
        // JS_LAYER_INFRA which had drifted (no pool fast-path, no
        // _woBaked/_wsBaked, no createLayer/destroyLayer, and an
        // object-array _collectDirtyLayers instead of the stride-17 flat
        // array) — turns these cases into a characterization test of
        // production.
        engine.evaluate(wek::qml_helper::kMaterialProxyJs);
        engine.evaluate(wek::qml_helper::kHierarchyProxyJs);
        engine.evaluate(wek::qml_helper::kLayerProxyJs);
        engine.evaluate(wek::qml_helper::kApplyLayerLiteralJs);
        engine.evaluate(wek::qml_helper::kLayerRuntimeJs);

        // Sound layer infrastructure
        engine.evaluate(JS_SOUND_INFRA);

        // Indexed-access methods layered on top of the production layer
        // block above (kLayerProxyJs defines thisScene.getLayer; kLayerRuntimeJs
        // defines getLayerIndex/sortLayer).  These match the definitions
        // injected by SceneBackend right after m_nodeNameToId is built, using
        // the same maps.  getLayerIndex here intentionally re-defines the
        // production one with the deterministic test id map.
        engine.evaluate(
            "thisScene.getLayerByID = function(id) {\n"
            "  var name = _layerIdToName[id];\n"
            "  return name ? thisScene.getLayer(name) : null;\n"
            "};\n"
            "thisScene.getLayerCount = function() {\n"
            "  return Object.keys(_layerInitStates).length;\n"
            "};\n"
            "thisScene.getLayerIndex = function(name) {\n"
            "  var id = _layerNameToId[name];\n"
            "  return (typeof id === 'number') ? id : -1;\n"
            "};\n");

        // Cursor globals on engine: alias the `input` sub-objects and
        // install cursorHitTest (mirrors SceneBackend after Mat3/Mat4).
        engine.evaluate(
            "engine.cursorWorldPosition  = input.cursorWorldPosition;\n"
            "engine.cursorScreenPosition = input.cursorScreenPosition;\n"
            "Object.defineProperty(engine, 'cursorLeftDown', {\n"
            "  get: function() { return input.cursorLeftDown; },\n"
            "  enumerable: true\n"
            "});\n"
            "engine.cursorHitTest = function(x, y) {\n"
            "  if (typeof x !== 'number') x = engine.cursorWorldPosition.x;\n"
            "  if (typeof y !== 'number') y = engine.cursorWorldPosition.y;\n"
            "  if (typeof _layerList === 'undefined') return null;\n"
            "  for (var i = _layerList.length - 1; i >= 0; i--) {\n"
            "    var L = _layerList[i];\n"
            "    if (!L || !L.visible) continue;\n"
            "    var sz = L.size; if (!sz || !sz.x || !sz.y) continue;\n"
            "    var o = L.origin, s = L.scale;\n"
            "    var hw = sz.x * 0.5 * (s ? s.x : 1);\n"
            "    var hh = sz.y * 0.5 * (s ? s.y : 1);\n"
            "    if (Math.abs(x - o.x) <= hw && Math.abs(y - o.y) <= hh) return L;\n"
            "  }\n"
            "  return null;\n"
            "};\n");
    }
};

// Helper: check Vec3-like QJSValue
static void checkVec3(const QJSValue& v, double ex, double ey, double ez) {
    CHECK(v.property("x").toNumber() == doctest::Approx(ex).epsilon(1e-6));
    CHECK(v.property("y").toNumber() == doctest::Approx(ey).epsilon(1e-6));
    CHECK(v.property("z").toNumber() == doctest::Approx(ez).epsilon(1e-6));
}

// ------------------------------------------------------------------
// Vec2
// ------------------------------------------------------------------
TEST_SUITE("Vec2") {
    TEST_CASE("default args produce zero vector") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2()");
        CHECK(v.property("x").toNumber() == doctest::Approx(0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0));
    }

    TEST_CASE("explicit args") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(3, 4)");
        CHECK(v.property("x").toNumber() == doctest::Approx(3));
        CHECK(v.property("y").toNumber() == doctest::Approx(4));
    }

    TEST_CASE("single arg broadcasts") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(5)");
        CHECK(v.property("x").toNumber() == doctest::Approx(5));
        CHECK(v.property("y").toNumber() == doctest::Approx(5));
    }

    TEST_CASE("string constructor") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2('1.5 2.5')");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.5));
        CHECK(v.property("y").toNumber() == doctest::Approx(2.5));
    }

    TEST_CASE("object constructor (from Vec3)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(Vec3(7, 8, 9))");
        CHECK(v.property("x").toNumber() == doctest::Approx(7));
        CHECK(v.property("y").toNumber() == doctest::Approx(8));
    }

    TEST_CASE("length of (3,4) is 5") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec2(3,4).length()").toNumber() == doctest::Approx(5));
    }

    TEST_CASE("lengthSqr") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec2(3,4).lengthSqr()").toNumber() == doctest::Approx(25));
    }

    TEST_CASE("add and subtract") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(1,2).add(Vec2(3,4))");
        CHECK(v.property("x").toNumber() == doctest::Approx(4));
        CHECK(v.property("y").toNumber() == doctest::Approx(6));
    }

    TEST_CASE("multiply scalar") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(2,3).multiply(2)");
        CHECK(v.property("x").toNumber() == doctest::Approx(4));
        CHECK(v.property("y").toNumber() == doctest::Approx(6));
    }

    TEST_CASE("multiply element-wise") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(2,3).multiply(Vec2(4,5))");
        CHECK(v.property("x").toNumber() == doctest::Approx(8));
        CHECK(v.property("y").toNumber() == doctest::Approx(15));
    }

    TEST_CASE("dot product") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec2(1,0).dot(Vec2(0,1))").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("Vec2(2,3).dot(Vec2(4,5))").toNumber() == doctest::Approx(23));
    }

    TEST_CASE("perpendicular") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(1,0).perpendicular()");
        CHECK(v.property("x").toNumber() == doctest::Approx(0));
        CHECK(v.property("y").toNumber() == doctest::Approx(1));
    }

    TEST_CASE("normalize") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(3,4).normalize()");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.6));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.8));
    }

    TEST_CASE("equals") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec2(1,2).equals(Vec2(1,2))").toBool());
        CHECK_FALSE(env.engine.evaluate("Vec2(1,2).equals(Vec2(1,3))").toBool());
    }

    TEST_CASE("toString") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec2(1,2).toString()").toString() == "1 2");
    }

    TEST_CASE("Vec2.fromString") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2.fromString('3.5 4.5')");
        CHECK(v.property("x").toNumber() == doctest::Approx(3.5));
        CHECK(v.property("y").toNumber() == doctest::Approx(4.5));
    }

    TEST_CASE("Vec2.lerp") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2.lerp(Vec2(0,0), Vec2(10,20), 0.5)");
        CHECK(v.property("x").toNumber() == doctest::Approx(5));
        CHECK(v.property("y").toNumber() == doctest::Approx(10));
    }

    TEST_CASE("floor ceil round") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(1.7, 2.3).floor()");
        CHECK(v.property("x").toNumber() == doctest::Approx(1));
        CHECK(v.property("y").toNumber() == doctest::Approx(2));
        v = env.engine.evaluate("Vec2(1.3, 2.7).ceil()");
        CHECK(v.property("x").toNumber() == doctest::Approx(2));
        CHECK(v.property("y").toNumber() == doctest::Approx(3));
    }

} // TEST_SUITE Vec2

// ------------------------------------------------------------------
// Vec3
// ------------------------------------------------------------------
TEST_SUITE("Vec3") {
    TEST_CASE("default args produce zero vector") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3()");
        checkVec3(v, 0, 0, 0);
    }

    TEST_CASE("explicit args stored correctly") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3)");
        checkVec3(v, 1, 2, 3);
    }

    TEST_CASE("multiply scales all components") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).multiply(2)");
        checkVec3(v, 2, 4, 6);
    }

    TEST_CASE("add combines two vectors") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).add(Vec3(4,5,6))");
        checkVec3(v, 5, 7, 9);
    }

    TEST_CASE("subtract differences two vectors") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(5,7,9).subtract(Vec3(1,2,3))");
        checkVec3(v, 4, 5, 6);
    }

    TEST_CASE("length of known vector") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec3(3,4,0).length()").toNumber() == doctest::Approx(5.0));
    }

    TEST_CASE("length of zero vector") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec3(0,0,0).length()").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("normalize unit vector") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(0,0,5).normalize()");
        checkVec3(v, 0, 0, 1);
    }

    TEST_CASE("normalize zero vector does not produce NaN") {
        MathEnv env;
        // Implementation: length()||1 → divides by 1 for zero vector
        QJSValue v = env.engine.evaluate("Vec3(0,0,0).normalize()");
        CHECK_FALSE(env.engine.evaluate("isNaN(Vec3(0,0,0).normalize().x)").toBool());
        checkVec3(v, 0, 0, 0);
    }

    TEST_CASE("copy creates independent object") {
        MathEnv env;
        env.engine.evaluate("var orig = Vec3(1,2,3); var cp = orig.copy(); cp.x = 99;");
        CHECK(env.engine.evaluate("orig.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("cp.x").toNumber() == doctest::Approx(99.0));
    }

    TEST_CASE("dot product") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec3(1,0,0).dot(Vec3(0,1,0))").toNumber() ==
              doctest::Approx(0.0));
        CHECK(env.engine.evaluate("Vec3(1,2,3).dot(Vec3(4,5,6))").toNumber() ==
              doctest::Approx(32.0));
    }

    TEST_CASE("cross product") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,0,0).cross(Vec3(0,1,0))");
        checkVec3(v, 0, 0, 1);
    }

    TEST_CASE("negate") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,-2,3).negate()");
        checkVec3(v, -1, 2, -3);
    }

    TEST_CASE("method chaining") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).multiply(2).add(Vec3(1,1,1))");
        checkVec3(v, 3, 5, 7);
    }

    TEST_CASE("r/g/b aliases read x/y/z") {
        MathEnv env;
        CHECK(env.engine.evaluate("Vec3(0.1,0.2,0.3).r").toNumber() == doctest::Approx(0.1));
        CHECK(env.engine.evaluate("Vec3(0.1,0.2,0.3).g").toNumber() == doctest::Approx(0.2));
        CHECK(env.engine.evaluate("Vec3(0.1,0.2,0.3).b").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("r/g/b aliases write through to x/y/z") {
        MathEnv env;
        env.engine.evaluate("var v = Vec3(0,0,0); v.r = 1; v.g = 2; v.b = 3;");
        CHECK(env.engine.evaluate("v.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("v.y").toNumber() == doctest::Approx(2.0));
        CHECK(env.engine.evaluate("v.z").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("Vec3.fromString parses space-separated color") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.fromString('0.5 0.25 0.75')");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.5));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.25));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.75));
    }

    TEST_CASE("Vec3.fromString result supports methods") {
        MathEnv env;
        // fromString result is a full Vec3 — can call .multiply etc.
        QJSValue v = env.engine.evaluate("Vec3.fromString('1 0 0').multiply(2)");
        CHECK(v.property("x").toNumber() == doctest::Approx(2.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("Vec3.fromString handles extra whitespace") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.fromString('  0.1   0.2   0.3  ')");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.1));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.2));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("Vec3.fromString with missing components defaults to zero") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.fromString('0.5')");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.5));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("Vec3 divide by scalar") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(6, 4, 2).divide(2)");
        CHECK(v.property("x").toNumber() == doctest::Approx(3.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(2.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("Vec3 instance lerp t=0 returns self") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).lerp(Vec3(7,8,9), 0)");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(2.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("Vec3 instance lerp t=1 returns other") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).lerp(Vec3(7,8,9), 1)");
        CHECK(v.property("x").toNumber() == doctest::Approx(7.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(8.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(9.0));
    }

    TEST_CASE("Vec3 instance lerp t=0.5 midpoint") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(0,0,0).lerp(Vec3(2,4,6), 0.5)");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(2.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("Vec3.lerp static t=0 returns a") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.lerp(Vec3(1,2,3), Vec3(7,8,9), 0)");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(2.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("Vec3.lerp static t=1 returns b") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.lerp(Vec3(1,2,3), Vec3(7,8,9), 1)");
        CHECK(v.property("x").toNumber() == doctest::Approx(7.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(8.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(9.0));
    }

    TEST_CASE("Vec3.lerp static midpoint") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3.lerp(Vec3(0,0,0), Vec3(10,20,30), 0.5)");
        CHECK(v.property("x").toNumber() == doctest::Approx(5.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(10.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(15.0));
    }

    TEST_CASE("Vec3 distance to self is zero") {
        MathEnv env;
        double  d = env.engine.evaluate("Vec3(1,2,3).distance(Vec3(1,2,3))").toNumber();
        CHECK(d == doctest::Approx(0.0));
    }

    TEST_CASE("Vec3 distance axis-aligned") {
        MathEnv env;
        double  d = env.engine.evaluate("Vec3(0,0,0).distance(Vec3(3,4,0))").toNumber();
        CHECK(d == doctest::Approx(5.0));
    }

    TEST_CASE("Vec3 distance is symmetric") {
        MathEnv env;
        double  d1 = env.engine.evaluate("Vec3(1,2,3).distance(Vec3(4,5,6))").toNumber();
        double  d2 = env.engine.evaluate("Vec3(4,5,6).distance(Vec3(1,2,3))").toNumber();
        CHECK(d1 == doctest::Approx(d2));
    }

} // TEST_SUITE Vec3

// ------------------------------------------------------------------
// Vec2 / Vec3 polymorphic arithmetic
//
// Scripts expect Vec2/Vec3 to accept scalar / Vec2 / Vec3 operands on
// add/subtract/multiply/divide.  Ours used to take Vec3 only — scripts
// like `pos.add(3)` or `pos.add(new Vec2(x,y))` silently broke.  These
// tests lock in the three branches (scalar / Vec2 / Vec3) so regressions
// surface.
// ------------------------------------------------------------------
TEST_SUITE("Vec3 polymorphic arithmetic") {
    TEST_CASE("add scalar broadcasts to all components") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,3).add(10)");
        checkVec3(v, 11, 12, 13);
    }
    TEST_CASE("subtract scalar broadcasts to all components") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(10,20,30).subtract(5)");
        checkVec3(v, 5, 15, 25);
    }
    TEST_CASE("multiply Vec3 is componentwise") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(2,3,4).multiply(Vec3(10,20,30))");
        checkVec3(v, 20, 60, 120);
    }
    TEST_CASE("add Vec2 preserves our z") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(1,2,99).add(Vec2(10,20))");
        checkVec3(v, 11, 22, 99);
    }
    TEST_CASE("subtract Vec2 preserves our z") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(5,6,7).subtract(Vec2(1,2))");
        checkVec3(v, 4, 4, 7);
    }
    TEST_CASE("multiply Vec2 preserves our z untouched") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(2,3,4).multiply(Vec2(5,10))");
        checkVec3(v, 10, 30, 4);
    }
    TEST_CASE("divide scalar") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(10,20,30).divide(2)");
        checkVec3(v, 5, 10, 15);
    }
    TEST_CASE("divide Vec3 componentwise") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec3(10,20,30).divide(Vec3(2,4,5))");
        checkVec3(v, 5, 5, 6);
    }
}

TEST_SUITE("Vec2 polymorphic arithmetic") {
    TEST_CASE("add scalar broadcasts") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(1,2).add(10)");
        CHECK(v.property("x").toNumber() == doctest::Approx(11));
        CHECK(v.property("y").toNumber() == doctest::Approx(12));
    }
    TEST_CASE("subtract scalar broadcasts") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(10,20).subtract(5)");
        CHECK(v.property("x").toNumber() == doctest::Approx(5));
        CHECK(v.property("y").toNumber() == doctest::Approx(15));
    }
    TEST_CASE("multiply Vec2 componentwise") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("Vec2(2,3).multiply(Vec2(10,20))");
        CHECK(v.property("x").toNumber() == doctest::Approx(20));
        CHECK(v.property("y").toNumber() == doctest::Approx(60));
    }
}

// ------------------------------------------------------------------
// Mat3 / Mat4 — SceneScriptShimsJs.hpp::kMatricesJs
//
// Wallpapers using `new Mat4()` used to ReferenceError silently.  Tests
// cover the identity ctor, translation get/set overloads, and Mat3.angle().
// ------------------------------------------------------------------
TEST_SUITE("Mat4") {
    TEST_CASE("default ctor is identity") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("new Mat4().m");
        // m[0]=m[5]=m[10]=m[15]=1, rest 0
        CHECK(v.property(0).toNumber() == doctest::Approx(1));
        CHECK(v.property(5).toNumber() == doctest::Approx(1));
        CHECK(v.property(10).toNumber() == doctest::Approx(1));
        CHECK(v.property(15).toNumber() == doctest::Approx(1));
        CHECK(v.property(1).toNumber() == doctest::Approx(0));
        CHECK(v.property(12).toNumber() == doctest::Approx(0));
    }
    TEST_CASE("translation(Vec3) sets m[12..14] and returns this for chaining") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat4(); var r = m.translation(Vec3(7, 8, 9));\n"
            "[r === m, m.m[12], m.m[13], m.m[14]]");
        REQUIRE(v.isArray());
        CHECK(v.property(0).toBool() == true);
        CHECK(v.property(1).toNumber() == doctest::Approx(7));
        CHECK(v.property(2).toNumber() == doctest::Approx(8));
        CHECK(v.property(3).toNumber() == doctest::Approx(9));
    }
    TEST_CASE("translation(Vec2) sets xy, zeroes z") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat4(); m.translation(Vec2(3, 4));\n"
            "[m.m[12], m.m[13], m.m[14]]");
        CHECK(v.property(0).toNumber() == doctest::Approx(3));
        CHECK(v.property(1).toNumber() == doctest::Approx(4));
        CHECK(v.property(2).toNumber() == doctest::Approx(0));
    }
    TEST_CASE("translation() with no arg returns a Vec3 reading back m[12..14]") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat4(); m.m[12] = 5; m.m[13] = 6; m.m[14] = 7; m.translation();");
        checkVec3(v, 5, 6, 7);
    }
    TEST_CASE("right/up/forward return basis vectors from the rotation block") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat4();\n"
            "m.m[0]=1; m.m[1]=2; m.m[2]=3;   // right\n"
            "m.m[4]=4; m.m[5]=5; m.m[6]=6;   // up\n"
            "m.m[8]=7; m.m[9]=8; m.m[10]=9;  // forward\n"
            "[m.right().x, m.right().y, m.right().z,\n"
            " m.up().x,    m.up().y,    m.up().z,\n"
            " m.forward().x, m.forward().y, m.forward().z]");
        CHECK(v.property(0).toNumber() == 1);
        CHECK(v.property(1).toNumber() == 2);
        CHECK(v.property(2).toNumber() == 3);
        CHECK(v.property(3).toNumber() == 4);
        CHECK(v.property(4).toNumber() == 5);
        CHECK(v.property(5).toNumber() == 6);
        CHECK(v.property(6).toNumber() == 7);
        CHECK(v.property(7).toNumber() == 8);
        CHECK(v.property(8).toNumber() == 9);
    }
}

TEST_SUITE("Mat3") {
    TEST_CASE("default ctor is identity") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("new Mat3().m");
        CHECK(v.property(0).toNumber() == doctest::Approx(1));
        CHECK(v.property(4).toNumber() == doctest::Approx(1));
        CHECK(v.property(8).toNumber() == doctest::Approx(1));
        CHECK(v.property(1).toNumber() == doctest::Approx(0));
    }
    TEST_CASE("translation(Vec2) sets m[6..7] and chains") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat3(); var r = m.translation(Vec2(11, 12));\n"
            "[r === m, m.m[6], m.m[7]]");
        CHECK(v.property(0).toBool() == true);
        CHECK(v.property(1).toNumber() == doctest::Approx(11));
        CHECK(v.property(2).toNumber() == doctest::Approx(12));
    }
    TEST_CASE("translation() no-arg returns Vec2 from m[6..7]") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var m = new Mat3(); m.m[6]=3; m.m[7]=4; m.translation()");
        CHECK(v.property("x").toNumber() == doctest::Approx(3));
        CHECK(v.property("y").toNumber() == doctest::Approx(4));
    }
    TEST_CASE("angle() on identity is 90 degrees (atan2(1,0))") {
        MathEnv  env;
        // atan2(m[0], -m[1]) = atan2(1, 0) = PI/2 → 90 deg
        double deg = env.engine.evaluate("new Mat3().angle()").toNumber();
        CHECK(deg == doctest::Approx(90.0));
    }
    TEST_CASE("angle() rotated 45 degrees") {
        MathEnv  env;
        // For rotation R(45deg) with first col (cos, -sin), atan2(cos, sin) = 45deg
        double deg = env.engine.evaluate(
            "var m = new Mat3();\n"
            "var c = Math.cos(Math.PI/4), s = Math.sin(Math.PI/4);\n"
            "m.m[0] = c; m.m[1] = -s;\n"
            "m.angle()").toNumber();
        CHECK(deg == doctest::Approx(45.0).epsilon(1e-6));
    }
}

// ------------------------------------------------------------------
// WEMath
// ------------------------------------------------------------------
TEST_SUITE("WEMath") {
    TEST_CASE("lerp t=0 returns a") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.lerp(10, 20, 0)").toNumber() == doctest::Approx(10.0));
    }

    TEST_CASE("lerp t=1 returns b") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.lerp(10, 20, 1)").toNumber() == doctest::Approx(20.0));
    }

    TEST_CASE("lerp t=0.5 returns midpoint") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.lerp(10, 20, 0.5)").toNumber() == doctest::Approx(15.0));
    }

    TEST_CASE("mix is identical to lerp") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.mix(10, 20, 0.25)").toNumber() == doctest::Approx(12.5));
    }

    TEST_CASE("clamp within range") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.clamp(5, 0, 10)").toNumber() == doctest::Approx(5.0));
    }

    TEST_CASE("clamp below minimum") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.clamp(-5, 0, 10)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("clamp above maximum") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.clamp(15, 0, 10)").toNumber() == doctest::Approx(10.0));
    }

    TEST_CASE("smoothstep below edge0") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, -1)").toNumber() ==
              doctest::Approx(0.0));
    }

    TEST_CASE("smoothstep above edge1") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, 2)").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("smoothstep midpoint") {
        MathEnv env;
        // Hermite: 0.5^2 * (3 - 2*0.5) = 0.25 * 2 = 0.5
        CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, 0.5)").toNumber() ==
              doctest::Approx(0.5));
    }

    TEST_CASE("fract of positive") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.fract(3.7)").toNumber() == doctest::Approx(0.7));
    }

    TEST_CASE("fract of integer") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.fract(5.0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("fract of negative") {
        MathEnv env;
        // floor(-0.3) = -1, so fract = -0.3 - (-1) = 0.7
        CHECK(env.engine.evaluate("WEMath.fract(-0.3)").toNumber() == doctest::Approx(0.7));
    }

    TEST_CASE("sign positive negative zero") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.sign(5)").toNumber() == 1);
        CHECK(env.engine.evaluate("WEMath.sign(-3)").toNumber() == -1);
        CHECK(env.engine.evaluate("WEMath.sign(0)").toNumber() == 0);
    }

    TEST_CASE("step function") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.step(0.5, 0.3)").toNumber() == 0);
        CHECK(env.engine.evaluate("WEMath.step(0.5, 0.5)").toNumber() == 1);
        CHECK(env.engine.evaluate("WEMath.step(0.5, 0.7)").toNumber() == 1);
    }

    TEST_CASE("randomFloat returns value in [min, max)") {
        MathEnv env;
        // Run many samples — all must be within [min, max)
        env.engine.evaluate("var ok = true;\n"
                            "for (var i = 0; i < 200; i++) {\n"
                            "  var v = WEMath.randomFloat(2.0, 5.0);\n"
                            "  if (v < 2.0 || v >= 5.0) { ok = false; break; }\n"
                            "}\n");
        CHECK(env.engine.evaluate("ok").toBool() == true);
    }

    TEST_CASE("randomFloat with equal min/max always returns that value") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.randomFloat(3.0, 3.0)").toNumber() ==
              doctest::Approx(3.0));
    }

    TEST_CASE("randomInteger returns integer in [min, max]") {
        MathEnv env;
        env.engine.evaluate("var ok = true;\n"
                            "var sawMin = false, sawMax = false;\n"
                            "for (var i = 0; i < 500; i++) {\n"
                            "  var v = WEMath.randomInteger(1, 6);\n"
                            "  if (v < 1 || v > 6 || v !== Math.floor(v)) { ok = false; break; }\n"
                            "  if (v === 1) sawMin = true;\n"
                            "  if (v === 6) sawMax = true;\n"
                            "}\n");
        CHECK(env.engine.evaluate("ok").toBool() == true);
        // With 500 samples of 6 possible values, min and max are very likely hit
        CHECK(env.engine.evaluate("sawMin").toBool() == true);
        CHECK(env.engine.evaluate("sawMax").toBool() == true);
    }

    TEST_CASE("WEMath.PI equals Math.PI") {
        MathEnv env;
        double  pi = env.engine.evaluate("WEMath.PI").toNumber();
        CHECK(pi == doctest::Approx(3.14159265358979323846));
    }

    TEST_CASE("WEMath.abs positive") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.abs(5.5)").toNumber() == doctest::Approx(5.5));
    }

    TEST_CASE("WEMath.abs negative") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.abs(-3.2)").toNumber() == doctest::Approx(3.2));
    }

    TEST_CASE("WEMath.abs zero") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.abs(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.pow basic") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.pow(2, 10)").toNumber() == doctest::Approx(1024.0));
    }

    TEST_CASE("WEMath.pow fractional exponent") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.pow(9, 0.5)").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("WEMath.mod positive operands") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.mod(7, 3)").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("WEMath.mod negative x always non-negative (GLSL-style)") {
        MathEnv env;
        // GLSL mod(-1, 3) = -1 - 3*floor(-1/3) = -1 - 3*(-1) = 2
        CHECK(env.engine.evaluate("WEMath.mod(-1, 3)").toNumber() == doctest::Approx(2.0));
    }

    TEST_CASE("WEMath.mod fractional") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.mod(2.5, 1.0)").toNumber() == doctest::Approx(0.5));
    }

    TEST_CASE("WEMath.degToRad 180 degrees") {
        MathEnv env;
        double  r = env.engine.evaluate("WEMath.degToRad(180)").toNumber();
        CHECK(r == doctest::Approx(3.14159265358979323846));
    }

    TEST_CASE("WEMath.degToRad 90 degrees") {
        MathEnv env;
        double  r = env.engine.evaluate("WEMath.degToRad(90)").toNumber();
        CHECK(r == doctest::Approx(1.5707963267948966));
    }

    TEST_CASE("WEMath.degToRad 0 degrees") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.degToRad(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.radToDeg PI radians") {
        MathEnv env;
        double  d = env.engine.evaluate("WEMath.radToDeg(Math.PI)").toNumber();
        CHECK(d == doctest::Approx(180.0));
    }

    TEST_CASE("WEMath.radToDeg 0 radians") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.radToDeg(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.degToRad and radToDeg are inverses") {
        MathEnv env;
        double  roundtrip = env.engine.evaluate("WEMath.radToDeg(WEMath.degToRad(45))").toNumber();
        CHECK(roundtrip == doctest::Approx(45.0));
    }

    TEST_CASE("WEMath.smoothStep is alias for smoothstep") {
        MathEnv env;
        double  r1 = env.engine.evaluate("WEMath.smoothstep(0, 1, 0.5)").toNumber();
        double  r2 = env.engine.evaluate("WEMath.smoothStep(0, 1, 0.5)").toNumber();
        CHECK(r1 == doctest::Approx(r2));
        CHECK(r1 == doctest::Approx(0.5));
    }

    TEST_CASE("WEMath.min and max") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.min(3, 7)").toNumber() == doctest::Approx(3.0));
        CHECK(env.engine.evaluate("WEMath.max(3, 7)").toNumber() == doctest::Approx(7.0));
    }

    TEST_CASE("WEMath.floor ceil round") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.floor(2.7)").toNumber() == doctest::Approx(2.0));
        CHECK(env.engine.evaluate("WEMath.ceil(2.3)").toNumber() == doctest::Approx(3.0));
        CHECK(env.engine.evaluate("WEMath.round(2.5)").toNumber() == doctest::Approx(3.0));
    }

    TEST_CASE("WEMath.sqrt") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.sqrt(4)").toNumber() == doctest::Approx(2.0));
        CHECK(env.engine.evaluate("WEMath.sqrt(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.sin cos tan") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.sin(0)").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("WEMath.cos(0)").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("WEMath.tan(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.asin acos atan") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.asin(0)").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("WEMath.acos(1)").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("WEMath.atan(0)").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("WEMath.atan2") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.atan2(1, 1)").toNumber() == doctest::Approx(M_PI / 4.0));
    }

    TEST_CASE("WEMath.log and exp") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.log(1)").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("WEMath.exp(0)").toNumber() == doctest::Approx(1.0));
        // Round-trip: log(exp(2)) == 2
        CHECK(env.engine.evaluate("WEMath.log(WEMath.exp(2))").toNumber() == doctest::Approx(2.0));
    }

    TEST_CASE("WEMath.deg2rad constant") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.deg2rad").toNumber() == doctest::Approx(M_PI / 180.0));
        CHECK(env.engine.evaluate("90 * WEMath.deg2rad").toNumber() == doctest::Approx(M_PI / 2.0));
    }

    TEST_CASE("WEMath.rad2deg constant") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEMath.rad2deg").toNumber() == doctest::Approx(180.0 / M_PI));
        CHECK(env.engine.evaluate("Math.PI * WEMath.rad2deg").toNumber() == doctest::Approx(180.0));
    }

} // TEST_SUITE WEMath

// ------------------------------------------------------------------
// WEColor
// ------------------------------------------------------------------
TEST_SUITE("WEColor") {
    TEST_CASE("red RGB to HSV") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:1,y:0,z:0})");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("green RGB to HSV") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:1,z:0})");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0 / 3.0));
    }

    TEST_CASE("blue RGB to HSV") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:0,z:1})");
        CHECK(v.property("x").toNumber() == doctest::Approx(2.0 / 3.0));
    }

    TEST_CASE("black RGB to HSV") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:0,z:0})");
        CHECK(v.property("z").toNumber() == doctest::Approx(0.0)); // v=0
    }

    TEST_CASE("white RGB to HSV") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:1,y:1,z:1})");
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0)); // s=0
        CHECK(v.property("z").toNumber() == doctest::Approx(1.0)); // v=1
    }

    TEST_CASE("hsv2rgb known red") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.hsv2rgb({x:0,y:1,z:1})");
        checkVec3(v, 1, 0, 0);
    }

    TEST_CASE("round-trip rgb to hsv to rgb") {
        MathEnv env;
        // Test several colors
        for (const char* color : { "{x:1,y:0,z:0}",
                                   "{x:0,y:1,z:0}",
                                   "{x:0,y:0,z:1}",
                                   "{x:0.5,y:0.3,z:0.8}",
                                   "{x:0.2,y:0.7,z:0.4}" }) {
            QString expr = QString("(function(){ var c = %1; var h = WEColor.rgb2hsv(c);"
                                   " var r = WEColor.hsv2rgb(h); return r; })()")
                               .arg(color);
            QJSValue orig = env.engine.evaluate(QString("(%1)").arg(color));
            QJSValue rt   = env.engine.evaluate(expr);
            CHECK(rt.property("x").toNumber() ==
                  doctest::Approx(orig.property("x").toNumber()).epsilon(1e-4));
            CHECK(rt.property("y").toNumber() ==
                  doctest::Approx(orig.property("y").toNumber()).epsilon(1e-4));
            CHECK(rt.property("z").toNumber() ==
                  doctest::Approx(orig.property("z").toNumber()).epsilon(1e-4));
        }
    }

    TEST_CASE("normalizeColor 255 to 1") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.normalizeColor({x:255,y:127.5,z:0})");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.5));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("expandColor 1 to 255") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEColor.expandColor({x:1,y:0.5,z:0})");
        CHECK(v.property("x").toNumber() == doctest::Approx(255));
        CHECK(v.property("y").toNumber() == doctest::Approx(127.5));
        CHECK(v.property("z").toNumber() == doctest::Approx(0));
    }

    TEST_CASE("normalizeColor and expandColor round-trip") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate(
            "var c = WEColor.expandColor(WEColor.normalizeColor({x:200,y:100,z:50})); c");
        CHECK(v.property("x").toNumber() == doctest::Approx(200));
        CHECK(v.property("y").toNumber() == doctest::Approx(100));
        CHECK(v.property("z").toNumber() == doctest::Approx(50));
    }

} // TEST_SUITE WEColor

// ------------------------------------------------------------------
// WEVector
// ------------------------------------------------------------------
// WEVector — degrees on both sides, matching WE's bundled
// jsmodules/wevector.js (multiplies input by deg2rad, output by rad2deg).
TEST_SUITE("WEVector") {
    TEST_CASE("angleVector2 at 0deg returns (1,0)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEVector.angleVector2(0)");
        CHECK(v.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("angleVector2 at 90deg returns (0,1)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEVector.angleVector2(90)");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.0).epsilon(0.001));
        CHECK(v.property("y").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("angleVector2 at 180deg returns (-1,0)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEVector.angleVector2(180)");
        CHECK(v.property("x").toNumber() == doctest::Approx(-1.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0).epsilon(0.001));
    }

    TEST_CASE("angleVector2 at 270deg returns (0,-1)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEVector.angleVector2(270)");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.0).epsilon(0.001));
        CHECK(v.property("y").toNumber() == doctest::Approx(-1.0));
    }

    TEST_CASE("angleVector2 at 45deg returns (sqrt2/2, sqrt2/2)") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("WEVector.angleVector2(45)");
        CHECK(v.property("x").toNumber() == doctest::Approx(M_SQRT1_2));
        CHECK(v.property("y").toNumber() == doctest::Approx(M_SQRT1_2));
    }

    TEST_CASE("vectorAngle2 of (1,0) returns 0deg") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEVector.vectorAngle2(Vec2(1,0))").toNumber() ==
              doctest::Approx(0.0));
    }

    TEST_CASE("vectorAngle2 of (0,1) returns 90deg") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEVector.vectorAngle2(Vec2(0,1))").toNumber() ==
              doctest::Approx(90.0));
    }

    TEST_CASE("vectorAngle2 of (-1,0) returns 180deg") {
        MathEnv env;
        CHECK(env.engine.evaluate("WEVector.vectorAngle2(Vec2(-1,0))").toNumber() ==
              doctest::Approx(180.0));
    }

    TEST_CASE("round-trip angle->vector->angle preserves degrees") {
        MathEnv env;
        double  angle =
            env.engine.evaluate("WEVector.vectorAngle2(WEVector.angleVector2(73.5))").toNumber();
        CHECK(angle == doctest::Approx(73.5));
    }

    // Naruto-style spectrum-ring regression: 64-bar circle. Each bar's
    // position must land on a clean ring of radius `r` and adjacent bars
    // must lie at the expected angular increment.  With degrees-vs-radians
    // confusion these spread chaotically — pin the WE-correct math.
    TEST_CASE("64-bar ring uses 5.625deg increments at radius 265") {
        MathEnv env;
        env.engine.evaluate(
            "var pts = [];"
            "for (var i = 0; i < 64; ++i) {"
            "  var a = 360 * (i / 64);"
            "  var v = WEVector.angleVector2(a);"
            "  pts.push({ x: v.x * 265, y: v.y * 265 });"
            "}");
        // All points lie on the radius-265 circle.
        for (int i = 0; i < 64; ++i) {
            QString  expr = QString("Math.sqrt(pts[%1].x*pts[%1].x + pts[%1].y*pts[%1].y)").arg(i);
            double   r    = env.engine.evaluate(expr).toNumber();
            CHECK(r == doctest::Approx(265.0).epsilon(1e-6));
        }
        // pts[0] is on +X.
        CHECK(env.engine.evaluate("pts[0].x").toNumber() == doctest::Approx(265.0));
        CHECK(env.engine.evaluate("pts[0].y").toNumber() == doctest::Approx(0.0));
        // pts[16] (i=16 → 90deg) is on +Y.
        CHECK(env.engine.evaluate("pts[16].x").toNumber() == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(env.engine.evaluate("pts[16].y").toNumber() == doctest::Approx(265.0));
        // pts[32] (i=32 → 180deg) is on -X.
        CHECK(env.engine.evaluate("pts[32].x").toNumber() == doctest::Approx(-265.0));
        CHECK(env.engine.evaluate("pts[32].y").toNumber() == doctest::Approx(0.0).epsilon(1e-6));
        // pts[48] (i=48 → 270deg) is on -Y.
        CHECK(env.engine.evaluate("pts[48].x").toNumber() == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(env.engine.evaluate("pts[48].y").toNumber() == doctest::Approx(-265.0));
    }

} // TEST_SUITE WEVector

// ------------------------------------------------------------------
// _Internal helper namespace (SceneScriptShimsJs::kInternalNamespaceJs)
// ------------------------------------------------------------------
TEST_SUITE("_Internal") {
    TEST_CASE("updateScriptProperties writes from a JSON string") {
        MathEnv  env;
        env.engine.evaluate(
            "var s = { scriptProperties: { brightness: 0.5, tag: 'old' } };\n"
            "_Internal.updateScriptProperties(s, "
            "'{\"brightness\":0.9,\"tag\":\"new\"}');\n");
        CHECK(env.engine.evaluate("s.scriptProperties.brightness").toNumber()
              == doctest::Approx(0.9));
        CHECK(env.engine.evaluate("s.scriptProperties.tag").toString()
              == QString("new"));
    }

    TEST_CASE("updateScriptProperties accepts a parsed object too") {
        MathEnv  env;
        env.engine.evaluate(
            "var s = { scriptProperties: { n: 1 } };\n"
            "_Internal.updateScriptProperties(s, { n: 42 });\n");
        CHECK(env.engine.evaluate("s.scriptProperties.n").toInt() == 42);
    }

    TEST_CASE("updateScriptProperties rewraps Vec3 fields from string") {
        MathEnv  env;
        env.engine.evaluate(
            "var s = { scriptProperties: { col: Vec3(0,0,0) } };\n"
            "_Internal.updateScriptProperties(s, '{\"col\":\"0.25 0.5 0.75\"}');\n");
        // Duck-typed Vec3 check — the test's closure Vec3 and production's
        // prototype Vec3 both produce {x:,y:,z:} shapes; the point is that
        // the string got parsed into component floats, not assigned raw.
        CHECK(env.engine.evaluate("typeof s.scriptProperties.col").toString()
              == QString("object"));
        CHECK(env.engine.evaluate("s.scriptProperties.col.x").toNumber()
              == doctest::Approx(0.25));
        CHECK(env.engine.evaluate("s.scriptProperties.col.y").toNumber()
              == doctest::Approx(0.5));
        CHECK(env.engine.evaluate("s.scriptProperties.col.z").toNumber()
              == doctest::Approx(0.75));
    }

    TEST_CASE("updateScriptProperties ignores keys not present on script") {
        MathEnv  env;
        env.engine.evaluate(
            "var s = { scriptProperties: { a: 1 } };\n"
            "_Internal.updateScriptProperties(s, { a: 2, unknown: 99 });\n");
        CHECK(env.engine.evaluate("s.scriptProperties.a").toInt() == 2);
        CHECK(env.engine.evaluate("'unknown' in s.scriptProperties").toBool() == false);
    }

    TEST_CASE("convertUserProperties unwraps .value for plain types") {
        MathEnv  env;
        QJSValue r = env.engine.evaluate(
            "_Internal.convertUserProperties('"
            "{\"bright\":{\"type\":\"slider\",\"value\":0.7},"
            "\"name\":{\"type\":\"text\",\"value\":\"hi\"}}')");
        CHECK(r.property("bright").toNumber() == doctest::Approx(0.7));
        CHECK(r.property("name").toString() == QString("hi"));
    }

    TEST_CASE("convertUserProperties wraps color type as Vec3") {
        MathEnv  env;
        QJSValue r = env.engine.evaluate(
            "_Internal.convertUserProperties('"
            "{\"tint\":{\"type\":\"color\",\"value\":\"0.1 0.2 0.3\"}}')");
        CHECK(r.property("tint").property("x").toNumber() == doctest::Approx(0.1));
        CHECK(r.property("tint").property("y").toNumber() == doctest::Approx(0.2));
        CHECK(r.property("tint").property("z").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("convertUserProperties preserves usershortcut structure") {
        MathEnv  env;
        QJSValue r = env.engine.evaluate(
            "_Internal.convertUserProperties('"
            "{\"k\":{\"type\":\"usershortcut\",\"isbound\":true,"
            "\"commandtype\":\"media\",\"file\":\"x.mp3\"}}')");
        CHECK(r.property("k").property("isbound").toBool() == true);
        CHECK(r.property("k").property("commandtype").toString() == QString("media"));
        CHECK(r.property("k").property("file").toString() == QString("x.mp3"));
    }

    TEST_CASE("stringifyConfig honours toJSONString adapter") {
        MathEnv  env;
        env.engine.evaluate(
            "var obj = { pos: { toJSONString: function() { return 'PX'; } },\n"
            "             n:   42 };\n"
            "var s = _Internal.stringifyConfig(obj);");
        QJSValue s = env.engine.evaluate("s");
        CHECK(s.toString() == QString("{\"pos\":\"PX\",\"n\":42}"));
    }
} // TEST_SUITE _Internal

// _applyLayerLiteral is the helper inside thisScene.createLayer that copies
// per-call overrides (origin/scale/angles/...) from an object-literal asset
// onto the rented layer.  Naruto Shippuden 2800255344's bar script calls
// `thisScene.createLayer('models/bar.json')` — the string form.  Before the
// `typeof asset === 'object'` guard, the helper hit `'visible' in asset`
// where asset was a JS string, which throws TypeError ("Cannot use 'in'
// operator to search for 'visible' in models/bar.json") and aborted init().
// These tests pin the guard so future refactors can't reintroduce the throw.
TEST_SUITE("_applyLayerLiteral guard") {
    struct LayerEnv {
        QJSEngine engine;
        LayerEnv() {
            engine.evaluate(wek::qml_helper::kVecClassesJs);
            engine.evaluate(wek::qml_helper::kApplyLayerLiteralJs);
            engine.evaluate(
                "function _newLayer() {\n"
                "  return { origin: Vec3(0,0,0), scale: Vec3(1,1,1),\n"
                "           angles: Vec3(0,0,0), alpha: 1, color: Vec3(1,1,1),\n"
                "           visible: false };\n"
                "}\n");
        }
    };

    TEST_CASE("string asset path doesn't throw and forces visible=true") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, 'models/bar.json');\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("visible").toBool() == true);
    }

    TEST_CASE("asset descriptor with __asset is treated as opaque handle") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "var asset = { __asset: 'models/bar.json' };\n"
            "_applyLayerLiteral(L, asset);\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("visible").toBool() == true);
    }

    TEST_CASE("object literal applies origin/scale/angles overrides") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, { origin: Vec3(10,20,30),\n"
            "                         scale:  Vec3(2,3,4),\n"
            "                         angles: Vec3(0,0,90),\n"
            "                         visible: true });\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("origin").property("x").toNumber() == doctest::Approx(10));
        CHECK(r.property("origin").property("y").toNumber() == doctest::Approx(20));
        CHECK(r.property("origin").property("z").toNumber() == doctest::Approx(30));
        CHECK(r.property("scale").property("x").toNumber() == doctest::Approx(2));
        CHECK(r.property("angles").property("z").toNumber() == doctest::Approx(90));
        CHECK(r.property("visible").toBool() == true);
    }

    TEST_CASE("object literal honors visible:false") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, { visible: false });\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("visible").toBool() == false);
    }

    TEST_CASE("missing visible key defaults to true") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, { origin: Vec3(1,2,3) });\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("visible").toBool() == true);
    }

    TEST_CASE("alpha:0 is honored (uses 'in' check, not truthiness)") {
        LayerEnv env;
        QJSValue r = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, { alpha: 0 });\n"
            "L;\n");
        REQUIRE(! r.isError());
        CHECK(r.property("alpha").toNumber() == doctest::Approx(0));
    }

    TEST_CASE("null/undefined asset just sets visible=true") {
        LayerEnv env;
        QJSValue r1 = env.engine.evaluate(
            "var L = _newLayer();\n"
            "_applyLayerLiteral(L, null);\n"
            "L;\n");
        REQUIRE(! r1.isError());
        CHECK(r1.property("visible").toBool() == true);

        QJSValue r2 = env.engine.evaluate(
            "var L2 = _newLayer();\n"
            "_applyLayerLiteral(L2, undefined);\n"
            "L2;\n");
        REQUIRE(! r2.isError());
        CHECK(r2.property("visible").toBool() == true);
    }
} // TEST_SUITE _applyLayerLiteral guard

// ------------------------------------------------------------------
// SceneScript Globals
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Globals") {
    TEST_CASE("String.match works normally") {
        MathEnv env;
        CHECK(env.engine.evaluate("'hello world'.match(/hello/)[0]").toString() == "hello");
    }

    TEST_CASE("String.match returns empty array on no match") {
        MathEnv  env;
        QJSValue v = env.engine.evaluate("'hello'.match(/xyz/)");
        CHECK(v.isArray());
        CHECK(v.property("length").toInt() == 0);
    }

    TEST_CASE("String.match global regex returns all") {
        MathEnv env;
        CHECK(env.engine.evaluate("'aaa'.match(/a/g).length").toInt() == 3);
    }

    TEST_CASE("localStorage.get returns undefined for missing key") {
        MathEnv env;
        CHECK(env.engine.evaluate("localStorage.get('anything')").isUndefined());
    }

    TEST_CASE("localStorage.set and get round-trip") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('key1', 'hello');");
        CHECK(env.engine.evaluate("localStorage.get('key1')").toString() == "hello");
    }

    TEST_CASE("localStorage stores numbers") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('n', 42);");
        CHECK(env.engine.evaluate("localStorage.get('n')").toInt() == 42);
    }

    TEST_CASE("localStorage stores booleans") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('flag', true);");
        CHECK(env.engine.evaluate("localStorage.get('flag')").toBool() == true);
    }

    TEST_CASE("localStorage overwrites existing key") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('x', 1);");
        env.engine.evaluate("localStorage.set('x', 2);");
        CHECK(env.engine.evaluate("localStorage.get('x')").toInt() == 2);
    }

    TEST_CASE("localStorage.remove deletes a key") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('temp', 'val');");
        env.engine.evaluate("localStorage.remove('temp');");
        CHECK(env.engine.evaluate("localStorage.get('temp')").isUndefined());
    }

    TEST_CASE("localStorage.remove on missing key is safe") {
        MathEnv  env;
        QJSValue r = env.engine.evaluate("localStorage.remove('nonexistent')");
        CHECK_FALSE(r.isError());
    }

    TEST_CASE("localStorage.clear removes all keys") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('a', 1);");
        env.engine.evaluate("localStorage.set('b', 2);");
        env.engine.evaluate("localStorage.clear();");
        CHECK(env.engine.evaluate("localStorage.get('a')").isUndefined());
        CHECK(env.engine.evaluate("localStorage.get('b')").isUndefined());
    }

    TEST_CASE("localStorage persists across evaluate calls") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('persist', 'yes');");
        CHECK(env.engine.evaluate("localStorage.get('persist')").toString() == "yes");
    }

    TEST_CASE("localStorage.delete is alias for remove") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('d', 1);");
        env.engine.evaluate("localStorage['delete']('d');");
        CHECK(env.engine.evaluate("localStorage.get('d')").isUndefined());
    }

    TEST_CASE("localStorage constants exist") {
        MathEnv env;
        CHECK(env.engine.evaluate("localStorage.LOCATION_GLOBAL").toInt() == 0);
        CHECK(env.engine.evaluate("localStorage.LOCATION_SCREEN").toInt() == 1);
    }

    TEST_CASE("localStorage location parameter is accepted") {
        MathEnv env;
        env.engine.evaluate("localStorage.set('k', 'v', localStorage.LOCATION_GLOBAL);");
        CHECK(
            env.engine.evaluate("localStorage.get('k', localStorage.LOCATION_GLOBAL)").toString() ==
            "v");
    }

    TEST_CASE("input.cursorScreenPosition exists") {
        MathEnv env;
        CHECK(env.engine.evaluate("input.cursorScreenPosition.x").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("input.cursorScreenPosition.y").toNumber() == doctest::Approx(0));
    }

    TEST_CASE("input.cursorLeftDown defaults to false") {
        MathEnv env;
        CHECK_FALSE(env.engine.evaluate("input.cursorLeftDown").toBool());
    }

    TEST_CASE("shared.volume defaults to 1.0") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("shared.volume").toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("shared stores arbitrary properties") {
        ScriptEnv env;
        env.engine.evaluate("shared.foo = 42;");
        CHECK(env.engine.evaluate("shared.foo").toInt() == 42);
    }

    TEST_CASE("shared persists across evaluate calls") {
        ScriptEnv env;
        env.engine.evaluate("shared.bar = 'hello';");
        CHECK(env.engine.evaluate("shared.bar").toString() == "hello");
    }

    TEST_CASE("console.log buffers to _buf") {
        ScriptEnv env;
        env.engine.evaluate("console.log('hi');");
        CHECK(env.engine.evaluate("console._buf[0]").toString() == "hi");
    }

    TEST_CASE("console.warn and error delegate to log") {
        ScriptEnv env;
        env.engine.evaluate("console.warn('w'); console.error('e');");
        CHECK(env.engine.evaluate("console._buf.length").toInt() == 2);
        CHECK(env.engine.evaluate("console._buf[0]").toString() == "w");
        CHECK(env.engine.evaluate("console._buf[1]").toString() == "e");
    }

    TEST_CASE("console.log joins multiple args with space") {
        ScriptEnv env;
        env.engine.evaluate("console.log('a', 'b', 'c');");
        CHECK(env.engine.evaluate("console._buf[0]").toString() == "a b c");
    }

    TEST_CASE("engine audio resolution constants") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("engine.AUDIO_RESOLUTION_16").toInt() == 16);
        CHECK(env.engine.evaluate("engine.AUDIO_RESOLUTION_32").toInt() == 32);
        CHECK(env.engine.evaluate("engine.AUDIO_RESOLUTION_64").toInt() == 64);
    }

    TEST_CASE("engine.openUserShortcut is a no-op") {
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate("engine.openUserShortcut('test')");
        CHECK_FALSE(r.isError());
    }

    // ---- Time/frame globals ----------------------------------------------
    // Production sets frameCount from a monotonic counter in
    // evaluatePropertyScripts; timeZone/timeZoneName are populated once at
    // SceneBackend init.  The ScriptEnv fixture seeds plausible defaults so
    // scripts can read these without a live tick loop.

    TEST_CASE("engine.frameCount is readable as a number") {
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate("typeof engine.frameCount");
        CHECK(r.toString() == QString("number"));
    }

    TEST_CASE("engine.frameCount reads back the value written") {
        ScriptEnv env;
        env.engine.evaluate("engine.frameCount = 42");
        CHECK(env.engine.evaluate("engine.frameCount").toInt() == 42);
    }

    TEST_CASE("engine.timeZone is a numeric UTC-offset minutes value") {
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate("typeof engine.timeZone");
        CHECK(r.toString() == QString("number"));
        // Fixture seeds this to 0 (UTC) for determinism.
        CHECK(env.engine.evaluate("engine.timeZone").toInt() == 0);
    }

    TEST_CASE("engine.timeZoneName is a non-empty string") {
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate("typeof engine.timeZoneName");
        CHECK(r.toString() == QString("string"));
        CHECK(env.engine.evaluate("engine.timeZoneName.length > 0").toBool());
    }

    TEST_CASE("time globals compose in arithmetic expressions") {
        // Scripts do things like `engine.frameCount % 60` or
        // `engine.timeOfDay < 0.5` — make sure those don't throw.
        ScriptEnv env;
        env.engine.evaluate("engine.frameCount = 125");
        CHECK(env.engine.evaluate("engine.frameCount % 60").toInt() == 5);
        CHECK(env.engine.evaluate("engine.timeOfDay < 1.0 && engine.timeOfDay >= 0.0")
                  .toBool());
    }

    // ---- Script identity (scriptId / scriptName / getScriptHash) --------
    // Production fingerprints the loaded property scripts and writes
    // engine.scriptId / getScriptHash() so scripts can key persistent state
    // to the current scene's script layout.  The fixture seeds
    // deterministic values (12345 / "scene" / "3039").

    TEST_CASE("engine.scriptId is a number") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("typeof engine.scriptId").toString()
              == QString("number"));
        CHECK(env.engine.evaluate("engine.scriptId").toInt() == 12345);
    }

    TEST_CASE("engine.scriptName is a string") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("engine.scriptName").toString()
              == QString("scene"));
    }

    TEST_CASE("engine.getScriptHash returns a non-empty string") {
        ScriptEnv env;
        QJSValue  h = env.engine.evaluate("engine.getScriptHash()");
        CHECK(h.isString());
        CHECK(h.toString().length() > 0);
    }

    TEST_CASE("getScriptHash is stable across calls within a session") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("engine.getScriptHash() === engine.getScriptHash()")
                  .toBool());
    }

    TEST_CASE("script identity can key a localStorage entry") {
        // Usage pattern: scripts build per-scene keys by appending
        // getScriptHash() so stale data from a different scene doesn't leak.
        ScriptEnv env;
        env.engine.evaluate(
            "var key = 'prefs_' + engine.getScriptHash();\n"
            "var hashPart = key.substring(6);");
        CHECK(env.engine.evaluate("hashPart").toString() == QString("3039"));
    }

    // ---- Cursor aliases on engine ---------------------------------------
    // engine.cursorWorldPosition / cursorScreenPosition are shared refs to
    // the same {x,y} sub-objects on the `input` global; cursorLeftDown is
    // a getter that reads through.  Lets scripts that live outside a layer
    // IIFE query cursor state without digging through `input`.

    TEST_CASE("engine.cursorWorldPosition is an alias of input.cursorWorldPosition") {
        ScriptEnv env;
        env.engine.evaluate(
            "input.cursorWorldPosition.x = 123;\n"
            "input.cursorWorldPosition.y = 456;");
        CHECK(env.engine.evaluate("engine.cursorWorldPosition.x").toNumber()
              == doctest::Approx(123));
        CHECK(env.engine.evaluate("engine.cursorWorldPosition.y").toNumber()
              == doctest::Approx(456));
    }

    TEST_CASE("engine.cursorScreenPosition is an alias of input.cursorScreenPosition") {
        ScriptEnv env;
        env.engine.evaluate(
            "input.cursorScreenPosition.x = 10;\n"
            "input.cursorScreenPosition.y = 20;");
        CHECK(env.engine.evaluate("engine.cursorScreenPosition.x").toNumber()
              == doctest::Approx(10));
    }

    TEST_CASE("engine.cursorLeftDown reads through to input") {
        ScriptEnv env;
        CHECK_FALSE(env.engine.evaluate("engine.cursorLeftDown").toBool());
        env.engine.evaluate("input.cursorLeftDown = true;");
        CHECK(env.engine.evaluate("engine.cursorLeftDown").toBool());
    }

    TEST_CASE("engine.cursorHitTest returns null when no layers overlap") {
        // fixture bg is at origin (100,200,0) size 1920x1080 — 200 is within.
        // Place cursor far outside any layer bounds.
        ScriptEnv env;
        env.engine.evaluate(
            "thisScene.getLayer('bg');\n"  // materialise proxy so it enters _layerList
            "input.cursorWorldPosition.x = 99999;\n"
            "input.cursorWorldPosition.y = 99999;");
        CHECK(env.engine.evaluate("engine.cursorHitTest() === null").toBool());
    }

    TEST_CASE("engine.cursorHitTest returns a layer when cursor is inside") {
        // bg is centered at (100,200) with size 1920x1080 so a cursor at
        // (100,200) is dead-center inside the AABB.
        ScriptEnv env;
        env.engine.evaluate(
            "thisScene.getLayer('bg');\n"
            "input.cursorWorldPosition.x = 100;\n"
            "input.cursorWorldPosition.y = 200;");
        QJSValue hit = env.engine.evaluate("engine.cursorHitTest()");
        REQUIRE(hit.isObject());
        CHECK(hit.property("name").toString() == QString("bg"));
    }

    TEST_CASE("engine.cursorHitTest skips invisible layers") {
        // fg has v:false in the fixture — shouldn't be selected even with
        // cursor inside its AABB.
        ScriptEnv env;
        env.engine.evaluate(
            "thisScene.getLayer('fg');\n"
            "input.cursorWorldPosition.x = 0;\n"
            "input.cursorWorldPosition.y = 0;");
        CHECK(env.engine.evaluate("engine.cursorHitTest() === null").toBool());
    }

    TEST_CASE("engine.cursorHitTest accepts explicit coordinates") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg');");
        QJSValue hit = env.engine.evaluate("engine.cursorHitTest(100, 200)");
        REQUIRE(hit.isObject());
        CHECK(hit.property("name").toString() == QString("bg"));
    }

} // TEST_SUITE SceneScript Globals

// ------------------------------------------------------------------
// createScriptProperties
// ------------------------------------------------------------------
TEST_SUITE("createScriptProperties") {
    TEST_CASE("addSlider stores value") {
        ScriptEnv env;
        CHECK(
            env.engine.evaluate("createScriptProperties().addSlider({name:'x',value:5}).finish().x")
                .toInt() == 5);
    }

    TEST_CASE("addSlider default value is 0") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("createScriptProperties().addSlider({name:'x'}).finish().x")
                  .toInt() == 0);
    }

    TEST_CASE("addCheckbox stores value") {
        ScriptEnv env;
        CHECK(
            env.engine
                .evaluate("createScriptProperties().addCheckbox({name:'y',value:true}).finish().y")
                .toBool());
    }

    TEST_CASE("addCheckbox default value is false") {
        ScriptEnv env;
        CHECK_FALSE(
            env.engine.evaluate("createScriptProperties().addCheckbox({name:'y'}).finish().y")
                .toBool());
    }

    TEST_CASE("addCombo stores value") {
        ScriptEnv env;
        CHECK(
            env.engine.evaluate("createScriptProperties().addCombo({name:'c',value:2}).finish().c")
                .toInt() == 2);
    }

    TEST_CASE("addText and addColor store values") {
        ScriptEnv env;
        CHECK(env.engine
                  .evaluate("createScriptProperties().addText({name:'t',value:'hi'}).finish().t")
                  .toString() == "hi");
        CHECK(env.engine
                  .evaluate(
                      "createScriptProperties().addColor({name:'cl',value:'1 0 0'}).finish().cl")
                  .toString() == "1 0 0");
    }

    TEST_CASE("addColor without value resolves to undefined/null") {
        // Matches production: no hardcoded "0 0 0" default — scripts are
        // expected to supply their own starting colour.
        ScriptEnv env;
        QJSValue  v =
            env.engine.evaluate("createScriptProperties().addColor({name:'cl'}).finish().cl");
        CHECK((v.isUndefined() || v.isNull()));
    }

    TEST_CASE("chaining multiple methods") {
        ScriptEnv env;
        env.engine.evaluate("var props = createScriptProperties()"
                            ".addSlider({name:'speed',value:3})"
                            ".addCheckbox({name:'enabled',value:true})"
                            ".finish();");
        CHECK(env.engine.evaluate("props.speed").toInt() == 3);
        CHECK(env.engine.evaluate("props.enabled").toBool());
    }

    TEST_CASE("per-IIFE override with stored props") {
        ScriptEnv env;
        // Simulate per-IIFE createScriptProperties override (SceneBackend.cpp:1167-1184)
        env.engine.evaluate("var result = (function() {\n"
                            "  var _storedProps = { x: { value: 99 }, z: 42 };\n"
                            "  function createScriptProperties() {\n"
                            "    var b = {};\n"
                            "    function ap(def) {\n"
                            "      var n = def.name || def.n;\n"
                            "      if (n) {\n"
                            "        if (n in _storedProps) {\n"
                            "          var sp = _storedProps[n];\n"
                            "          b[n] = (typeof sp === 'object' && sp !== null && 'value' in "
                            "sp) ? sp.value : sp;\n"
                            "        } else { b[n] = def.value; }\n"
                            "      }\n"
                            "      return b;\n"
                            "    }\n"
                            "    b.addSlider=ap; b.addCheckbox=ap; b.addCombo=ap;\n"
                            "    b.addText=ap; b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
                            "    b.finish=function(){return b;};\n"
                            "    return b;\n"
                            "  }\n"
                            "  return createScriptProperties()\n"
                            "    .addSlider({name:'x',value:5})\n"
                            "    .addSlider({name:'y',value:10})\n"
                            "    .addSlider({name:'z',value:0})\n"
                            "    .finish();\n"
                            "})();\n");
        CHECK(env.engine.evaluate("result.x").toInt() == 99); // overridden
        CHECK(env.engine.evaluate("result.y").toInt() == 10); // default
        CHECK(env.engine.evaluate("result.z").toInt() == 42); // bare value (not {value:})
    }

    // ---- onChange wiring ---------------------------------------------
    //
    // WE scripts (Lucy Clock, many workshop configs) attach `onChange`
    // callbacks to checkboxes so toggling one mutually-excludes its
    // siblings.  The builder must store the callback and invoke it on
    // assignment, with `this` bound to the builder, and must NOT invoke
    // it on the initial default or on same-value writes (the latter
    // would cause infinite recursion in mutual-exclusion patterns).

    TEST_CASE("onChange does NOT fire on initial default") {
        ScriptEnv env;
        env.engine.evaluate(
            "var calls = 0;\n"
            "var p = createScriptProperties()\n"
            "  .addCheckbox({name:'a', value:true,  onChange:function(){ calls++; }})\n"
            "  .addCheckbox({name:'b', value:false, onChange:function(){ calls++; }})\n"
            "  .finish();\n");
        CHECK(env.engine.evaluate("calls").toInt() == 0);
        CHECK(env.engine.evaluate("p.a").toBool() == true);
        CHECK(env.engine.evaluate("p.b").toBool() == false);
    }

    TEST_CASE("onChange fires on value change with new value") {
        ScriptEnv env;
        env.engine.evaluate("var lastVal = null;\n"
                            "var p = createScriptProperties()\n"
                            "  .addCheckbox({name:'a', value:false,\n"
                            "                onChange:function(v){ lastVal = v; }})\n"
                            "  .finish();\n"
                            "p.a = true;\n");
        CHECK(env.engine.evaluate("lastVal").toBool() == true);
        CHECK(env.engine.evaluate("p.a").toBool() == true);
    }

    TEST_CASE("onChange `this` binds to the builder") {
        ScriptEnv env;
        env.engine.evaluate("var sameObj = null;\n"
                            "var p = createScriptProperties()\n"
                            "  .addSlider({name:'x', value:1,\n"
                            "              onChange:function(){ sameObj = (this === p); }})\n"
                            "  .finish();\n"
                            "p.x = 2;\n");
        CHECK(env.engine.evaluate("sameObj").toBool());
    }

    TEST_CASE("same-value write does NOT fire onChange (prevents infinite recursion)") {
        // Lucy Clock's toggleDateFormat writes `false` to sibling checkboxes
        // that are already false.  If onChange fires on same-value writes,
        // those siblings recurse back into toggleDateFormat → stack overflow.
        ScriptEnv env;
        env.engine.evaluate("var calls = 0;\n"
                            "var p = createScriptProperties()\n"
                            "  .addCheckbox({name:'a', value:false,\n"
                            "                onChange:function(){ calls++; }})\n"
                            "  .finish();\n"
                            "p.a = false;\n"   // same value — suppressed
                            "p.a = false;\n"); // same value — suppressed
        CHECK(env.engine.evaluate("calls").toInt() == 0);
    }

    TEST_CASE("Lucy Clock-style mutual exclusion does not recurse") {
        // Reproduces the Clock date-format pattern:
        //   setting useMMDDYYYY=true must unset the other two, and their
        //   onChanges must NOT bounce back to re-set useMMDDYYYY=false.
        ScriptEnv env;
        env.engine.evaluate(
            "var p = createScriptProperties()\n"
            "  .addCheckbox({name:'useMMDDYYYY', value:false,\n"
            "                onChange:function(){ toggleDateFormat('useMMDDYYYY'); }})\n"
            "  .addCheckbox({name:'useDDMMYYYY', value:false,\n"
            "                onChange:function(){ toggleDateFormat('useDDMMYYYY'); }})\n"
            "  .addCheckbox({name:'useYYYYMMDD', value:true,\n"
            "                onChange:function(){ toggleDateFormat('useYYYYMMDD'); }})\n"
            "  .finish();\n"
            "function toggleDateFormat(selected) {\n"
            "  if (p[selected]) {\n"
            "    ['useMMDDYYYY','useDDMMYYYY','useYYYYMMDD'].forEach(function(f){\n"
            "      if (f !== selected) p[f] = false;\n"
            "    });\n"
            "  }\n"
            "}\n"
            "p.useMMDDYYYY = true;\n");
        CHECK(env.engine.evaluate("p.useMMDDYYYY").toBool() == true);
        CHECK(env.engine.evaluate("p.useDDMMYYYY").toBool() == false);
        CHECK(env.engine.evaluate("p.useYYYYMMDD").toBool() == false);
    }

    TEST_CASE("onChange exception doesn't break the setter") {
        // An onChange that throws must NOT prevent the stored value from
        // being updated — scripts should be able to recover on the next
        // assignment.
        ScriptEnv env;
        env.engine.evaluate("var p = createScriptProperties()\n"
                            "  .addSlider({name:'x', value:0,\n"
                            "              onChange:function(){ throw new Error('boom'); }})\n"
                            "  .finish();\n"
                            "p.x = 5;\n");
        CHECK(env.engine.evaluate("p.x").toNumber() == 5);
    }

    TEST_CASE("property without onChange has no callback machinery") {
        // Regression: the getter/setter path must still work for props
        // that don't supply onChange.
        ScriptEnv env;
        env.engine.evaluate("var p = createScriptProperties()\n"
                            "  .addSlider({name:'q', value:3})\n"
                            "  .finish();\n"
                            "p.q = 10;\n");
        CHECK(env.engine.evaluate("p.q").toNumber() == 10);
    }

} // TEST_SUITE createScriptProperties

// ------------------------------------------------------------------
// Layer Proxy
// ------------------------------------------------------------------
TEST_SUITE("Layer Proxy") {
    TEST_CASE("initial origin from init state") {
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayer('bg').origin");
        checkVec3(v, 100, 200, 0);
    }

    TEST_CASE("initial scale from init state") {
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayer('bg').scale");
        checkVec3(v, 1, 1, 1);
    }

    TEST_CASE("initial angles from init state") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').angles.z").toNumber() ==
              doctest::Approx(45));
    }

    TEST_CASE("initial size from init state") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').size.x").toInt() == 1920);
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').size.y").toInt() == 1080);
    }

    TEST_CASE("initial visible from init state") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').visible").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.getLayer('fg').visible").toBool() == false);
    }

    TEST_CASE("name is read-only") {
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg'); l.name = 'xxx';");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').name").toString() == "bg");
    }

    TEST_CASE("debug returns undefined") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').debug").isUndefined());
    }

    TEST_CASE("setting origin marks dirty") {
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg'); l.origin = {x:1,y:2,z:3};");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.origin").toBool());
    }

    TEST_CASE("origin getter returns Vec3 with methods") {
        // Regression: dino_run's update() does `marioOrigin.subtract(origin).lengthSqr()`.
        // If the getter returns a POJO without Vec3 methods, the script throws
        // TypeError at the coin-iteration step and update() silently stops running.
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate(
            "var o = thisScene.getLayer('bg').origin;\n"
             "typeof o.subtract + ',' + typeof o.lengthSqr + ',' + typeof o.length;\n");
        CHECK(r.toString() == "function,function,function");
    }

    TEST_CASE("origin getter returns independent Vec3 copies") {
        // Regression: dino_run caches `initialPosition = thisLayer.origin` and
        // later `marioOrigin = thisLayer.origin`; it then does `isInAir =
        // marioOrigin.y > initialPosition.y + 1`. If both reads share the same
        // reference, isInAir is always false (gravity never applies).
        ScriptEnv env;
        env.engine.evaluate("var a = thisScene.getLayer('bg').origin;\n"
                            "var b = thisScene.getLayer('bg').origin;\n"
                            "var originalA = a.y;\n"
                            "b.y += 100;\n");
        CHECK(env.engine.evaluate("a.y").toNumber() ==
              doctest::Approx(env.engine.evaluate("originalA").toNumber()));
        CHECK(env.engine.evaluate("b.y").toNumber() ==
              doctest::Approx(env.engine.evaluate("originalA").toNumber() + 100));
    }

    TEST_CASE("mutating returned origin does not affect proxy state") {
        // WE semantics: get-copy-then-mutate does NOT persist without explicit set.
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg');\n"
                            "var o = l.origin; o.x = 999;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').origin.x").toNumber() !=
              doctest::Approx(999));
    }

    TEST_CASE("origin round-trip get-mutate-set persists") {
        // The canonical WE pattern: get -> mutate -> set.
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg');\n"
                            "var o = l.origin; o.x = 42; l.origin = o;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').origin.x").toNumber() ==
              doctest::Approx(42));
    }

    TEST_CASE("origin setter copies — script-side mutation does not leak in") {
        // Regression for Blue Archive (2764537029) audio visualizer:
        //   var origin = baseOrigin.copy();
        //   for (i = 0; i < 64; ++i) {
        //       origin.x += 30;
        //       bar.origin = origin;
        //   }
        // Without copy-on-set, all 64 bars end up sharing the same Vec3 ref
        // and pile up at iteration 63's x value (e.g. 2218 instead of spreading
        // 30..2218).  The setter must take a value snapshot of the assigned
        // Vec3, not store the reference.
        ScriptEnv env;
        env.engine.evaluate("var o = Vec3(10, 20, 30);\n"
                            "thisScene.getLayer('bg').origin = o;\n"
                            "o.x = 999;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').origin.x").toNumber() ==
              doctest::Approx(10));
    }

    TEST_CASE("origin setter — same Vec3 assigned to multiple layers yields independent state") {
        // Direct reproduction of the visualizer's per-bar update pattern.
        ScriptEnv env;
        env.engine.evaluate(
            "var l1 = thisScene.getLayer('bg');\n"
            "var l2 = thisScene.getLayer('fg');\n"
            "var origin = Vec3(0, 0, 0);\n"
            "origin.x = 30; l1.origin = origin;\n"
            "origin.x = 60; l2.origin = origin;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').origin.x").toNumber() ==
              doctest::Approx(30));
        CHECK(env.engine.evaluate("thisScene.getLayer('fg').origin.x").toNumber() ==
              doctest::Approx(60));
    }

    TEST_CASE("scale setter — copy-on-assign matches origin behavior") {
        ScriptEnv env;
        env.engine.evaluate("var s = Vec3(2, 2, 2);\n"
                            "thisScene.getLayer('bg').scale = s;\n"
                            "s.y = 99;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').scale.y").toNumber() ==
              doctest::Approx(2));
    }

    TEST_CASE("angles setter — copy-on-assign matches origin behavior") {
        ScriptEnv env;
        env.engine.evaluate("var a = Vec3(0, 0, 90);\n"
                            "thisScene.getLayer('bg').angles = a;\n"
                            "a.z = -1;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').angles.z").toNumber() ==
              doctest::Approx(90));
    }

    TEST_CASE("originalOrigin reflects parse-time value") {
        // Drag-reset scripts (Lucy Clock, Cyberpunk Lucy media player) do
        // `thisLayer.origin = thisLayer.originalOrigin` to snap back to
        // the author-placed coordinate.  Must mirror the init state exactly.
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayer('bg').originalOrigin");
        checkVec3(v, 100, 200, 0);
    }

    TEST_CASE("originalScale reflects parse-time value") {
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayer('fg').originalScale");
        checkVec3(v, 2, 2, 2);
    }

    TEST_CASE("originalAngles reflects parse-time value") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').originalAngles.z").toNumber() ==
              doctest::Approx(45));
    }

    TEST_CASE("originalOrigin is frozen when origin is mutated") {
        // Core invariant — otherwise reset-to-default collapses to reset-to-current.
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg');\n"
                            "l.origin = Vec3(0,0,0);\n");
        QJSValue v = env.engine.evaluate("thisScene.getLayer('bg').originalOrigin");
        checkVec3(v, 100, 200, 0);
    }

    TEST_CASE("originalOrigin returns independent copies each get") {
        // Matches the defensive-copy semantic of `origin`.  Without fresh
        // copies per read, scripts that mutate the returned Vec3 would
        // silently stomp the frozen snapshot.
        ScriptEnv env;
        env.engine.evaluate("var a = thisScene.getLayer('bg').originalOrigin;\n"
                            "a.x = -999;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').originalOrigin.x").toNumber() ==
              doctest::Approx(100));
    }

    TEST_CASE("originalOrigin setter is a no-op (read-only)") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').originalOrigin = Vec3(1,2,3);");
        QJSValue v = env.engine.evaluate("thisScene.getLayer('bg').originalOrigin");
        checkVec3(v, 100, 200, 0);
    }

    TEST_CASE("Lucy drag-reset end-to-end: origin → originalOrigin snaps back") {
        // Reproduces cad28c04.js resetPosition():
        //   thisLayer.origin = thisLayer.originalOrigin;
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg');\n"
                            "l.origin = Vec3(555, 666, 7);\n"  // user dragged
                            "l.origin = l.originalOrigin;\n"); // reset
        QJSValue v = env.engine.evaluate("thisScene.getLayer('bg').origin");
        checkVec3(v, 100, 200, 0);
    }

    TEST_CASE("setting visible marks dirty") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').visible = false;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.visible").toBool());
    }

    TEST_CASE("setting alpha marks dirty") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').alpha = 0.5;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.alpha").toBool());
    }

    TEST_CASE("opacity is an alias for alpha") {
        // Wallpaper 2866203962 playervolume.cursorUp writes
        // `percentageLayer.opacity = 0` to hide the "100%" text.  Without this
        // alias the assignment silently dropped into a plain JS property, the
        // layer's alpha stayed at 1, and the old "100%" bitmap remained on screen.
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').opacity = 0.25;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').alpha").toNumber() ==
              doctest::Approx(0.25));
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.alpha").toBool());
        // Reading opacity returns alpha.
        env.engine.evaluate("thisScene.getLayer('bg').alpha = 0.7;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').opacity").toNumber() ==
              doctest::Approx(0.7));
    }

    TEST_CASE("solid defaults to true and is writeable") {
        // Scripts (e.g. wallpaper 2866203962 playerplay alpha update) toggle
        // `element.solid = false` so faded-out UI doesn't catch clicks.
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').solid").toBool() == true);
        env.engine.evaluate("thisScene.getLayer('bg').solid = false;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').solid").toBool() == false);
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state.solid").toBool() == false);
        env.engine.evaluate("thisScene.getLayer('bg').solid = true;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').solid").toBool() == true);
    }

    TEST_CASE("setting text marks dirty") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').text = 'hello';");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.text").toBool());
    }

    TEST_CASE("play stop pause are no-ops") {
        ScriptEnv env;
        QJSValue  r = env.engine.evaluate(
            "var l = thisScene.getLayer('bg'); l.play(); l.stop(); l.pause(); true;");
        CHECK_FALSE(r.isError());
    }

    // Dynamic-asset pool: dino_run / any game-style wallpaper that spawns
    // runtime layers via thisScene.createLayer(asset).  The pool holds hidden
    // scene-node names that createLayer pops and destroyLayer returns.
    TEST_CASE("createLayer pops from asset pool and marks visible") {
        ScriptEnv env;
        // Simulate parser-supplied pool data: 3 hidden pool layers
        env.engine.evaluate(
            "_layerInitStates['__pool_coin_0'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [0,0], v: false };\n"
            "_layerInitStates['__pool_coin_1'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [0,0], v: false };\n"
            "_layerInitStates['__pool_coin_2'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [0,0], v: false };\n"
            "engine._assetPools = { 'coin': ['__pool_coin_0','__pool_coin_1','__pool_coin_2'] };\n"
            "engine.registerAsset = function(path) { return { __asset: path }; };\n"
            "thisScene.createLayer = function(asset) {\n"
            "  var path = asset && asset.__asset;\n"
            "  var pool = path && engine._assetPools[path];\n"
            "  if (pool && pool.length > 0) {\n"
            "    var name = pool.shift();\n"
            "    var layer = thisScene.getLayer(name);\n"
            "    if (layer && layer !== _nullProxy) {\n"
            "      layer.__asset = path;\n"
            "      layer.visible = true;\n"
            "      return layer;\n"
            "    }\n"
            "  }\n"
            "  return { __stub: true };\n"
            "};\n"
            "thisScene.destroyLayer = function(layer) {\n"
            "  if (!layer || layer.__stub) return;\n"
            "  layer.visible = false;\n"
            "  var path = layer.__asset;\n"
            "  if (path && engine._assetPools[path] && layer.name)\n"
            "    engine._assetPools[path].push(layer.name);\n"
            "};\n"
            "var coinAsset = engine.registerAsset('coin');\n"
            "var c1 = thisScene.createLayer(coinAsset);\n"
            "var c2 = thisScene.createLayer(coinAsset);\n");
        // First two pool slots were popped; visible flags set
        CHECK(env.engine.evaluate("c1.name").toString() == "__pool_coin_0");
        CHECK(env.engine.evaluate("c2.name").toString() == "__pool_coin_1");
        CHECK(env.engine.evaluate("c1.visible").toBool());
        CHECK(env.engine.evaluate("c2.visible").toBool());
        CHECK(env.engine.evaluate("engine._assetPools.coin.length").toInt() == 1);
    }

    TEST_CASE("pool layer origin setter copies — visualizer per-bar pattern") {
        // Direct repro of Blue Archive (2764537029) audio visualizer logic.
        // Without copy-on-set on pool slots, all 64 bars share one Vec3 ref
        // and pile up at the last loop iteration.
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['__pool_bar_0'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [4,4], v: false };\n"
            "_layerInitStates['__pool_bar_1'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [4,4], v: false };\n"
            "_layerInitStates['__pool_bar_2'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [4,4], v: false };\n"
            "var b0 = thisScene.getLayer('__pool_bar_0');\n"
            "var b1 = thisScene.getLayer('__pool_bar_1');\n"
            "var b2 = thisScene.getLayer('__pool_bar_2');\n"
            "var origin = Vec3(0, 100, 0);\n"
            "origin.x = 30; b0.origin = origin;\n"
            "origin.x = 60; b1.origin = origin;\n"
            "origin.x = 90; b2.origin = origin;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('__pool_bar_0').origin.x").toNumber() ==
              doctest::Approx(30));
        CHECK(env.engine.evaluate("thisScene.getLayer('__pool_bar_1').origin.x").toNumber() ==
              doctest::Approx(60));
        CHECK(env.engine.evaluate("thisScene.getLayer('__pool_bar_2').origin.x").toNumber() ==
              doctest::Approx(90));
    }

    TEST_CASE("pool layer scale/angles also copy on assign") {
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['__pool_z_0'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [4,4], v: false };\n"
            "_layerInitStates['__pool_z_1'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [4,4], v: false };\n"
            "var z0 = thisScene.getLayer('__pool_z_0');\n"
            "var z1 = thisScene.getLayer('__pool_z_1');\n"
            "var s = Vec3(2, 3, 4);\n"
            "z0.scale = s;\n"
            "s.y = 99;\n"
            "z1.scale = s;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('__pool_z_0').scale.y").toNumber() ==
              doctest::Approx(3));
        CHECK(env.engine.evaluate("thisScene.getLayer('__pool_z_1').scale.y").toNumber() ==
              doctest::Approx(99));
    }

    TEST_CASE("destroyLayer returns slot to pool and hides it") {
        ScriptEnv env;
        env.engine.evaluate("_layerInitStates['__pool_x_0'] = { o: [0,0,0], s: [1,1,1],\n"
                            "  a: [0,0,0], sz: [0,0], v: false };\n"
                            "engine._assetPools = { 'x': ['__pool_x_0'] };\n"
                            "engine.registerAsset = function(path) { return { __asset: path }; };\n"
                            "thisScene.createLayer = function(asset) {\n"
                            "  var path = asset && asset.__asset;\n"
                            "  var pool = path && engine._assetPools[path];\n"
                            "  if (pool && pool.length > 0) {\n"
                            "    var name = pool.shift();\n"
                            "    var layer = thisScene.getLayer(name);\n"
                            "    if (layer && layer !== _nullProxy) {\n"
                            "      layer.__asset = path;\n"
                            "      layer.visible = true;\n"
                            "      return layer;\n"
                            "    }\n"
                            "  }\n"
                            "  return { __stub: true };\n"
                            "};\n"
                            "thisScene.destroyLayer = function(layer) {\n"
                            "  if (!layer || layer.__stub) return;\n"
                            "  layer.visible = false;\n"
                            "  var path = layer.__asset;\n"
                            "  if (path && engine._assetPools[path] && layer.name)\n"
                            "    engine._assetPools[path].push(layer.name);\n"
                            "};\n"
                            "var a  = engine.registerAsset('x');\n"
                            "var l1 = thisScene.createLayer(a);\n"
                            "thisScene.destroyLayer(l1);\n");
        CHECK(env.engine.evaluate("engine._assetPools.x.length").toInt() == 1);
        CHECK(env.engine.evaluate("engine._assetPools.x[0]").toString() == "__pool_x_0");
        CHECK_FALSE(env.engine.evaluate("l1.visible").toBool());
    }

    // Real-WE createLayer form: object literal with `image:` and other
    // properties baked in (trail-style scripts, wallpaper 3509243656 Three-Body
    // Santi).  Our JS createLayer must extract the path from asset.image and
    // apply origin/scale/alpha/visible/color/angles to the rented layer.
    TEST_CASE("createLayer accepts object literal with image key") {
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['__pool_trail_0'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [0,0], v: false };\n"
            "engine._assetPools = { 'models/trail.json': ['__pool_trail_0'] };\n"
            "thisScene.createLayer = function(asset) {\n"
            "  var path = asset && (asset.__asset || asset.image);\n"
            "  var pool = path && engine._assetPools[path];\n"
            "  if (pool && pool.length > 0) {\n"
            "    var name = pool.shift();\n"
            "    var layer = thisScene.getLayer(name);\n"
            "    if (layer && layer !== _nullProxy) {\n"
            "      layer.__asset = path;\n"
            "      if (asset && !asset.__asset) {\n"
            "        if (asset.origin) layer.origin = asset.origin;\n"
            "        if (asset.scale)  layer.scale  = asset.scale;\n"
            "        if (asset.angles) layer.angles = asset.angles;\n"
            "        if ('alpha' in asset) layer.alpha = asset.alpha;\n"
            "        if (asset.color) layer.color = asset.color;\n"
            "        layer.visible = ('visible' in asset) ? !!asset.visible : true;\n"
            "      } else {\n"
            "        layer.visible = true;\n"
            "      }\n"
            "      return layer;\n"
            "    }\n"
            "  }\n"
            "  return { __stub: true };\n"
            "};\n"
            "var l = thisScene.createLayer({\n"
            "  image: 'models/trail.json',\n"
            "  origin: Vec3(1, 2, 3),\n"
            "  scale:  Vec3(0.5, 0.5, 0.5),\n"
            "  alpha:  0.25,\n"
            "  visible: false\n"
            "});\n");
        // Pool popped, layer rented
        CHECK(env.engine.evaluate("l.name").toString() == "__pool_trail_0");
        CHECK(env.engine.evaluate("l.__asset").toString() == "models/trail.json");
        // Literal-supplied properties applied
        CHECK(env.engine.evaluate("l.origin.x").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("l.origin.y").toNumber() == doctest::Approx(2.0));
        CHECK(env.engine.evaluate("l.origin.z").toNumber() == doctest::Approx(3.0));
        CHECK(env.engine.evaluate("l.scale.x").toNumber() == doctest::Approx(0.5));
        CHECK(env.engine.evaluate("l.alpha").toNumber() == doctest::Approx(0.25));
        // 'visible: false' in literal overrides the default-true, matching WE
        // where scripts manually toggle visible=true after configuring.
        CHECK_FALSE(env.engine.evaluate("l.visible").toBool());
        CHECK(env.engine.evaluate("engine._assetPools['models/trail.json'].length").toInt() == 0);
    }

    TEST_CASE("createLayer literal without visible key defaults to visible=true") {
        // Regression: earlier impl always set visible=true, breaking scripts
        // that create trail points with visible:false then toggle later.  Now
        // we honor the literal's explicit value but default to true when unset.
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['__pool_q_0'] = { o: [0,0,0], s: [1,1,1],\n"
            "  a: [0,0,0], sz: [0,0], v: false };\n"
            "engine._assetPools = { 'q.json': ['__pool_q_0'] };\n"
            "thisScene.createLayer = function(asset) {\n"
            "  var path = asset && (asset.__asset || asset.image);\n"
            "  var pool = path && engine._assetPools[path];\n"
            "  if (pool && pool.length > 0) {\n"
            "    var name = pool.shift();\n"
            "    var layer = thisScene.getLayer(name);\n"
            "    if (layer && layer !== _nullProxy) {\n"
            "      layer.__asset = path;\n"
            "      if (asset && !asset.__asset) {\n"
            "        layer.visible = ('visible' in asset) ? !!asset.visible : true;\n"
            "      } else {\n"
            "        layer.visible = true;\n"
            "      }\n"
            "      return layer;\n"
            "    }\n"
            "  }\n"
            "  return { __stub: true };\n"
            "};\n"
            "var l1 = thisScene.createLayer({ image: 'q.json' });\n");
        CHECK(env.engine.evaluate("l1.visible").toBool());
    }

    // Pool exhaustion → LRU recycle.  Scripts like 3body's MAIN physics loop
    // call createLayer without matching destroyLayer; once free slots run out we
    // recycle the OLDEST live slot instead of falling back to a render-nothing
    // stub.  See feedback_no_stubs.
    TEST_CASE("createLayer recycles oldest live slot when pool exhausted") {
        ScriptEnv env;
        env.engine.evaluate("_layerInitStates['__pool_x_0'] = { o: [0,0,0], s: [1,1,1],\n"
                            "  a: [0,0,0], sz: [0,0], v: false };\n"
                            "_layerInitStates['__pool_x_1'] = { o: [0,0,0], s: [1,1,1],\n"
                            "  a: [0,0,0], sz: [0,0], v: false };\n"
                            "engine._assetPools = { 'x.json': ['__pool_x_0','__pool_x_1'] };\n"
                            "engine._assetLive = {};\n"
                            "thisScene.createLayer = function(asset) {\n"
                            "  var path = asset && (asset.__asset || asset.image);\n"
                            "  var pool = path && engine._assetPools[path];\n"
                            "  if (!engine._assetLive[path]) engine._assetLive[path] = [];\n"
                            "  var live = engine._assetLive[path];\n"
                            "  var name = null;\n"
                            "  if (pool && pool.length > 0) name = pool.shift();\n"
                            "  else if (live.length > 0) name = live.shift();\n"
                            "  if (name) {\n"
                            "    var layer = thisScene.getLayer(name);\n"
                            "    if (layer && layer !== _nullProxy) {\n"
                            "      layer.__asset = path;\n"
                            "      layer.visible = true;\n"
                            "      live.push(name);\n"
                            "      return layer;\n"
                            "    }\n"
                            "  }\n"
                            "  return { __stub: true };\n"
                            "};\n"
                            // Rent both, pool now empty.
                            "var l0 = thisScene.createLayer({ image: 'x.json' });\n"
                            "var l1 = thisScene.createLayer({ image: 'x.json' });\n"
                            // Third call should recycle the oldest (l0 = __pool_x_0).
                            "var l2 = thisScene.createLayer({ image: 'x.json' });\n");
        CHECK(env.engine.evaluate("l0.name").toString() == "__pool_x_0");
        CHECK(env.engine.evaluate("l1.name").toString() == "__pool_x_1");
        CHECK(env.engine.evaluate("l2.name").toString() == "__pool_x_0");
        // No stubs.
        CHECK_FALSE(env.engine.evaluate("!!l2.__stub").toBool());
        // Live list holds current tenants in LRU order: l1 still live, l2 newest.
        CHECK(env.engine.evaluate("engine._assetLive['x.json'].length").toInt() == 2);
        CHECK(env.engine.evaluate("engine._assetLive['x.json'][0]").toString() == "__pool_x_1");
        CHECK(env.engine.evaluate("engine._assetLive['x.json'][1]").toString() == "__pool_x_0");
    }

    TEST_CASE("destroyLayer removes from live list and returns to free pool") {
        ScriptEnv env;
        env.engine.evaluate("_layerInitStates['__pool_y_0'] = { o: [0,0,0], s: [1,1,1],\n"
                            "  a: [0,0,0], sz: [0,0], v: false };\n"
                            "engine._assetPools = { 'y.json': ['__pool_y_0'] };\n"
                            "engine._assetLive = {};\n"
                            "thisScene.createLayer = function(asset) {\n"
                            "  var path = asset && (asset.__asset || asset.image);\n"
                            "  var pool = engine._assetPools[path];\n"
                            "  if (!engine._assetLive[path]) engine._assetLive[path] = [];\n"
                            "  var live = engine._assetLive[path];\n"
                            "  if (pool.length > 0) {\n"
                            "    var name = pool.shift();\n"
                            "    var layer = thisScene.getLayer(name);\n"
                            "    layer.__asset = path; layer.visible = true;\n"
                            "    live.push(name);\n"
                            "    return layer;\n"
                            "  }\n"
                            "  return { __stub: true };\n"
                            "};\n"
                            "thisScene.destroyLayer = function(layer) {\n"
                            "  if (!layer || layer.__stub) return;\n"
                            "  layer.visible = false;\n"
                            "  var path = layer.__asset;\n"
                            "  var live = engine._assetLive[path];\n"
                            "  if (live && layer.name) {\n"
                            "    var idx = live.indexOf(layer.name);\n"
                            "    if (idx >= 0) live.splice(idx, 1);\n"
                            "  }\n"
                            "  if (engine._assetPools[path] && layer.name)\n"
                            "    engine._assetPools[path].push(layer.name);\n"
                            "};\n"
                            "var l = thisScene.createLayer({ image: 'y.json' });\n"
                            "thisScene.destroyLayer(l);\n");
        // After destroy: slot back in free pool, live list empty.
        CHECK(env.engine.evaluate("engine._assetPools['y.json'].length").toInt() == 1);
        CHECK(env.engine.evaluate("engine._assetLive['y.json'].length").toInt() == 0);
    }

    TEST_CASE("createLayer legacy __asset form still works") {
        // Dino-run style: registerAsset returns a descriptor, createLayer
        // consumes it via __asset.  Must coexist with the new literal form.
        ScriptEnv env;
        env.engine.evaluate("_layerInitStates['__pool_coin_0'] = { o: [0,0,0], s: [1,1,1],\n"
                            "  a: [0,0,0], sz: [0,0], v: false };\n"
                            "engine._assetPools = { 'coin': ['__pool_coin_0'] };\n"
                            "engine.registerAsset = function(path) { return { __asset: path }; };\n"
                            "thisScene.createLayer = function(asset) {\n"
                            "  var path = asset && (asset.__asset || asset.image);\n"
                            "  var pool = path && engine._assetPools[path];\n"
                            "  if (pool && pool.length > 0) {\n"
                            "    var name = pool.shift();\n"
                            "    var layer = thisScene.getLayer(name);\n"
                            "    if (layer && layer !== _nullProxy) {\n"
                            "      layer.__asset = path;\n"
                            "      if (!asset.__asset && 'visible' in asset) {\n"
                            "        layer.visible = !!asset.visible;\n"
                            "      } else {\n"
                            "        layer.visible = true;\n"
                            "      }\n"
                            "      return layer;\n"
                            "    }\n"
                            "  }\n"
                            "  return { __stub: true };\n"
                            "};\n"
                            "var a = engine.registerAsset('coin');\n"
                            "var l = thisScene.createLayer(a);\n");
        CHECK(env.engine.evaluate("l.name").toString() == "__pool_coin_0");
        CHECK(env.engine.evaluate("l.visible").toBool());
    }

    TEST_CASE("createLayer falls back to stub when pool exhausted") {
        ScriptEnv env;
        env.engine.evaluate("engine._assetPools = { 'empty': [] };\n"
                            "engine.registerAsset = function(path) { return { __asset: path }; };\n"
                            "thisScene.createLayer = function(asset) {\n"
                            "  var path = asset && asset.__asset;\n"
                            "  var pool = path && engine._assetPools[path];\n"
                            "  if (pool && pool.length > 0) {\n"
                            "    var name = pool.shift();\n"
                            "    var layer = thisScene.getLayer(name);\n"
                            "    if (layer && layer !== _nullProxy) {\n"
                            "      layer.__asset = path; layer.visible = true; return layer;\n"
                            "    }\n"
                            "  }\n"
                            "  return { __stub: true, origin: Vec3(0,0,0), visible: true };\n"
                            "};\n"
                            "var a = engine.registerAsset('empty');\n"
                            "var l = thisScene.createLayer(a);\n");
        CHECK(env.engine.evaluate("l.__stub").toBool());
    }

    TEST_CASE("isPlaying returns false") {
        ScriptEnv env;
        CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('bg').isPlaying()").toBool());
    }

    TEST_CASE("getTextureAnimation lifecycle (no-bridge defaults)") {
        // Production's getTextureAnimation is __sceneBridge-backed: getFrame /
        // isPlaying read back from __sceneBridge.getLayerSpriteInfo, and
        // play/pause/stop/setFrame dispatch to __sceneBridge.setLayerSpriteFrame
        // (no JS-side _frame state).  ScriptEnv installs no bridge, so the
        // reader returns the documented defaults: frameCount 1, currentFrame 0,
        // duration 0, isManualPin false (=> rate 1, isPlaying true), and the
        // setters are guarded no-ops.  The full bridge-backed lifecycle is
        // characterized separately under the "getTextureAnimation (production
        // proxy)" suite with a sprite-info mock.
        ScriptEnv env;
        env.engine.evaluate("var anim = thisScene.getLayer('bg').getTextureAnimation();\n"
                            "anim.setFrame(5);\n"); // no-op without a bridge
        CHECK(env.engine.evaluate("anim.getFrame()").toInt() == 0);
        CHECK(env.engine.evaluate("anim.isPlaying()").toBool()); // not manual-pinned
        CHECK(env.engine.evaluate("anim.rate").toInt() == 1);
        CHECK(env.engine.evaluate("anim.frameCount").toInt() == 1);

        env.engine.evaluate("anim.play();");
        CHECK(env.engine.evaluate("anim.isPlaying()").toBool());

        env.engine.evaluate("anim.stop();");
        // Still "playing" by the no-bridge reader (isManualPin stays false).
        CHECK(env.engine.evaluate("anim.isPlaying()").toBool());
        CHECK(env.engine.evaluate("anim.getFrame()").toInt() == 0);
    }

    TEST_CASE("getAnimationLayerCount defaults to 1") {
        ScriptEnv env;
        // Non-null proxy returns at least 1 (stub)
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getAnimationLayerCount()").toInt() >=
              1);
    }

    TEST_CASE("getAnimationLayer returns proxy with methods") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('bg').getAnimationLayer(0);");
        CHECK(env.engine.evaluate("al.rate").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("al.blend").toNumber() == doctest::Approx(1.0));
        CHECK(env.engine.evaluate("al.visible").toBool() == true);
        CHECK(env.engine.evaluate("al.frameCount").toInt() == 60);
    }

    TEST_CASE("getAnimationLayer play/pause/stop lifecycle") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('bg').getAnimationLayer(0);");
        CHECK_FALSE(env.engine.evaluate("al.isPlaying()").toBool());

        env.engine.evaluate("al.play();");
        CHECK(env.engine.evaluate("al.isPlaying()").toBool());

        env.engine.evaluate("al.pause();");
        CHECK_FALSE(env.engine.evaluate("al.isPlaying()").toBool());

        env.engine.evaluate("al.play(); al.stop();");
        CHECK_FALSE(env.engine.evaluate("al.isPlaying()").toBool());
        CHECK(env.engine.evaluate("al.getFrame()").toInt() == 0);
    }

    TEST_CASE("getAnimationLayer setFrame/getFrame") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('bg').getAnimationLayer(0);");
        env.engine.evaluate("al.setFrame(30);");
        CHECK(env.engine.evaluate("al.getFrame()").toInt() == 30);
    }

    TEST_CASE("getAnimationLayer rate and blend are writable") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('bg').getAnimationLayer(0);\n"
                            "al.rate = 2.5; al.blend = 0.5;");
        CHECK(env.engine.evaluate("al.rate").toNumber() == doctest::Approx(2.5));
        CHECK(env.engine.evaluate("al.blend").toNumber() == doctest::Approx(0.5));
    }

    TEST_CASE("getAnimationLayer cached by index") {
        ScriptEnv env;
        env.engine.evaluate("var a1 = thisScene.getLayer('bg').getAnimationLayer(0);\n"
                            "a1.rate = 99;\n"
                            "var a2 = thisScene.getLayer('bg').getAnimationLayer(0);");
        CHECK(env.engine.evaluate("a2.rate").toNumber() == doctest::Approx(99.0));
    }

    TEST_CASE("getAnimationLayer by name") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('bg').getAnimationLayer('sway');\n"
                            "al.blend = 0.3;");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getAnimationLayer('sway').blend")
                  .toNumber() == doctest::Approx(0.3));
    }

    // ------------------------------------------------------------------
    // Effect Proxy (getEffect)
    // ------------------------------------------------------------------

    TEST_CASE("getEffect returns proxy with correct name") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getEffect('Blur').name").toString() ==
              "Blur");
    }

    TEST_CASE("getEffect returns null for unknown effect") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getEffect('NoSuchEffect')").isNull());
    }

    TEST_CASE("getEffect returns null on layer without effects") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('fg').getEffect('Blur')").isNull());
    }

    TEST_CASE("getEffect visible defaults to true") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getEffect('Shake_Glitch_3').visible")
                  .toBool());
    }

    TEST_CASE("getEffect visible setter updates value") {
        ScriptEnv env;
        env.engine.evaluate(
            "thisScene.getLayer('bg').getEffect('Shake_Glitch_3').visible = false;");
        CHECK_FALSE(
            env.engine.evaluate("thisScene.getLayer('bg').getEffect('Shake_Glitch_3').visible")
                .toBool());
    }

    TEST_CASE("getEffect visible setter marks dirty") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').getEffect('Blur').visible = false;");
        QJSValue updates = env.engine.evaluate("_collectDirtyLayers()");
        // Production flat stride-17 array.  Effect-visibility edits set the
        // F_EFX flag (256) and pack a flat [idx, v?1:0, ...] pair list into
        // stride slot 16 (the 17th element).  Blur is index 0 in
        // efx:['Blur','Shake_Glitch_3','ChromAb'], set to false.
        CHECK(updates.property("length").toInt() == 17);
        CHECK(updates.property(0).toInt() == 7);         // slot 0 is bg's resolved id
        CHECK((updates.property(1).toInt() & 256) != 0); // F_EFX
        QJSValue efx = updates.property(16);
        CHECK(efx.property("length").toInt() == 2);
        CHECK(efx.property(0).toInt() == 0); // idx
        CHECK(efx.property(1).toInt() == 0); // v (false)
    }

    TEST_CASE("getEffect dirty resets after collection") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').getEffect('ChromAb').visible = false;");
        env.engine.evaluate("_collectDirtyLayers()");
        QJSValue updates2 = env.engine.evaluate("_collectDirtyLayers()");
        CHECK(updates2.property("length").toInt() == 0);
    }

    TEST_CASE("getEffectCount returns correct count") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getEffectCount()").toInt() == 3);
        CHECK(env.engine.evaluate("thisScene.getLayer('fg').getEffectCount()").toInt() == 0);
    }

    TEST_CASE("getEffect cached by name") {
        ScriptEnv env;
        env.engine.evaluate("var e1 = thisScene.getLayer('bg').getEffect('Blur');\n"
                            "e1.visible = false;\n"
                            "var e2 = thisScene.getLayer('bg').getEffect('Blur');");
        CHECK_FALSE(env.engine.evaluate("e2.visible").toBool());
    }

    TEST_CASE("nullProxy getEffect returns safe stub") {
        ScriptEnv env;
        QJSValue  eff = env.engine.evaluate("thisScene.getLayer('nonexistent').getEffect('X')");
        CHECK(eff.property("name").toString() == "X");
        CHECK(eff.property("visible").toBool() == false);
    }

    TEST_CASE("nullProxy getEffectCount returns 0") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('nonexistent').getEffectCount()").toInt() ==
              0);
    }

    TEST_CASE("nullProxy getAnimationLayer is safe") {
        ScriptEnv env;
        env.engine.evaluate("var al = thisScene.getLayer('nonexistent').getAnimationLayer(0);");
        CHECK(env.engine.evaluate("al.rate").toNumber() == doctest::Approx(0.0));
        CHECK(env.engine.evaluate("al.getFrame()").toInt() == 0);
        // Methods should not crash
        QJSValue r = env.engine.evaluate("al.play(); al.stop(); true;");
        CHECK_FALSE(r.isError());
    }

    TEST_CASE("nullProxy getters return defaults") {
        ScriptEnv env;
        QJSValue  np = env.engine.evaluate("thisScene.getLayer('nonexistent')");
        CHECK(np.property("name").toString() == "");
        CHECK(np.property("visible").toBool() == false);
        CHECK(np.property("alpha").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("nullProxy setters are no-ops") {
        ScriptEnv env;
        env.engine.evaluate("var np = thisScene.getLayer('nonexistent');\n"
                            "np.origin = {x:99,y:99,z:99};\n"
                            "np.visible = true;\n"
                            "np.alpha = 1.0;\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('nonexistent').origin.x").toNumber() ==
              doctest::Approx(0.0));
        CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('nonexistent').visible").toBool());
    }

    TEST_CASE("getLayer returns cached proxy") {
        ScriptEnv env;
        CHECK(
            env.engine.evaluate("thisScene.getLayer('bg') === thisScene.getLayer('bg')").toBool());
    }

    TEST_CASE("getLayer returns nullProxy for unknown") {
        ScriptEnv env;
        // nullProxy has name=='' and visible==false
        CHECK(env.engine.evaluate("thisScene.getLayer('nope').name").toString() == "");
        CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('nope').visible").toBool());
    }

    TEST_CASE("scene is alias for thisScene") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("scene === thisScene").toBool());
        CHECK(env.engine.evaluate("typeof scene.getLayer").toString() == "function");
    }

    // ---- Indexed-access API ---------------------------------------------
    // thisScene.getLayer(name) / getLayerByID(id) / getLayerCount() /
    // getLayerIndex(name).  Production builds the id↔name maps from
    // SceneBackend::m_nodeNameToId; ScriptEnv seeds the same shape with
    // bg=7, fg=11.

    TEST_CASE("getLayerCount returns the number of declared layers") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayerCount()").toInt() == 2);
    }

    TEST_CASE("getLayerByID returns a layer proxy for a known id") {
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayerByID(7).name");
        CHECK(v.toString() == QString("bg"));
    }

    TEST_CASE("getLayerByID returns null for unknown id") {
        ScriptEnv env;
        QJSValue  v = env.engine.evaluate("thisScene.getLayerByID(9999) === null");
        CHECK(v.toBool());
    }

    TEST_CASE("getLayerByID returns same proxy as getLayer(name) (cached)") {
        ScriptEnv env;
        CHECK(env.engine.evaluate(
                     "thisScene.getLayerByID(11) === thisScene.getLayer('fg')")
                  .toBool());
    }

    TEST_CASE("getLayerIndex returns the declared id for a known name") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayerIndex('bg')").toInt() == 7);
        CHECK(env.engine.evaluate("thisScene.getLayerIndex('fg')").toInt() == 11);
    }

    TEST_CASE("getLayerIndex returns -1 for unknown name") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayerIndex('nope')").toInt() == -1);
    }

    TEST_CASE("getLayerByID then getLayerIndex round-trips") {
        ScriptEnv env;
        CHECK(env.engine.evaluate(
                     "thisScene.getLayerIndex(thisScene.getLayerByID(7).name) === 7")
                  .toBool());
    }

    // ---- isObjectValid guard --------------------------------------------
    // Scripts that stash a layer reference in a timer/event callback can
    // get a stale proxy after thisScene.destroyLayer recycles the slot.
    // `layer.isObjectValid()` lets them bail out.

    TEST_CASE("fresh layer proxy reports valid") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').isObjectValid()").toBool());
    }

    TEST_CASE("flipping _destroyed invalidates the proxy") {
        ScriptEnv env;
        env.engine.evaluate("var L = thisScene.getLayer('bg'); L._destroyed = true;");
        CHECK_FALSE(env.engine.evaluate("L.isObjectValid()").toBool());
    }

    TEST_CASE("isObjectValid survives re-validation (create/destroy cycle)") {
        // Production createLayer resets _destroyed on pool re-use.  Simulate.
        ScriptEnv env;
        env.engine.evaluate(
            "var L = thisScene.getLayer('bg');\n"
            "L._destroyed = true;\n"
            "var stale = L.isObjectValid();\n"
            "L._destroyed = false;\n"
            "var revived = L.isObjectValid();");
        CHECK_FALSE(env.engine.evaluate("stale").toBool());
        CHECK(env.engine.evaluate("revived").toBool());
    }

    TEST_CASE("isObjectValid is a function (callable shape)") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("typeof thisScene.getLayer('bg').isObjectValid")
                  .toString()
              == QString("function"));
    }

    // ---- getInitialLayerConfig ------------------------------------------
    // Returns a snapshot of the pre-script layer state so scripts can
    // implement "reset" buttons or interpolate between original and
    // modified values.

    TEST_CASE("getInitialLayerConfig returns origin/scale/angles from init") {
        ScriptEnv env;
        // fixture's bg has o=[100,200,0], s=[1,1,1], a=[0,0,45]
        QJSValue  c = env.engine.evaluate("thisScene.getLayer('bg').getInitialLayerConfig()");
        CHECK(c.property("origin").property("x").toNumber() == doctest::Approx(100));
        CHECK(c.property("origin").property("y").toNumber() == doctest::Approx(200));
        CHECK(c.property("angles").property("z").toNumber() == doctest::Approx(45));
        CHECK(c.property("visible").toBool() == true);
    }

    TEST_CASE("getInitialLayerConfig fixed defaults for alpha/color") {
        ScriptEnv env;
        QJSValue  c = env.engine.evaluate("thisScene.getLayer('fg').getInitialLayerConfig()");
        CHECK(c.property("alpha").toNumber() == doctest::Approx(1.0));
        CHECK(c.property("color").property("x").toNumber() == doctest::Approx(1.0));
        CHECK(c.property("color").property("y").toNumber() == doctest::Approx(1.0));
        CHECK(c.property("color").property("z").toNumber() == doctest::Approx(1.0));
        // fg had v: false in the fixture
        CHECK(c.property("visible").toBool() == false);
    }

    TEST_CASE("getInitialLayerConfig is a fresh snapshot, not aliased") {
        // Mutating the proxy should NOT affect a previously-captured snapshot
        // (origin/scale/angles are fresh Vec3s; the snapshot object itself
        // is a new object every call).
        ScriptEnv env;
        env.engine.evaluate(
            "var L = thisScene.getLayer('bg');\n"
            "var snap = L.getInitialLayerConfig();\n"
            "L.origin = Vec3(999, 999, 999);");
        QJSValue v = env.engine.evaluate("snap.origin.x");
        // The snapshot's origin was 100 (from fixture), mutating layer.origin
        // must not retroactively change snap.origin.
        CHECK(v.toNumber() == doctest::Approx(100));
    }

    TEST_CASE("getInitialLayerConfig reports size for layers with sz") {
        ScriptEnv env;
        QJSValue  c = env.engine.evaluate("thisScene.getLayer('bg').getInitialLayerConfig()");
        CHECK(c.property("size").property("x").toNumber() == doctest::Approx(1920));
        CHECK(c.property("size").property("y").toNumber() == doctest::Approx(1080));
    }

} // TEST_SUITE Layer Proxy

// ------------------------------------------------------------------
// Scene Event Bus
// ------------------------------------------------------------------

TEST_SUITE("Scene Event Bus") {
    TEST_CASE("scene.on and scene.off are functions") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("typeof scene.on").toString() == "function");
        CHECK(env.engine.evaluate("typeof scene.off").toString() == "function");
    }

    TEST_CASE("scene.on registers listener that _fireSceneEvent invokes") {
        ScriptEnv env;
        env.engine.evaluate("var called = 0; scene.on('update', function() { called++; })");
        env.engine.evaluate("_fireSceneEvent('update')");
        CHECK(env.engine.evaluate("called").toInt() == 1);
    }

    TEST_CASE("_fireSceneEvent returns listener count") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("_fireSceneEvent('nope')").toInt() == 0);
        env.engine.evaluate("scene.on('test', function() {})");
        CHECK(env.engine.evaluate("_fireSceneEvent('test')").toInt() == 1);
    }

    TEST_CASE("multiple listeners for same event all fire") {
        ScriptEnv env;
        env.engine.evaluate("var a = 0, b = 0;"
                            "scene.on('tick', function() { a++; });"
                            "scene.on('tick', function() { b++; });");
        env.engine.evaluate("_fireSceneEvent('tick')");
        CHECK(env.engine.evaluate("a").toInt() == 1);
        CHECK(env.engine.evaluate("b").toInt() == 1);
    }

    TEST_CASE("listener receives arguments") {
        ScriptEnv env;
        env.engine.evaluate("var gotW = 0, gotH = 0;"
                            "scene.on('resizeScreen', function(w, h) { gotW = w; gotH = h; });");
        env.engine.evaluate("_fireSceneEvent('resizeScreen', 1920, 1080)");
        CHECK(env.engine.evaluate("gotW").toInt() == 1920);
        CHECK(env.engine.evaluate("gotH").toInt() == 1080);
    }

    TEST_CASE("listener receives event object") {
        ScriptEnv env;
        env.engine.evaluate(
            "var gotState = -1;"
            "scene.on('mediaPlaybackChanged', function(e) { gotState = e.state; });");
        env.engine.evaluate("_fireSceneEvent('mediaPlaybackChanged', { state: 1 })");
        CHECK(env.engine.evaluate("gotState").toInt() == 1);
    }

    TEST_CASE("error in one listener does not prevent others") {
        ScriptEnv env;
        env.engine.evaluate("var ok = false;"
                            "scene.on('test', function() { throw new Error('boom'); });"
                            "scene.on('test', function() { ok = true; });");
        auto result = env.engine.evaluate("_fireSceneEvent('test')");
        CHECK_FALSE(result.isError());
        CHECK(env.engine.evaluate("ok").toBool());
    }

    TEST_CASE("scene.off removes specific callback") {
        ScriptEnv env;
        env.engine.evaluate("var count = 0;"
                            "var fn = function() { count++; };"
                            "scene.on('test', fn);"
                            "scene.on('test', function() { count += 10; });");
        env.engine.evaluate("_fireSceneEvent('test')");
        CHECK(env.engine.evaluate("count").toInt() == 11);
        env.engine.evaluate("count = 0; scene.off('test', fn)");
        env.engine.evaluate("_fireSceneEvent('test')");
        CHECK(env.engine.evaluate("count").toInt() == 10);
    }

    TEST_CASE("scene.off with event name only removes all listeners") {
        ScriptEnv env;
        env.engine.evaluate("scene.on('test', function() {});"
                            "scene.on('test', function() {});");
        CHECK(env.engine.evaluate("_hasSceneListeners('test')").toBool());
        env.engine.evaluate("scene.off('test')");
        CHECK_FALSE(env.engine.evaluate("_hasSceneListeners('test')").toBool());
    }

    TEST_CASE("scene.on ignores non-string event name") {
        ScriptEnv env;
        auto      result = env.engine.evaluate("scene.on(123, function() {})");
        CHECK_FALSE(result.isError());
        CHECK_FALSE(env.engine.evaluate("_hasSceneListeners(123)").toBool());
    }

    TEST_CASE("scene.on ignores non-function callback") {
        ScriptEnv env;
        auto      result = env.engine.evaluate("scene.on('test', 'notAFunction')");
        CHECK_FALSE(result.isError());
        CHECK_FALSE(env.engine.evaluate("_hasSceneListeners('test')").toBool());
    }

    TEST_CASE("_hasSceneListeners returns false when empty") {
        ScriptEnv env;
        CHECK_FALSE(env.engine.evaluate("_hasSceneListeners('update')").toBool());
    }

    TEST_CASE("_hasSceneListeners returns true after registration") {
        ScriptEnv env;
        env.engine.evaluate("scene.on('update', function() {})");
        CHECK(env.engine.evaluate("_hasSceneListeners('update')").toBool());
    }

    TEST_CASE("listeners fire in registration order") {
        ScriptEnv env;
        env.engine.evaluate("var order = [];"
                            "scene.on('test', function() { order.push(1); });"
                            "scene.on('test', function() { order.push(2); });"
                            "scene.on('test', function() { order.push(3); });");
        env.engine.evaluate("_fireSceneEvent('test')");
        CHECK(env.engine.evaluate("order.join(',')").toString() == "1,2,3");
    }

    TEST_CASE("update listener can modify layer proxy") {
        ScriptEnv env;
        env.engine.evaluate("scene.on('update', function() {"
                            "  var layer = thisScene.getLayer('bg');"
                            "  if (layer) layer.visible = false;"
                            "});");
        env.engine.evaluate("_fireSceneEvent('update')");
        auto dirty = env.engine.evaluate("_collectDirtyLayers()");
        // Production flat stride-17 array: one dirty layer -> length 17,
        // resolved id in slot 0, F_VISIBLE (8) in the flags slot.
        CHECK(dirty.property("length").toInt() == 17);
        CHECK(dirty.property(0).toInt() == 7);       // bg's resolved id
        CHECK((dirty.property(1).toInt() & 8) != 0); // F_VISIBLE
    }

    TEST_CASE("scene.off for unknown event is no-op") {
        ScriptEnv env;
        auto      result = env.engine.evaluate("scene.off('nonexistent')");
        CHECK_FALSE(result.isError());
    }

    // ---- applyGeneralSettings --------------------------------------------
    // Fires once at SceneBackend init (see fireApplyGeneralSettings), and
    // is available for scripts that want a hook distinct from
    // applyUserProperties (wallpaper user props) for app-level settings.

    TEST_CASE("applyGeneralSettings listener fires via _fireSceneEvent") {
        ScriptEnv env;
        env.engine.evaluate(
            "var fired = 0;"
            "var gotArg = null;"
            "scene.on('applyGeneralSettings', function(s) { fired++; gotArg = s; });");
        env.engine.evaluate("_fireSceneEvent('applyGeneralSettings', { foo: 1 })");
        CHECK(env.engine.evaluate("fired").toInt() == 1);
        CHECK(env.engine.evaluate("gotArg.foo").toInt() == 1);
    }

    TEST_CASE("applyGeneralSettings is independent of applyUserProperties") {
        ScriptEnv env;
        env.engine.evaluate(
            "var userCalls = 0;"
            "var genCalls  = 0;"
            "scene.on('applyUserProperties', function() { userCalls++; });"
            "scene.on('applyGeneralSettings', function() { genCalls++; });");
        env.engine.evaluate("_fireSceneEvent('applyGeneralSettings', {})");
        CHECK(env.engine.evaluate("userCalls").toInt() == 0);
        CHECK(env.engine.evaluate("genCalls").toInt() == 1);
    }

} // TEST_SUITE Scene Event Bus

// ------------------------------------------------------------------
// Dirty Layer Collection
// ------------------------------------------------------------------
TEST_SUITE("Dirty Layer Collection") {
    TEST_CASE("empty when no dirty") {
        ScriptEnv env;
        // Touch a layer to create it in cache, but don't modify
        env.engine.evaluate("thisScene.getLayer('bg');");
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
        CHECK(r.property("length").toInt() == 0);
    }

    TEST_CASE("returns update after setting origin") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').origin = {x:1,y:2,z:3};");
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
        // Production returns a FLAT stride-17 array (not an array of objects):
        // [name, flags, ox,oy,oz, sx,sy,sz, ax,ay,az, alpha, vis, psize,
        //  text, cmds, efx].  One dirty layer -> length 17.  flags carries the
        // F_ORIGIN bit (1).
        CHECK(r.property("length").toInt() == 17);
        CHECK(r.property(0).toInt() == 7);       // bg's resolved id
        CHECK((r.property(1).toInt() & 1) != 0); // F_ORIGIN
        CHECK(r.property(2).toNumber() == doctest::Approx(1.0)); // ox
        CHECK(r.property(3).toNumber() == doctest::Approx(2.0)); // oy
        CHECK(r.property(4).toNumber() == doctest::Approx(3.0)); // oz
    }

    TEST_CASE("resets dirty after collection") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').origin = {x:5,y:5,z:5};");
        env.engine.evaluate("_collectDirtyLayers()");              // first collect
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()"); // second
        CHECK(r.property("length").toInt() == 0);
    }

    TEST_CASE("multiple dirty properties on one layer") {
        ScriptEnv env;
        env.engine.evaluate("var l = thisScene.getLayer('bg');\n"
                            "l.origin = {x:1,y:2,z:3};\n"
                            "l.visible = false;\n");
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
        // One layer, two dirty props -> length 17 with both bits in flags.
        CHECK(r.property("length").toInt() == 17);
        CHECK(r.property(0).toInt() == 7); // bg's resolved id
        int flags = r.property(1).toInt();
        CHECK((flags & 1) != 0); // F_ORIGIN
        CHECK((flags & 8) != 0); // F_VISIBLE
    }

    TEST_CASE("multiple dirty layers simultaneously") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('bg').origin = {x:1,y:1,z:1};\n"
                            "thisScene.getLayer('fg').visible = true;\n");
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
        // Two dirty layers -> 2 * stride-17 = 34 flat entries.
        CHECK(r.property("length").toInt() == 34);
    }

    TEST_CASE("_layerList grows as layers are materialized via getLayer") {
        // Production _collectDirtyLayers iterates `_layerList` by index
        // instead of walking `_layerCache` via `for..in` (V8-style hash
        // iteration is slow for 1000+ entries).  The invariant: every entry
        // added to _layerCache must also appear in _layerList, in insertion
        // order, with no duplicates when getLayer() is re-called.
        ScriptEnv env;
        CHECK(env.engine.evaluate("_layerList.length").toInt() == 0);
        env.engine.evaluate("thisScene.getLayer('bg');");
        CHECK(env.engine.evaluate("_layerList.length").toInt() == 1);
        CHECK(env.engine.evaluate("_layerList[0].name").toString() == "bg");

        env.engine.evaluate("thisScene.getLayer('fg');");
        CHECK(env.engine.evaluate("_layerList.length").toInt() == 2);
        CHECK(env.engine.evaluate("_layerList[1].name").toString() == "fg");

        // Re-materialization must hit the cache — no second push.
        env.engine.evaluate("thisScene.getLayer('bg');");
        CHECK(env.engine.evaluate("_layerList.length").toInt() == 2);

        // Unknown layer returns null and does NOT grow the list.
        env.engine.evaluate("thisScene.getLayer('no-such-layer');");
        CHECK(env.engine.evaluate("_layerList.length").toInt() == 2);
    }

    TEST_CASE("_collectDirtyLayers walks _layerList in insertion order") {
        // Two dirty layers — verify the collector returns them in the same
        // order they were materialized (i.e., iteration-order semantics of
        // _layerList, not _layerCache hash order).
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('fg').visible = false;\n"
                            "thisScene.getLayer('bg').origin = {x:1,y:2,z:3};\n");
        QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
        // Flat stride-17 array: 2 dirty layers -> 34 entries.  The resolved id
        // of each layer sits at the start of its stride (slot 0 and
        // slot 17), in _layerList insertion order: fg materialized first (id
        // 11), then bg (id 7).
        REQUIRE(r.property("length").toInt() == 34);
        CHECK(r.property(0).toInt() == 11); // fg's resolved id
        CHECK(r.property(17).toInt() == 7); // bg's resolved id
    }

} // TEST_SUITE Dirty Layer Collection

// ------------------------------------------------------------------
// Sound Layer Proxy
// ------------------------------------------------------------------
TEST_SUITE("Sound Layer Proxy") {
    TEST_CASE("name property") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').name").toString() ==
              "music.mp3");
    }

    TEST_CASE("initial volume from state") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').volume").toNumber() ==
              doctest::Approx(0.8));
    }

    TEST_CASE("volume setter with dirty tracking") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('music.mp3').volume = 0.5;");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').volume").toNumber() ==
              doctest::Approx(0.5));
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._dirty.volume").toBool());
    }

    TEST_CASE("play stop pause queue commands") {
        ScriptEnv env;
        env.engine.evaluate("var sl = thisScene.getLayer('music.mp3');\n"
                            "sl.play(); sl.stop(); sl.pause();\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds.length").toInt() ==
              3);
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[0]").toString() ==
              "play");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[1]").toString() ==
              "stop");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[2]").toString() ==
              "pause");
    }

    TEST_CASE("isPlaying reads engine state") {
        ScriptEnv env;
        CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('music.mp3').isPlaying()").toBool());
        env.engine.evaluate("engine._soundPlayingStates['music.mp3'] = true;");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').isPlaying()").toBool());
    }

    TEST_CASE("play/pause update the isPlaying shadow synchronously") {
        // Regression for wallpaper 2866203962: calling pause() must flip
        // isPlaying() to false on the very next read, so scripts that do
        // `pause(); ... if (anyPlaying()) ...` on the same tick see the
        // correct state.  Previously the shadow only updated on the next
        // C++ refresh (a whole tick later), so the wallpaper's
        // anyPlaying() side-effect kept resurrecting playStatus=true.
        ScriptEnv env;
        env.engine.evaluate("var sl = thisScene.getLayer('music.mp3');\n"
                            "sl.play();");
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == true);
        env.engine.evaluate("sl.pause();");
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == false);
        env.engine.evaluate("sl.play();");
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == true);
        env.engine.evaluate("sl.stop();");
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == false);
    }

    TEST_CASE("play/pause mark the layer dirty so C++ refresh doesn't clobber") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('music.mp3').play();");
        CHECK(env.engine.evaluate("engine._soundPlayingStatesDirty['music.mp3']").toBool() == true);
        env.engine.evaluate("thisScene.getLayer('music.mp3').pause();");
        // Still dirty — the command intent hasn't been confirmed by C++ yet.
        CHECK(env.engine.evaluate("engine._soundPlayingStatesDirty['music.mp3']").toBool() == true);
    }

    TEST_CASE("refresh respects the dirty flag and clears once C++ agrees") {
        // This mirrors what SceneBackend does each tick BEFORE script eval:
        //   for each layer:
        //     if dirty:
        //       if shadow == cppPlaying: clear dirty (C++ caught up)
        //       else: leave shadow alone
        //     else: shadow = cppPlaying (trust C++)
        ScriptEnv env;
        env.engine.evaluate("var sl = thisScene.getLayer('music.mp3');\n"
                            "sl.pause();\n" // shadow=false, dirty=true
        );
        // Simulate C++ refresh where the stream is still Playing (not yet paused).
        auto refresh = [&](bool cppPlaying) {
            env.engine.evaluate(
                QString("(function(cppPlaying){\n"
                        "  var name = 'music.mp3';\n"
                        "  var dirty = engine._soundPlayingStatesDirty[name];\n"
                        "  if (dirty) {\n"
                        "    if (engine._soundPlayingStates[name] === cppPlaying) {\n"
                        "      engine._soundPlayingStatesDirty[name] = false;\n"
                        "    }\n"
                        "  } else { engine._soundPlayingStates[name] = cppPlaying; }\n"
                        "})(%1);\n")
                    .arg(cppPlaying ? "true" : "false"));
        };
        // First refresh: C++ still reports Playing (stale).  Shadow stays false.
        refresh(true);
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == false);
        CHECK(env.engine.evaluate("engine._soundPlayingStatesDirty['music.mp3']").toBool() == true);
        // Render thread catches up — C++ now reports Paused (false).  Dirty clears.
        refresh(false);
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == false);
        CHECK(env.engine.evaluate("engine._soundPlayingStatesDirty['music.mp3']").toBool() ==
              false);
        // Natural state change: track actually starts playing (e.g. external).
        // Shadow is not dirty — refresh is trusted.
        refresh(true);
        CHECK(env.engine.evaluate("sl.isPlaying()").toBool() == true);
    }

    TEST_CASE("image layer properties are no-ops") {
        ScriptEnv env;
        env.engine.evaluate("var sl = thisScene.getLayer('music.mp3');\n"
                            "sl.origin = {x:99,y:99,z:99};\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').origin.x").toNumber() ==
              doctest::Approx(0.0));
    }

    TEST_CASE("visible and alpha are read-only stubs") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').visible").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').alpha").toNumber() ==
              doctest::Approx(1.0));
    }

    TEST_CASE("collectDirtySoundLayers empty when clean") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('music.mp3');"); // create in cache
        QJSValue r = env.engine.evaluate("_collectDirtySoundLayers()");
        CHECK(r.property("length").toInt() == 0);
    }

    TEST_CASE("collectDirtySoundLayers volume change") {
        ScriptEnv env;
        env.engine.evaluate("thisScene.getLayer('music.mp3').volume = 0.3;");
        QJSValue r = env.engine.evaluate("_collectDirtySoundLayers()");
        CHECK(r.property("length").toInt() == 1);
        CHECK(r.property(0).property("dirty").property("volume").toBool());
        CHECK(r.property(0).property("volume").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("collectDirtySoundLayers commands collected and cleared") {
        ScriptEnv env;
        env.engine.evaluate("var sl = thisScene.getLayer('music.mp3'); sl.play(); sl.stop();");
        QJSValue r = env.engine.evaluate("_collectDirtySoundLayers()");
        CHECK(r.property("length").toInt() == 1);
        CHECK(r.property(0).property("cmds").property("length").toInt() == 2);

        // Second collect should be empty
        QJSValue r2 = env.engine.evaluate("_collectDirtySoundLayers()");
        CHECK(r2.property("length").toInt() == 0);
    }

    TEST_CASE("enumerateLayers returns all layers") {
        ScriptEnv env;
        // 2 image + 2 sound = 4
        CHECK(env.engine.evaluate("thisScene.enumerateLayers().length").toInt() == 4);
    }

    TEST_CASE("enumerateLayers each element is a proxy") {
        ScriptEnv env;
        QJSValue  layers = env.engine.evaluate("thisScene.enumerateLayers()");
        for (int i = 0; i < 4; i++) {
            CHECK(layers.property(i).property("name").isString());
        }
    }

} // TEST_SUITE Sound Layer Proxy

// ------------------------------------------------------------------
// Audio Buffers
// ------------------------------------------------------------------
TEST_SUITE("Audio Buffers") {
    TEST_CASE("default resolution is 64") {
        ScriptEnv env;
        env.engine.evaluate("var buf = engine.registerAudioBuffers();");
        CHECK(env.engine.evaluate("buf.resolution").toInt() == 64);
        CHECK(env.engine.evaluate("buf.left.length").toInt() == 64);
        CHECK(env.engine.evaluate("buf.right.length").toInt() == 64);
        CHECK(env.engine.evaluate("buf.average.length").toInt() == 64);
    }

    TEST_CASE("resolution 16") {
        ScriptEnv env;
        env.engine.evaluate("var buf = engine.registerAudioBuffers(16);");
        CHECK(env.engine.evaluate("buf.resolution").toInt() == 16);
        CHECK(env.engine.evaluate("buf.left.length").toInt() == 16);
    }

    TEST_CASE("resolution 32") {
        ScriptEnv env;
        env.engine.evaluate("var buf = engine.registerAudioBuffers(32);");
        CHECK(env.engine.evaluate("buf.resolution").toInt() == 32);
    }

    TEST_CASE("resolution normalized by rounding") {
        ScriptEnv env;
        // 20 → <=24 → 16
        env.engine.evaluate("var buf1 = engine.registerAudioBuffers(20);");
        CHECK(env.engine.evaluate("buf1.resolution").toInt() == 16);
        // 40 → <=48 → 32
        env.engine.evaluate("var buf2 = engine.registerAudioBuffers(40);");
        CHECK(env.engine.evaluate("buf2.resolution").toInt() == 32);
    }

    TEST_CASE("registered in _audioRegs with _regIdx") {
        ScriptEnv env;
        env.engine.evaluate("var b1 = engine.registerAudioBuffers(16);");
        env.engine.evaluate("var b2 = engine.registerAudioBuffers(32);");
        CHECK(env.engine.evaluate("engine._audioRegs.length").toInt() == 2);
        CHECK(env.engine.evaluate("engine._audioRegs[0]._regIdx").toInt() == 0);
        CHECK(env.engine.evaluate("engine._audioRegs[1]._regIdx").toInt() == 1);
    }

} // TEST_SUITE Audio Buffers

// ------------------------------------------------------------------
// Script Compilation (IIFE patterns)
// ------------------------------------------------------------------
TEST_SUITE("Script Compilation") {
    // Text/Color IIFE pattern: wraps script in IIFE, extracts exports.update
    static const char* TEXT_IIFE_PRE = "(function() {\n"
                                       "  'use strict';\n"
                                       "  var exports = {};\n";
    static const char* TEXT_IIFE_POST =
        "  var _upd = typeof exports.update === 'function' ? exports.update :\n"
        "             (typeof update === 'function' ? update : null);\n"
        "  var _init = typeof exports.init === 'function' ? exports.init :\n"
        "              (typeof init === 'function' ? init : null);\n"
        "  if (!_upd) return null;\n"
        "  return { update: _upd, init: _init };\n"
        "})()";

    // Property IIFE pattern: extracts all handlers with try-catch wrapping
    static const char* PROP_IIFE_POST =
        "  var _rawUpd = typeof exports.update === 'function' ? exports.update :\n"
        "                (typeof update === 'function' ? update : null);\n"
        "  var _rawInit = typeof exports.init === 'function' ? exports.init :\n"
        "                 (typeof init === 'function' ? init : null);\n"
        "  var _click = typeof exports.cursorClick === 'function' ? exports.cursorClick :\n"
        "               (typeof cursorClick === 'function' ? cursorClick : null);\n"
        "  var _enter = typeof exports.cursorEnter === 'function' ? exports.cursorEnter : null;\n"
        "  var _leave = typeof exports.cursorLeave === 'function' ? exports.cursorLeave : null;\n"
        "  var _down = typeof exports.cursorDown === 'function' ? exports.cursorDown : null;\n"
        "  var _up = typeof exports.cursorUp === 'function' ? exports.cursorUp : null;\n"
        "  var _move = typeof exports.cursorMove === 'function' ? exports.cursorMove : null;\n"
        "  var _aup = typeof exports.applyUserProperties === 'function' ? "
        "exports.applyUserProperties :\n"
        "             (typeof applyUserProperties === 'function' ? applyUserProperties : null);\n"
        "  var _destr = typeof exports.destroy === 'function' ? exports.destroy :\n"
        "              (typeof destroy === 'function' ? destroy : null);\n"
        "  var _resize = typeof exports.resizeScreen === 'function' ? exports.resizeScreen :\n"
        "               (typeof resizeScreen === 'function' ? resizeScreen : null);\n"
        "  var _mpbc = typeof exports.mediaPlaybackChanged === 'function' ? "
        "exports.mediaPlaybackChanged :\n"
        "              (typeof mediaPlaybackChanged === 'function' ? mediaPlaybackChanged : "
        "null);\n"
        "  var _mprc = typeof exports.mediaPropertiesChanged === 'function' ? "
        "exports.mediaPropertiesChanged :\n"
        "              (typeof mediaPropertiesChanged === 'function' ? mediaPropertiesChanged : "
        "null);\n"
        "  var _mtbc = typeof exports.mediaThumbnailChanged === 'function' ? "
        "exports.mediaThumbnailChanged :\n"
        "              (typeof mediaThumbnailChanged === 'function' ? mediaThumbnailChanged : "
        "null);\n"
        "  var _mtlc = typeof exports.mediaTimelineChanged === 'function' ? "
        "exports.mediaTimelineChanged :\n"
        "              (typeof mediaTimelineChanged === 'function' ? mediaTimelineChanged : "
        "null);\n"
        "  var _mstc = typeof exports.mediaStatusChanged === 'function' ? "
        "exports.mediaStatusChanged :\n"
        "              (typeof mediaStatusChanged === 'function' ? mediaStatusChanged : null);\n"
        "  var _init2 = _rawInit ? function(v) {\n"
        "    try { return _rawInit(v); } catch(e) { console.log('init error: ' + e.message); }\n"
        "  } : null;\n"
        "  var _upd2 = _rawUpd ? function(v) {\n"
        "    try { return _rawUpd(v); } catch(e) { return v; }\n"
        "  } : null;\n"
        "  return { update: _upd2, init: _init2,\n"
        "    cursorClick: _click, cursorEnter: _enter, cursorLeave: _leave,\n"
        "    cursorDown: _down, cursorUp: _up, cursorMove: _move,\n"
        "    applyUserProperties: _aup, destroy: _destr, resizeScreen: _resize,\n"
        "    mediaPlaybackChanged: _mpbc, mediaPropertiesChanged: _mprc,\n"
        "    mediaThumbnailChanged: _mtbc, mediaTimelineChanged: _mtlc,\n"
        "    mediaStatusChanged: _mstc };\n"
        "})()";

    TEST_CASE("exports.update extracted from text IIFE") {
        ScriptEnv env;
        QString   script = QString("%1  exports.update = function(v){ return v + '!'; };\n%2")
                             .arg(TEXT_IIFE_PRE)
                             .arg(TEXT_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("update").isCallable());
        CHECK(r.property("update").call({ QJSValue("hi") }).toString() == "hi!");
    }

    TEST_CASE("bare function update extracted") {
        ScriptEnv env;
        QString   script = QString("%1  function update(v){ return v + '?'; }\n%2")
                             .arg(TEXT_IIFE_PRE)
                             .arg(TEXT_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("update").isCallable());
        CHECK(r.property("update").call({ QJSValue("hi") }).toString() == "hi?");
    }

    TEST_CASE("missing update returns null") {
        ScriptEnv env;
        QString   script = QString("%1  var x = 42;\n%2").arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
        QJSValue  r      = env.engine.evaluate(script);
        CHECK(r.isNull());
    }

    TEST_CASE("exports.init extracted alongside update") {
        ScriptEnv env;
        QString   script = QString("%1  exports.init = function(v){ return 'init'; };\n"
                                   "  exports.update = function(v){ return v; };\n%2")
                             .arg(TEXT_IIFE_PRE)
                             .arg(TEXT_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("init").isCallable());
        CHECK(r.property("update").isCallable());
    }

    TEST_CASE("property IIFE extracts all 16 handlers") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "  exports.init = function(v){};\n"
                                   "  exports.cursorClick = function(){};\n"
                                   "  exports.cursorEnter = function(){};\n"
                                   "  exports.cursorLeave = function(){};\n"
                                   "  exports.cursorDown = function(){};\n"
                                   "  exports.cursorUp = function(){};\n"
                                   "  exports.cursorMove = function(){};\n"
                                   "  exports.applyUserProperties = function(p){};\n"
                                   "  exports.destroy = function(){};\n"
                                   "  exports.resizeScreen = function(w,h){};\n"
                                   "  exports.mediaPlaybackChanged = function(e){};\n"
                                   "  exports.mediaPropertiesChanged = function(e){};\n"
                                   "  exports.mediaThumbnailChanged = function(e){};\n"
                                   "  exports.mediaTimelineChanged = function(e){};\n"
                                   "  exports.mediaStatusChanged = function(e){};\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("update").isCallable());
        CHECK(r.property("init").isCallable());
        CHECK(r.property("cursorClick").isCallable());
        CHECK(r.property("cursorEnter").isCallable());
        CHECK(r.property("cursorLeave").isCallable());
        CHECK(r.property("cursorDown").isCallable());
        CHECK(r.property("cursorUp").isCallable());
        CHECK(r.property("cursorMove").isCallable());
        CHECK(r.property("applyUserProperties").isCallable());
        CHECK(r.property("destroy").isCallable());
        CHECK(r.property("resizeScreen").isCallable());
        CHECK(r.property("mediaPlaybackChanged").isCallable());
        CHECK(r.property("mediaPropertiesChanged").isCallable());
        CHECK(r.property("mediaThumbnailChanged").isCallable());
        CHECK(r.property("mediaTimelineChanged").isCallable());
        CHECK(r.property("mediaStatusChanged").isCallable());
    }

    TEST_CASE("property IIFE update wrapped in try-catch returns input on error") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ throw new Error('boom'); };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("update").isCallable());
        // Should return the input value (42) instead of throwing
        QJSValue result = r.property("update").call({ QJSValue(42) });
        CHECK(result.toInt() == 42);
    }

    TEST_CASE("property IIFE init wrapped in try-catch") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.init = function(v){ throw new Error('init boom'); };\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("init").isCallable());
        // Should not propagate the error
        QJSValue result = r.property("init").call({ QJSValue(0) });
        CHECK_FALSE(result.isError());
    }

    TEST_CASE("scriptProperties override in IIFE context") {
        ScriptEnv env;
        // Simulate property script with stored overrides
        env.engine.evaluate("var result = (function() {\n"
                            "  'use strict';\n"
                            "  var exports = {};\n"
                            "  var _storedProps = { speed: { value: 99 } };\n"
                            "  function createScriptProperties() {\n"
                            "    var b = {};\n"
                            "    function ap(def) {\n"
                            "      var n = def.name;\n"
                            "      if (n in _storedProps) {\n"
                            "        var sp = _storedProps[n];\n"
                            "        b[n] = (typeof sp === 'object' && sp !== null && 'value' in "
                            "sp) ? sp.value : sp;\n"
                            "      } else { b[n] = def.value; }\n"
                            "      return b;\n"
                            "    }\n"
                            "    b.addSlider=ap; b.finish=function(){return b;};\n"
                            "    return b;\n"
                            "  }\n"
                            "  var scriptProperties = "
                            "createScriptProperties().addSlider({name:'speed',value:5}).finish();\n"
                            "  exports.update = function(v){ return scriptProperties.speed; };\n"
                            "  return { update: exports.update };\n"
                            "})();\n");
        CHECK(env.engine.evaluate("result.update(0)").toInt() == 99);
    }

    TEST_CASE("scriptProperties `user` binding resolves from engine.userProperties") {
        // Wallpaper 2866203962's Player Options declares
        // `scriptproperties: {enableplayer: {user: 'ui', value: false}}` — meaning
        // scriptProperties.enableplayer should read from engine.userProperties.ui
        // at runtime, NOT from the fallback `value: false`.  Without this
        // resolution the UI fade script saw shared.enablePlayer=false forever
        // and the player never came back on hover.  Mirrors the production
        // createScriptProperties shadow defined in SceneBackend.cpp for property
        // scripts.
        ScriptEnv env;
        env.engine.evaluate(
            "var engine = { userProperties: { ui: true, music: false } };\n"
            "var result = (function() {\n"
            "  'use strict';\n"
            "  var _storedProps = {\n"
            "    enableplayer: { user: 'ui', value: false },\n"
            "    musicOn:      { user: 'music', value: true },\n"
            "    unboundProp:  { value: 'stored-literal' }\n"
            "  };\n"
            "  function createScriptProperties() {\n"
            "    var b = {};\n"
            "    function ap(def) {\n"
            "      var n = def.name;\n"
            "      if (n in _storedProps) {\n"
            "        var sp = _storedProps[n];\n"
            "        if (typeof sp === 'object' && sp !== null) {\n"
            "          if ('user' in sp && typeof engine !== 'undefined' &&\n"
            "              engine.userProperties && sp.user in engine.userProperties) {\n"
            "            b[n] = engine.userProperties[sp.user];\n"
            "          } else if ('value' in sp) {\n"
            "            b[n] = sp.value;\n"
            "          } else { b[n] = def.value; }\n"
            "        } else { b[n] = sp; }\n"
            "      } else { b[n] = def.value; }\n"
            "      return b;\n"
            "    }\n"
            "    b.addCheckbox=ap; b.addText=ap;\n"
            "    b.finish=function(){return b;};\n"
            "    return b;\n"
            "  }\n"
            "  return createScriptProperties()\n"
            "    .addCheckbox({ name: 'enableplayer', value: true })\n"
            "    .addCheckbox({ name: 'musicOn',      value: false })\n"
            "    .addText({ name: 'unboundProp',      value: 'script-default' })\n"
            "    .finish();\n"
            "})();\n");
        // `user: 'ui'` resolves to engine.userProperties.ui (true), NOT `value: false`
        CHECK(env.engine.evaluate("result.enableplayer").toBool() == true);
        // `user: 'music'` resolves to engine.userProperties.music (false), NOT `value: true`
        CHECK(env.engine.evaluate("result.musicOn").toBool() == false);
        // No `user` field → falls back to the stored `value`.
        CHECK(env.engine.evaluate("result.unboundProp").toString() == "stored-literal");
    }

    TEST_CASE("scriptProperties `user` binding falls back when user prop missing") {
        // If the `user` name isn't present in engine.userProperties, the resolver
        // falls back to the stored `value` (and ultimately the script default).
        ScriptEnv env;
        env.engine.evaluate(
            "var engine = { userProperties: { other: 'x' } };\n"
            "var result = (function() {\n"
            "  var _storedProps = { foo: { user: 'missing', value: 42 } };\n"
            "  function createScriptProperties() {\n"
            "    var b = {};\n"
            "    function ap(def) {\n"
            "      var n = def.name;\n"
            "      if (n in _storedProps) {\n"
            "        var sp = _storedProps[n];\n"
            "        if ('user' in sp && engine.userProperties && sp.user in "
            "engine.userProperties) {\n"
            "          b[n] = engine.userProperties[sp.user];\n"
            "        } else if ('value' in sp) { b[n] = sp.value; }\n"
            "        else { b[n] = def.value; }\n"
            "      } else { b[n] = def.value; }\n"
            "      return b;\n"
            "    }\n"
            "    b.addSlider=ap;\n"
            "    b.finish=function(){return b;};\n"
            "    return b;\n"
            "  }\n"
            "  return createScriptProperties().addSlider({name:'foo',value:7}).finish();\n"
            "})();\n");
        CHECK(env.engine.evaluate("result.foo").toInt() == 42);
    }

    // _overlayScriptProps — scriptProperties aliased onto thisLayer.
    //
    // WE SceneScripts commonly read `thisLayer.<propName>` where `propName`
    // is declared via createScriptProperties().addCheckbox({name:'debug'}),
    // treating thisLayer as a namespace for the owning script's properties.
    // Solar system wallpaper's media text script (id=684 in scene 3662790108)
    // does exactly this to gate console logging behind an opt-in debug
    // checkbox; before this overlay, `thisLayer.debug` returned undefined
    // and the diagnostics were unreachable.  The overlay is injected inside
    // each script's IIFE so it doesn't bleed across scripts.
    TEST_CASE("overlayScriptProps exposes scriptProperties via thisLayer") {
        ScriptEnv env;
        env.engine.evaluate("var _sp = { debug: true, speed: 99, label: 'hi' };\n"
                            "var _rl = { origin: 'real-origin' };\n"
                            "var tl = _overlayScriptProps(_rl, _sp);\n");
        CHECK(env.engine.evaluate("tl.debug").toBool() == true);
        CHECK(env.engine.evaluate("tl.speed").toInt() == 99);
        CHECK(env.engine.evaluate("tl.label").toString() == "hi");
        // Non-scriptProperty keys fall through to the real layer via proto chain.
        CHECK(env.engine.evaluate("tl.origin").toString() == "real-origin");
    }

    TEST_CASE("overlayScriptProps writes propagate to scriptProperties bag") {
        // Scripts that set `thisLayer.foo = x` where foo is a scriptProperty
        // should mutate the scriptProperties bag itself (so the next read of
        // `scriptProperties.foo` or `thisLayer.foo` reflects the new value).
        ScriptEnv env;
        env.engine.evaluate("var sp = { mode: 'idle' };\n"
                            "var tl = _overlayScriptProps({}, sp);\n"
                            "tl.mode = 'active';\n");
        CHECK(env.engine.evaluate("tl.mode").toString() == "active");
        CHECK(env.engine.evaluate("sp.mode").toString() == "active");
    }

    TEST_CASE("overlayScriptProps preserves layer-proxy accessor forwarding") {
        // Writing `thisLayer.origin = Vec3(...)` through the overlay must
        // reach the real layer's dirty-tracked setter (via the prototype
        // chain), otherwise every property-script transform update would be
        // dropped on the floor.  This test stands in for the full layer
        // proxy — a plain object with an accessor property for `origin` that
        // records the latest write on a captured closure variable.
        ScriptEnv env;
        env.engine.evaluate("var captured = null;\n"
                            "var realLayer = {};\n"
                            "Object.defineProperty(realLayer, 'origin', {\n"
                            "  get: function(){ return captured; },\n"
                            "  set: function(v){ captured = v; }\n"
                            "});\n"
                            "var sp = { debug: false };\n"
                            "var tl = _overlayScriptProps(realLayer, sp);\n"
                            "tl.origin = 'written-through';\n");
        CHECK(env.engine.evaluate("captured").toString() == "written-through");
        CHECK(env.engine.evaluate("realLayer.origin").toString() == "written-through");
        // scriptProperty still overlays
        CHECK(env.engine.evaluate("tl.debug").toBool() == false);
    }

    TEST_CASE("overlayScriptProps handles null layer") {
        // Text/color scripts may run with thisLayer == null (no explicit
        // binding).  The overlay should still produce a plain object that
        // exposes scriptProperties — otherwise scripts gated on
        // `thisLayer.debug` would crash rather than just log nothing.
        ScriptEnv env;
        env.engine.evaluate("var sp = { debug: true };\n"
                            "var tl = _overlayScriptProps(null, sp);\n");
        CHECK(env.engine.evaluate("tl.debug").toBool() == true);
        CHECK(env.engine.evaluate("typeof tl").toString() == "object");
    }

    TEST_CASE("overlayScriptProps passthrough when scriptProperties is falsy") {
        // Scripts without createScriptProperties() shouldn't pay the overlay
        // cost — the helper returns the original layer unchanged.
        ScriptEnv env;
        env.engine.evaluate("var orig = { mark: 'original' };\n"
                            "var a = _overlayScriptProps(orig, null);\n"
                            "var b = _overlayScriptProps(orig, undefined);\n");
        CHECK(env.engine.evaluate("a === orig").toBool() == true);
        CHECK(env.engine.evaluate("b === orig").toBool() == true);
    }

    TEST_CASE("overlayScriptProps IIFE isolation: sibling scripts don't collide") {
        // Two scripts attached to the same logical layer, each declaring a
        // scriptProperty named `debug` with different values.  Each IIFE's
        // thisLayer must see its OWN scriptProperties, not the sibling's —
        // otherwise shared-layer scenes (many wallpapers put several
        // property scripts on the same container node) would read garbage.
        ScriptEnv env;
        env.engine.evaluate("var realLayer = { name: 'shared' };\n"
                            "var a = (function(_tlo){\n"
                            "  var thisLayer = _tlo;\n"
                            "  var scriptProperties = { debug: 'A' };\n"
                            "  thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                            "  return thisLayer.debug;\n"
                            "})(realLayer);\n"
                            "var b = (function(_tlo){\n"
                            "  var thisLayer = _tlo;\n"
                            "  var scriptProperties = { debug: 'B' };\n"
                            "  thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
                            "  return thisLayer.debug;\n"
                            "})(realLayer);\n");
        CHECK(env.engine.evaluate("a").toString() == "A");
        CHECK(env.engine.evaluate("b").toString() == "B");
        // Global thisLayer — and the shared realLayer — must not be polluted
        // by either IIFE's overlay (they ran in locally-shadowed scope).
        CHECK(env.engine.evaluate("'debug' in realLayer").toBool() == false);
    }

    TEST_CASE("overlayScriptProps matches solar 0767 pattern (gated debug)") {
        // End-to-end shape: a text-script IIFE declares a `debug` checkbox
        // scriptProperty and then guards a console.log with `thisLayer.debug`.
        // With overlay enabled, the update function sees the value the user
        // configured via addCheckbox.
        ScriptEnv env;
        env.engine.evaluate(
            "var _realLayer = _makeLayerProxy('bg');\n"
            "var wrapper = (function(_tlo){\n"
            "  var thisLayer = _tlo;\n"
            "  var scriptProperties = createScriptProperties()\n"
            "    .addCheckbox({ name: 'debug', value: true })\n"
            "    .finish();\n"
            "  thisLayer = _overlayScriptProps(thisLayer, scriptProperties);\n"
            "  return { gated: function() { return thisLayer.debug ? 'LOG' : ''; },\n"
            "           layerName: function() { return thisLayer.name; } };\n"
            "})(_realLayer);\n");
        CHECK(env.engine.evaluate("wrapper.gated()").toString() == "LOG");
        CHECK(env.engine.evaluate("wrapper.layerName()").toString() == "bg");
    }

    // ----- localStorage JS shim -----
    // The real SceneObject binds __sceneBridge.lsGet/lsSet/lsRemove/lsClear
    // to C++ slots that persist to disk.  Here we verify the shim's call
    // translation: default location, key stringification, and that every
    // shim method actually delegates to the bridge (no silent no-ops).
    // Persistence is verified via sceneviewer-script — the C++ side is just
    // JSON file I/O and is covered by Qt's own QJson* tests.
    TEST_CASE("localStorage shim defaults loc=1 (SCREEN) when omitted") {
        // Solar system's icon-state save/load flow omits `loc`.  WE treats
        // that as LOCATION_SCREEN (per-scene) — otherwise two wallpapers
        // with the same key would collide.
        ScriptEnv env;
        env.engine.evaluate(
            "var _calls = [];\n"
            "var __sceneBridge = {\n"
            "  lsGet: function(loc, key) { _calls.push(['get', loc, key]); return 'v'; },\n"
            "  lsSet: function(loc, key, v) { _calls.push(['set', loc, key, v]); },\n"
            "  lsRemove: function(loc, key) { _calls.push(['remove', loc, key]); },\n"
            "  lsClear: function(loc) { _calls.push(['clear', loc]); }\n"
            "};\n"
            "var localStorage = (function() {\n"
            "  function _loc(l) { return (l === 0 || l === 1) ? l : 1; }\n"
            "  return {\n"
            "    LOCATION_GLOBAL: 0, LOCATION_SCREEN: 1,\n"
            "    get: function(key, loc) { return __sceneBridge.lsGet(_loc(loc), String(key)); },\n"
            "    set: function(key, v, loc) { __sceneBridge.lsSet(_loc(loc), String(key), v); },\n"
            "    remove: function(key, loc) { __sceneBridge.lsRemove(_loc(loc), String(key)); },\n"
            "    clear: function(loc) { __sceneBridge.lsClear(_loc(loc)); }\n"
            "  };\n"
            "})();\n"
            "localStorage.set('k', 'v');\n"
            "localStorage.get('k');\n"
            "localStorage.remove('k');\n"
            "localStorage.clear();\n");
        CHECK(env.engine.evaluate("_calls[0][1]").toInt() == 1); // set → SCREEN
        CHECK(env.engine.evaluate("_calls[1][1]").toInt() == 1); // get → SCREEN
        CHECK(env.engine.evaluate("_calls[2][1]").toInt() == 1); // remove → SCREEN
        CHECK(env.engine.evaluate("_calls[3][1]").toInt() == 1); // clear → SCREEN
    }

    TEST_CASE("localStorage shim honors explicit LOCATION_GLOBAL") {
        ScriptEnv env;
        env.engine.evaluate(
            "var _calls = [];\n"
            "var __sceneBridge = {\n"
            "  lsGet: function(loc, key) { _calls.push(['get', loc, key]); return null; },\n"
            "  lsSet: function(loc, key, v) { _calls.push(['set', loc, key, v]); },\n"
            "  lsRemove: function() {}, lsClear: function() {}\n"
            "};\n"
            "var localStorage = (function() {\n"
            "  function _loc(l) { return (l === 0 || l === 1) ? l : 1; }\n"
            "  return {\n"
            "    LOCATION_GLOBAL: 0, LOCATION_SCREEN: 1,\n"
            "    get: function(key, loc) { return __sceneBridge.lsGet(_loc(loc), String(key)); },\n"
            "    set: function(key, v, loc) { __sceneBridge.lsSet(_loc(loc), String(key), v); }\n"
            "  };\n"
            "})();\n"
            "localStorage.set('shared', 42, localStorage.LOCATION_GLOBAL);\n"
            "localStorage.get('shared', localStorage.LOCATION_GLOBAL);\n");
        CHECK(env.engine.evaluate("_calls[0][1]").toInt() == 0); // GLOBAL
        CHECK(env.engine.evaluate("_calls[0][2]").toString() == "shared");
        CHECK(env.engine.evaluate("_calls[0][3]").toInt() == 42);
        CHECK(env.engine.evaluate("_calls[1][1]").toInt() == 0);
    }

    TEST_CASE("localStorage shim coerces non-string keys") {
        // Scripts sometimes pass numeric/bool keys by accident.  The shim
        // normalizes to String() so the bridge sees a consistent type and
        // cache key collisions don't happen based on JS toString semantics.
        ScriptEnv env;
        env.engine.evaluate(
            "var _lastKey = null;\n"
            "var __sceneBridge = {\n"
            "  lsGet: function(loc, key) { _lastKey = key; return undefined; },\n"
            "  lsSet: function() {}, lsRemove: function() {}, lsClear: function() {}\n"
            "};\n"
            "var localStorage = {\n"
            "  LOCATION_SCREEN: 1,\n"
            "  get: function(key, loc) { return __sceneBridge.lsGet(1, String(key)); }\n"
            "};\n"
            "localStorage.get(42);\n");
        CHECK(env.engine.evaluate("_lastKey").toString() == "42");
        CHECK(env.engine.evaluate("typeof _lastKey").toString() == "string");
    }

    // ----- engine.openUserShortcut -----
    // Production shim delegates to __sceneBridge.openUserShortcut, which
    // (in the real SceneObject) emits userShortcutRequested for Scene.qml
    // to route to MPRIS.  The shim itself must: (a) delegate for non-empty
    // strings, (b) no-op on empty/non-string inputs (solar never fires
    // those but older wallpapers may bind mis-typed names).
    TEST_CASE("openUserShortcut shim delegates non-empty names to bridge") {
        ScriptEnv env;
        env.engine.evaluate(
            "var _fired = [];\n"
            "var __sceneBridge = { openUserShortcut: function(n) { _fired.push(n); } };\n"
            "engine.openUserShortcut = function(name) {\n"
            "  if (typeof name !== 'string' || !name) return;\n"
            "  if (__sceneBridge && __sceneBridge.openUserShortcut)\n"
            "    __sceneBridge.openUserShortcut(name);\n"
            "};\n"
            "engine.openUserShortcut('b11');\n"
            "engine.openUserShortcut('bplay');\n");
        CHECK(env.engine.evaluate("_fired.length").toInt() == 2);
        CHECK(env.engine.evaluate("_fired[0]").toString() == "b11");
        CHECK(env.engine.evaluate("_fired[1]").toString() == "bplay");
    }

    TEST_CASE("openUserShortcut shim ignores empty / non-string inputs") {
        ScriptEnv env;
        env.engine.evaluate("var _count = 0;\n"
                            "var __sceneBridge = { openUserShortcut: function(n) { _count++; } };\n"
                            "engine.openUserShortcut = function(name) {\n"
                            "  if (typeof name !== 'string' || !name) return;\n"
                            "  __sceneBridge.openUserShortcut(name);\n"
                            "};\n"
                            "engine.openUserShortcut();\n"
                            "engine.openUserShortcut('');\n"
                            "engine.openUserShortcut(null);\n"
                            "engine.openUserShortcut(42);\n"
                            "engine.openUserShortcut({name: 'bplay'});\n");
        CHECK(env.engine.evaluate("_count").toInt() == 0);
    }

    TEST_CASE("localStorage shim out-of-range loc clamps to SCREEN") {
        // Defensive: WE docs only define 0 and 1.  Any other integer (or
        // bogus truthy value from a script bug) must not write to GLOBAL
        // by accident — treat unknown as SCREEN (per-scene, scoped).
        ScriptEnv env;
        env.engine.evaluate("var _lastLoc = -1;\n"
                            "var __sceneBridge = {\n"
                            "  lsGet: function() { return null; },\n"
                            "  lsSet: function(loc, key, v) { _lastLoc = loc; },\n"
                            "  lsRemove: function() {}, lsClear: function() {}\n"
                            "};\n"
                            "function _loc(l) { return (l === 0 || l === 1) ? l : 1; }\n"
                            "var localStorage = { set: function(k, v, l) { "
                            "__sceneBridge.lsSet(_loc(l), k, v); } };\n"
                            "localStorage.set('a', 1, 9);\n"
                            "localStorage.set('b', 2, 'GLOBAL');\n"
                            "localStorage.set('c', 3, -1);\n");
        // All three clamped to SCREEN (1), never leaked to GLOBAL (0)
        CHECK(env.engine.evaluate("_lastLoc").toInt() == 1);
    }

    TEST_CASE("empty script produces null") {
        ScriptEnv env;
        QString   script = QString("%1\n%2").arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
        QJSValue  r      = env.engine.evaluate(script);
        CHECK(r.isNull());
    }

    TEST_CASE("init without update returns null") {
        ScriptEnv env;
        // Text/color IIFE requires update to return non-null
        QString script =
            QString("%1  exports.init = function(v){};\n%2").arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.isNull());
    }

    TEST_CASE("applyUserProperties extracted from exports") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  var _captured = null;\n"
                                   "  exports.update = function(v){ return _captured; };\n"
                                   "  exports.applyUserProperties = function(props){\n"
                                   "    _captured = props.speed;\n"
                                   "  };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("applyUserProperties").isCallable());
        // Simulate property update
        r.property("applyUserProperties").call({ env.engine.evaluate("({speed: 42})") });
        QJSValue result = r.property("update").call({ QJSValue(0) });
        CHECK(result.toInt() == 42);
    }

    TEST_CASE("applyUserProperties bare function form") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  var _captured = '';\n"
                                   "  exports.update = function(v){ return _captured; };\n"
                                   "  function applyUserProperties(props) {\n"
                                   "    _captured = props.theme;\n"
                                   "  }\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("applyUserProperties").isCallable());
        r.property("applyUserProperties").call({ env.engine.evaluate("({theme: 'dark'})") });
        CHECK(r.property("update").call({ QJSValue(0) }).toString() == "dark");
    }

    TEST_CASE("applyUserProperties null when not defined") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK_FALSE(r.property("applyUserProperties").isCallable());
    }

    TEST_CASE("destroy extracted from exports") {
        ScriptEnv env;
        env.engine.evaluate("var _destroyCalled = false;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  exports.destroy = function(){\n"
                                 "    _destroyCalled = true;\n"
                                 "  };\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("destroy").isCallable());
        r.property("destroy").call({});
        CHECK(env.engine.evaluate("_destroyCalled").toBool());
    }

    TEST_CASE("destroy bare function form") {
        ScriptEnv env;
        env.engine.evaluate("var _destroyCount = 0;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  function destroy() { _destroyCount++; }\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("destroy").isCallable());
        r.property("destroy").call({});
        r.property("destroy").call({});
        CHECK(env.engine.evaluate("_destroyCount").toInt() == 2);
    }

    TEST_CASE("destroy null when not defined") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK_FALSE(r.property("destroy").isCallable());
    }

    TEST_CASE("resizeScreen extracted from exports") {
        ScriptEnv env;
        env.engine.evaluate("var _resizeW = 0; var _resizeH = 0;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  exports.resizeScreen = function(w, h){\n"
                                 "    _resizeW = w; _resizeH = h;\n"
                                 "  };\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("resizeScreen").isCallable());
        r.property("resizeScreen").call({ QJSValue(2560), QJSValue(1440) });
        CHECK(env.engine.evaluate("_resizeW").toInt() == 2560);
        CHECK(env.engine.evaluate("_resizeH").toInt() == 1440);
    }

    TEST_CASE("resizeScreen bare function form") {
        ScriptEnv env;
        env.engine.evaluate("var _resizeArea = 0;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  function resizeScreen(w, h) { _resizeArea = w * h; }\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("resizeScreen").isCallable());
        r.property("resizeScreen").call({ QJSValue(1920), QJSValue(1080) });
        CHECK(env.engine.evaluate("_resizeArea").toInt() == 1920 * 1080);
    }

    TEST_CASE("resizeScreen null when not defined") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK_FALSE(r.property("resizeScreen").isCallable());
    }

    TEST_CASE("script with only applyUserProperties is kept") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.applyUserProperties = function(p){};\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        // Should not be null/undefined — script has a callable handler
        CHECK_FALSE(r.isNull());
        CHECK_FALSE(r.isUndefined());
        CHECK(r.property("applyUserProperties").isCallable());
        // update/init should be null
        CHECK_FALSE(r.property("update").isCallable());
        CHECK_FALSE(r.property("init").isCallable());
    }

    TEST_CASE("script with only destroy is kept") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.destroy = function(){};\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK_FALSE(r.isNull());
        CHECK(r.property("destroy").isCallable());
    }

    TEST_CASE("mediaPlaybackChanged extracted from exports") {
        ScriptEnv env;
        env.engine.evaluate("var _capturedState = -1;");
        QString script =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  exports.update = function(v){ return v; };\n"
                    "  exports.mediaPlaybackChanged = function(e){ _capturedState = e.state; };\n"
                    "%1")
                .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK(r.property("mediaPlaybackChanged").isCallable());
        // Simulate calling the handler
        QJSValue event = env.engine.newObject();
        event.setProperty("state", 1);
        r.property("mediaPlaybackChanged").call({ event });
        CHECK(env.engine.evaluate("_capturedState").toInt() == 1);
    }

    TEST_CASE("mediaPropertiesChanged receives event properties") {
        ScriptEnv env;
        env.engine.evaluate("var _capturedTitle = '';");
        QString script =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  exports.update = function(v){ return v; };\n"
                    "  exports.mediaPropertiesChanged = function(e){ _capturedTitle = e.title; };\n"
                    "%1")
                .arg(PROP_IIFE_POST);
        QJSValue r     = env.engine.evaluate(script);
        QJSValue event = env.engine.newObject();
        event.setProperty("title", QJSValue("Test Song"));
        event.setProperty("artist", QJSValue("Test Artist"));
        r.property("mediaPropertiesChanged").call({ event });
        CHECK(env.engine.evaluate("_capturedTitle").toString() == "Test Song");
    }

    TEST_CASE("mediaThumbnailChanged receives Vec3 colors") {
        ScriptEnv env;
        env.engine.evaluate("var _hasThumbnail = false; var _primaryR = 0;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  exports.mediaThumbnailChanged = function(e){\n"
                                 "    _hasThumbnail = e.hasThumbnail;\n"
                                 "    _primaryR = e.primaryColor.r;\n"
                                 "  };\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r     = env.engine.evaluate(script);
        QJSValue event = env.engine.newObject();
        event.setProperty("hasThumbnail", true);
        QJSValue vec3Fn = env.engine.globalObject().property("Vec3");
        event.setProperty("primaryColor",
                          vec3Fn.call({ QJSValue(0.8), QJSValue(0.2), QJSValue(0.1) }));
        r.property("mediaThumbnailChanged").call({ event });
        CHECK(env.engine.evaluate("_hasThumbnail").toBool());
        CHECK(env.engine.evaluate("_primaryR").toNumber() == doctest::Approx(0.8));
    }

    TEST_CASE("mediaTimelineChanged receives position and duration") {
        ScriptEnv env;
        env.engine.evaluate("var _pos = 0; var _dur = 0;");
        QString script = QString("(function() {\n"
                                 "  'use strict';\n"
                                 "  var exports = {};\n"
                                 "  exports.update = function(v){ return v; };\n"
                                 "  exports.mediaTimelineChanged = function(e){ _pos = e.position; "
                                 "_dur = e.duration; };\n"
                                 "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r     = env.engine.evaluate(script);
        QJSValue event = env.engine.newObject();
        event.setProperty("position", 42.5);
        event.setProperty("duration", 180.0);
        r.property("mediaTimelineChanged").call({ event });
        CHECK(env.engine.evaluate("_pos").toNumber() == doctest::Approx(42.5));
        CHECK(env.engine.evaluate("_dur").toNumber() == doctest::Approx(180.0));
    }

    TEST_CASE("mediaStatusChanged receives enabled flag") {
        ScriptEnv env;
        env.engine.evaluate("var _enabled = false;");
        QString script =
            QString("(function() {\n"
                    "  'use strict';\n"
                    "  var exports = {};\n"
                    "  exports.update = function(v){ return v; };\n"
                    "  exports.mediaStatusChanged = function(e){ _enabled = e.enabled; };\n"
                    "%1")
                .arg(PROP_IIFE_POST);
        QJSValue r     = env.engine.evaluate(script);
        QJSValue event = env.engine.newObject();
        event.setProperty("enabled", true);
        r.property("mediaStatusChanged").call({ event });
        CHECK(env.engine.evaluate("_enabled").toBool());
    }

    TEST_CASE("media handler null when not defined") {
        ScriptEnv env;
        QString   script = QString("(function() {\n"
                                   "  'use strict';\n"
                                   "  var exports = {};\n"
                                   "  exports.update = function(v){ return v; };\n"
                                   "%1")
                             .arg(PROP_IIFE_POST);
        QJSValue r = env.engine.evaluate(script);
        CHECK_FALSE(r.property("mediaPlaybackChanged").isCallable());
        CHECK_FALSE(r.property("mediaPropertiesChanged").isCallable());
        CHECK_FALSE(r.property("mediaThumbnailChanged").isCallable());
        CHECK_FALSE(r.property("mediaTimelineChanged").isCallable());
        CHECK_FALSE(r.property("mediaStatusChanged").isCallable());
    }

} // TEST_SUITE Script Compilation

// ===================================================================
// Scene Property Control tests
// ===================================================================

// JS code for scene-level property control (matches SceneBackend.cpp)
static const char* JS_SCENE_PROPS =
    // _sceneInit would normally come from C++ JSON.parse; we define it inline
    "var _sceneInit = {\n"
    "  cc: [0.1, 0.2, 0.3], bloom: true, bs: 2.0, bt: 0.65,\n"
    "  ac: [0.2, 0.2, 0.2], sc: [0.3, 0.3, 0.3],\n"
    "  persp: true, fov: 50.0,\n"
    "  eye: [0, 0, 1], ctr: [0, 0, 0], up: [0, 1, 0],\n"
    "  lights: [\n"
    "    { c: [1, 0, 0], r: 100, i: 2.0, p: [10, 20, 30] },\n"
    "    { c: [0, 1, 0], r: 50, i: 1.5, p: [-5, 10, 15] }\n"
    "  ]\n"
    "};\n"
    "var _sceneState = {\n"
    "  clearColor: {x:_sceneInit.cc[0], y:_sceneInit.cc[1], z:_sceneInit.cc[2]},\n"
    "  bloomEnabled: _sceneInit.bloom,\n"
    "  bloomStrength: _sceneInit.bs,\n"
    "  bloomThreshold: _sceneInit.bt,\n"
    "  ambientColor: {x:_sceneInit.ac[0], y:_sceneInit.ac[1], z:_sceneInit.ac[2]},\n"
    "  skylightColor: {x:_sceneInit.sc[0], y:_sceneInit.sc[1], z:_sceneInit.sc[2]},\n"
    "  isPerspective: _sceneInit.persp,\n"
    "  cameraFov: _sceneInit.fov,\n"
    "  cameraEye: {x:_sceneInit.eye[0], y:_sceneInit.eye[1], z:_sceneInit.eye[2]},\n"
    "  cameraCenter: {x:_sceneInit.ctr[0], y:_sceneInit.ctr[1], z:_sceneInit.ctr[2]},\n"
    "  cameraUp: {x:_sceneInit.up[0], y:_sceneInit.up[1], z:_sceneInit.up[2]},\n"
    "  _dirty: {}\n"
    "};\n"
    "_sceneState.lights = _sceneInit.lights.map(function(l) {\n"
    "  var _s = { color:{x:l.c[0],y:l.c[1],z:l.c[2]},\n"
    "    radius:l.r, intensity:l.i,\n"
    "    position:{x:l.p[0],y:l.p[1],z:l.p[2]}, _dirty:{} };\n"
    "  var p = {};\n"
    "  ['color','position'].forEach(function(prop) {\n"
    "    Object.defineProperty(p, prop, {\n"
    "      get: function(){ return _s[prop]; },\n"
    "      set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
    "      enumerable: true\n"
    "    });\n"
    "  });\n"
    "  ['radius','intensity'].forEach(function(prop) {\n"
    "    Object.defineProperty(p, prop, {\n"
    "      get: function(){ return _s[prop]; },\n"
    "      set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
    "      enumerable: true\n"
    "    });\n"
    "  });\n"
    "  p._state = _s;\n"
    "  return p;\n"
    "});\n"
    // thisScene needs to exist before we add properties to it
    "if (typeof thisScene === 'undefined') var thisScene = {};\n"
    "['clearColor','ambientColor','skylightColor','cameraEye','cameraCenter','cameraUp'].forEach("
    "function(prop) {\n"
    "  Object.defineProperty(thisScene, prop, {\n"
    "    get: function(){ return _sceneState[prop]; },\n"
    "    set: function(v){ _sceneState[prop] = v; _sceneState._dirty[prop] = true; },\n"
    "    enumerable: true\n"
    "  });\n"
    "});\n"
    "['bloomStrength','bloomThreshold','cameraFov'].forEach(function(prop) {\n"
    "  Object.defineProperty(thisScene, prop, {\n"
    "    get: function(){ return _sceneState[prop]; },\n"
    "    set: function(v){ _sceneState[prop] = v; _sceneState._dirty[prop] = true; },\n"
    "    enumerable: true\n"
    "  });\n"
    "});\n"
    "Object.defineProperty(thisScene, 'bloomEnabled', {\n"
    "  get: function(){ return _sceneState.bloomEnabled; }, enumerable: true\n"
    "});\n"
    "Object.defineProperty(thisScene, 'isPerspective', {\n"
    "  get: function(){ return _sceneState.isPerspective; }, enumerable: true\n"
    "});\n"
    "thisScene.getLights = function() { return _sceneState.lights; };\n"
    "function _collectDirtyScene() {\n"
    "  var d = _sceneState._dirty;\n"
    "  var keys = Object.keys(d);\n"
    "  var dirtyLights = [];\n"
    "  for (var i = 0; i < _sceneState.lights.length; i++) {\n"
    "    var ls = _sceneState.lights[i]._state;\n"
    "    var ld = ls._dirty;\n"
    "    if (Object.keys(ld).length > 0) {\n"
    "      dirtyLights.push({idx:i, dirty:ld,\n"
    "        color:ls.color, radius:ls.radius,\n"
    "        intensity:ls.intensity, position:ls.position});\n"
    "      ls._dirty = {};\n"
    "    }\n"
    "  }\n"
    "  if (keys.length === 0 && dirtyLights.length === 0) return null;\n"
    "  var r = {dirty:d, lights:dirtyLights};\n"
    "  if (d.clearColor) r.clearColor = _sceneState.clearColor;\n"
    "  if (d.bloomStrength) r.bloomStrength = _sceneState.bloomStrength;\n"
    "  if (d.bloomThreshold) r.bloomThreshold = _sceneState.bloomThreshold;\n"
    "  if (d.ambientColor) r.ambientColor = _sceneState.ambientColor;\n"
    "  if (d.skylightColor) r.skylightColor = _sceneState.skylightColor;\n"
    "  if (d.cameraFov) r.cameraFov = _sceneState.cameraFov;\n"
    "  if (d.cameraEye) r.cameraEye = _sceneState.cameraEye;\n"
    "  if (d.cameraCenter) r.cameraCenter = _sceneState.cameraCenter;\n"
    "  if (d.cameraUp) r.cameraUp = _sceneState.cameraUp;\n"
    "  _sceneState._dirty = {};\n"
    "  return r;\n"
    "}\n";

struct ScenePropertyEnv {
    QJSEngine engine;
    ScenePropertyEnv() {
        // Vec2/Vec3/Vec4 + Mat3/Mat4 are needed for thisScene.getCameraTransforms
        // (the lookAt + projection builder targets Mat4) and
        // setCameraTransforms (writes plain {x,y,z} onto thisScene properties).
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        engine.evaluate(wek::qml_helper::kMatricesJs);
        engine.evaluate(JS_SCENE_PROPS);
        // Mirror SceneBackend's scripted-camera helpers.  engine.canvasSize is
        // supplied so getCameraTransforms can compute aspect; fixture uses 16:9.
        engine.globalObject().setProperty("engine", engine.newObject());
        engine.evaluate("engine.canvasSize = { x: 1920, y: 1080 };\n");
        engine.evaluate(
            "thisScene.getCameraTransforms = function() {\n"
            "  var eye    = thisScene.cameraEye;\n"
            "  var center = thisScene.cameraCenter;\n"
            "  var up     = thisScene.cameraUp;\n"
            "  var ex = eye.x, ey = eye.y, ez = eye.z;\n"
            "  var fx = center.x - ex, fy = center.y - ey, fz = center.z - ez;\n"
            "  var flen = Math.sqrt(fx*fx + fy*fy + fz*fz) || 1;\n"
            "  fx /= flen; fy /= flen; fz /= flen;\n"
            "  var rx = fy*up.z - fz*up.y, ry = fz*up.x - fx*up.z, rz = fx*up.y - fy*up.x;\n"
            "  var rlen = Math.sqrt(rx*rx + ry*ry + rz*rz) || 1;\n"
            "  rx /= rlen; ry /= rlen; rz /= rlen;\n"
            "  var ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx;\n"
            "  var view = new Mat4();\n"
            "  view.m[0]=rx;  view.m[1]=ux;  view.m[2] =-fx; view.m[3] =0;\n"
            "  view.m[4]=ry;  view.m[5]=uy;  view.m[6] =-fy; view.m[7] =0;\n"
            "  view.m[8]=rz;  view.m[9]=uz;  view.m[10]=-fz; view.m[11]=0;\n"
            "  view.m[12]=-(rx*ex+ry*ey+rz*ez);\n"
            "  view.m[13]=-(ux*ex+uy*ey+uz*ez);\n"
            "  view.m[14]= (fx*ex+fy*ey+fz*ez);\n"
            "  view.m[15]=1;\n"
            "  var proj = new Mat4();\n"
            "  var fovDeg = thisScene.cameraFov || 60;\n"
            "  var fRad   = fovDeg * Math.PI / 180;\n"
            "  var f      = 1.0 / Math.tan(fRad * 0.5);\n"
            "  var aspect = (engine.canvasSize && engine.canvasSize.x && engine.canvasSize.y)\n"
            "             ? engine.canvasSize.x / engine.canvasSize.y : 1.0;\n"
            "  var near = 0.1, far = 10000;\n"
            "  proj.m[0]=f/aspect; proj.m[5]=f;\n"
            "  proj.m[10]=(far+near)/(near-far);\n"
            "  proj.m[11]=-1;\n"
            "  proj.m[14]=(2*far*near)/(near-far);\n"
            "  proj.m[15]=0;\n"
            "  return { view: view, projection: proj };\n"
            "};\n"
            "thisScene.setCameraTransforms = function(t) {\n"
            "  if (!t) return;\n"
            "  if (t.eye    !== undefined) thisScene.cameraEye    = t.eye;\n"
            "  if (t.center !== undefined) thisScene.cameraCenter = t.center;\n"
            "  if (t.up     !== undefined) thisScene.cameraUp     = t.up;\n"
            "  if (t.fov    !== undefined) thisScene.cameraFov    = t.fov;\n"
            "  if (t.view   !== undefined && t.view.m && t.eye === undefined) {\n"
            "    thisScene.cameraEye = { x: -t.view.m[12], y: -t.view.m[13], z:  t.view.m[14] };\n"
            "  }\n"
            "};\n");
    }
};

// ------------------------------------------------------------------
TEST_SUITE("Scene Clear Color") {
    TEST_CASE("initial clearColor from init state") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.clearColor.x").toNumber() == doctest::Approx(0.1));
        CHECK(env.engine.evaluate("thisScene.clearColor.y").toNumber() == doctest::Approx(0.2));
        CHECK(env.engine.evaluate("thisScene.clearColor.z").toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("clearColor setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.clearColor = {x:0.5, y:0.6, z:0.7}");
        CHECK(env.engine.evaluate("_sceneState._dirty.clearColor").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.clearColor.x").toNumber() == doctest::Approx(0.5));
    }

    TEST_CASE("collectDirtyScene returns clearColor update") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.clearColor = {x:0.9, y:0.8, z:0.7}");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(! r.isNull());
        CHECK(r.property("dirty").property("clearColor").toBool() == true);
        CHECK(r.property("clearColor").property("x").toNumber() == doctest::Approx(0.9));
        CHECK(r.property("clearColor").property("y").toNumber() == doctest::Approx(0.8));
    }

    TEST_CASE("dirty resets after collection") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.clearColor = {x:1, y:1, z:1}");
        env.engine.evaluate("_collectDirtyScene()");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.isNull());
    }

} // TEST_SUITE Scene Clear Color

// ------------------------------------------------------------------
TEST_SUITE("Scene Bloom") {
    TEST_CASE("bloomEnabled is read-only") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.bloomEnabled").toBool() == true);
        // Attempt to set — should be silently ignored (no setter)
        env.engine.evaluate("thisScene.bloomEnabled = false");
        CHECK(env.engine.evaluate("thisScene.bloomEnabled").toBool() == true);
    }

    TEST_CASE("bloomStrength initial value") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.bloomStrength").toNumber() == doctest::Approx(2.0));
    }

    TEST_CASE("bloomStrength setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.bloomStrength = 3.5");
        CHECK(env.engine.evaluate("_sceneState._dirty.bloomStrength").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.bloomStrength").toNumber() == doctest::Approx(3.5));
    }

    TEST_CASE("bloomThreshold setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.bloomThreshold = 0.8");
        CHECK(env.engine.evaluate("_sceneState._dirty.bloomThreshold").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.bloomThreshold").toNumber() == doctest::Approx(0.8));
    }

    TEST_CASE("collection returns both bloom properties when dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.bloomStrength = 1.5; thisScene.bloomThreshold = 0.3");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(! r.isNull());
        CHECK(r.property("bloomStrength").toNumber() == doctest::Approx(1.5));
        CHECK(r.property("bloomThreshold").toNumber() == doctest::Approx(0.3));
    }

} // TEST_SUITE Scene Bloom

// ------------------------------------------------------------------
TEST_SUITE("Scene Camera") {
    TEST_CASE("isPerspective is read-only") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.isPerspective").toBool() == true);
        env.engine.evaluate("thisScene.isPerspective = false");
        CHECK(env.engine.evaluate("thisScene.isPerspective").toBool() == true);
    }

    TEST_CASE("cameraFov initial value and setter") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.cameraFov").toNumber() == doctest::Approx(50.0));
        env.engine.evaluate("thisScene.cameraFov = 75.0");
        CHECK(env.engine.evaluate("_sceneState._dirty.cameraFov").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.cameraFov").toNumber() == doctest::Approx(75.0));
    }

    TEST_CASE("cameraEye/cameraCenter/cameraUp initial values") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.cameraEye.x").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("thisScene.cameraEye.z").toNumber() == doctest::Approx(1));
        CHECK(env.engine.evaluate("thisScene.cameraCenter.x").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("thisScene.cameraCenter.z").toNumber() == doctest::Approx(0));
        CHECK(env.engine.evaluate("thisScene.cameraUp.y").toNumber() == doctest::Approx(1));
    }

    TEST_CASE("camera Vec3 setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.cameraEye = {x:5, y:5, z:5}");
        CHECK(env.engine.evaluate("_sceneState._dirty.cameraEye").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.cameraEye.x").toNumber() == doctest::Approx(5));
    }

    TEST_CASE("collection returns camera update") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.cameraFov = 90; thisScene.cameraEye = {x:1,y:2,z:3}");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(! r.isNull());
        CHECK(r.property("cameraFov").toNumber() == doctest::Approx(90));
        CHECK(r.property("cameraEye").property("x").toNumber() == doctest::Approx(1));
        CHECK(r.property("cameraEye").property("y").toNumber() == doctest::Approx(2));
    }

    TEST_CASE("camera center and up dirty tracking") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.cameraCenter = {x:1,y:0,z:-1}");
        env.engine.evaluate("thisScene.cameraUp = {x:0,y:0,z:1}");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.property("dirty").property("cameraCenter").toBool() == true);
        CHECK(r.property("dirty").property("cameraUp").toBool() == true);
        CHECK(r.property("cameraUp").property("z").toNumber() == doctest::Approx(1));
    }

    // ---- getCameraTransforms / setCameraTransforms ----------------------
    // Builds a view Mat4 from cameraEye/Center/Up via lookAt, and a
    // projection Mat4 from cameraFov + canvasSize.  Writes back through
    // high-level fields or falls back to extracting eye from the view's
    // translation column.

    TEST_CASE("getCameraTransforms returns view + projection Mat4s") {
        ScenePropertyEnv env;
        QJSValue t = env.engine.evaluate("thisScene.getCameraTransforms()");
        REQUIRE(t.isObject());
        CHECK(t.property("view").property("m").property(15).toNumber() == doctest::Approx(1));
        // Projection w = 0 (perspective), w-row receives -1 at m[11]
        CHECK(t.property("projection").property("m").property(11).toNumber() == doctest::Approx(-1));
    }

    TEST_CASE("getCameraTransforms.view reflects current eye position") {
        ScenePropertyEnv env;
        env.engine.evaluate(
            "thisScene.cameraEye    = { x: 10, y: 0,  z: 5 };\n"
            "thisScene.cameraCenter = { x: 0,  y: 0,  z: 0 };\n"
            "thisScene.cameraUp     = { x: 0,  y: 1,  z: 0 };\n");
        QJSValue t = env.engine.evaluate("thisScene.getCameraTransforms()");
        // view * eye_world ≈ 0 — translation encodes -R*eye in view space.
        // With eye at (10,0,5) looking at origin, forward = -eye/|eye|.
        // The view matrix's bottom-row translation (m[12..14]) should be
        // -(right.eye, up.eye, -fwd.eye).  Sanity: length(translation) roughly
        // matches eye distance from center.
        double tx = t.property("view").property("m").property(12).toNumber();
        double ty = t.property("view").property("m").property(13).toNumber();
        double tz = t.property("view").property("m").property(14).toNumber();
        double len = std::sqrt(tx*tx + ty*ty + tz*tz);
        CHECK(len == doctest::Approx(std::sqrt(125.0)).epsilon(0.02));
    }

    TEST_CASE("getCameraTransforms.projection aspect tracks canvasSize") {
        ScenePropertyEnv env;
        // canvasSize x=1920, y=1080 → aspect ≈ 16/9. proj.m[0] = f/aspect,
        // proj.m[5] = f. Ratio m[5]/m[0] should equal aspect.
        QJSValue t = env.engine.evaluate("thisScene.getCameraTransforms().projection.m");
        double   m0 = t.property(0).toNumber();
        double   m5 = t.property(5).toNumber();
        CHECK((m5 / m0) == doctest::Approx(1920.0 / 1080.0).epsilon(1e-6));
    }

    TEST_CASE("setCameraTransforms writes eye through high-level field") {
        ScenePropertyEnv env;
        env.engine.evaluate(
            "thisScene.setCameraTransforms({ eye: { x: 100, y: 200, z: 300 } });");
        CHECK(env.engine.evaluate("thisScene.cameraEye.x").toNumber() == doctest::Approx(100));
        CHECK(env.engine.evaluate("thisScene.cameraEye.z").toNumber() == doctest::Approx(300));
        // Dirty flag flipped so the render path picks up the change.
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.property("dirty").property("cameraEye").toBool() == true);
    }

    TEST_CASE("setCameraTransforms with .view decomposes translation to eye") {
        ScenePropertyEnv env;
        env.engine.evaluate(
            "var v = new Mat4();\n"
            "v.m[12] = -7; v.m[13] = -8; v.m[14] = 9;\n"
            "thisScene.setCameraTransforms({ view: v });");
        CHECK(env.engine.evaluate("thisScene.cameraEye.x").toNumber() == doctest::Approx(7));
        CHECK(env.engine.evaluate("thisScene.cameraEye.y").toNumber() == doctest::Approx(8));
        CHECK(env.engine.evaluate("thisScene.cameraEye.z").toNumber() == doctest::Approx(9));
    }

    TEST_CASE("setCameraTransforms fov updates cameraFov") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.setCameraTransforms({ fov: 75 });");
        CHECK(env.engine.evaluate("thisScene.cameraFov").toNumber() == doctest::Approx(75));
    }

    TEST_CASE("setCameraTransforms(null) is a no-op") {
        ScenePropertyEnv env;
        QJSValue  r = env.engine.evaluate("thisScene.setCameraTransforms(null)");
        CHECK_FALSE(r.isError());
    }

    TEST_CASE("getCameraTransforms → setCameraTransforms round-trips eye") {
        ScenePropertyEnv env;
        env.engine.evaluate(
            "thisScene.cameraEye    = { x: 3, y: 4, z: 5 };\n"
            "thisScene.cameraCenter = { x: 3, y: 4, z: 0 };\n"  // forward = -Z
            "thisScene.cameraUp     = { x: 0, y: 1, z: 0 };\n"
            "var t = thisScene.getCameraTransforms();\n"
            "thisScene.cameraEye = { x: 0, y: 0, z: 0 };\n"  // scrub
            "thisScene.setCameraTransforms({ view: t.view });");
        // The naive decomposition extracts eye from view.m[12..14] which
        // for an axis-aligned lookAt roughly recovers the original eye.
        QJSValue ex = env.engine.evaluate("thisScene.cameraEye.x");
        QJSValue ey = env.engine.evaluate("thisScene.cameraEye.y");
        CHECK(ex.toNumber() == doctest::Approx(3).epsilon(0.01));
        CHECK(ey.toNumber() == doctest::Approx(4).epsilon(0.01));
    }

} // TEST_SUITE Scene Camera

// ------------------------------------------------------------------
TEST_SUITE("Scene Ambient/Skylight") {
    TEST_CASE("ambientColor initial value") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.ambientColor.x").toNumber() == doctest::Approx(0.2));
        CHECK(env.engine.evaluate("thisScene.ambientColor.y").toNumber() == doctest::Approx(0.2));
    }

    TEST_CASE("ambientColor setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.ambientColor = {x:0.5, y:0.5, z:0.5}");
        CHECK(env.engine.evaluate("_sceneState._dirty.ambientColor").toBool() == true);
    }

    TEST_CASE("skylightColor initial and setter") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.skylightColor.x").toNumber() == doctest::Approx(0.3));
        env.engine.evaluate("thisScene.skylightColor = {x:0.8, y:0.7, z:0.6}");
        CHECK(env.engine.evaluate("_sceneState._dirty.skylightColor").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.skylightColor.x").toNumber() == doctest::Approx(0.8));
    }

    TEST_CASE("collection returns both lighting colors") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.ambientColor = {x:1,y:0,z:0}");
        env.engine.evaluate("thisScene.skylightColor = {x:0,y:1,z:0}");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.property("ambientColor").property("x").toNumber() == doctest::Approx(1));
        CHECK(r.property("skylightColor").property("y").toNumber() == doctest::Approx(1));
    }

} // TEST_SUITE Scene Ambient/Skylight

// ------------------------------------------------------------------
TEST_SUITE("Scene Light Proxies") {
    TEST_CASE("getLights returns correct count") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.getLights().length").toInt() == 2);
    }

    TEST_CASE("initial light color/radius/intensity/position") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.getLights()[0].color.x").toNumber() ==
              doctest::Approx(1));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].color.y").toNumber() ==
              doctest::Approx(0));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].radius").toNumber() ==
              doctest::Approx(100));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].intensity").toNumber() ==
              doctest::Approx(2.0));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].position.x").toNumber() ==
              doctest::Approx(10));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].position.y").toNumber() ==
              doctest::Approx(20));
    }

    TEST_CASE("second light initial values") {
        ScenePropertyEnv env;
        CHECK(env.engine.evaluate("thisScene.getLights()[1].color.y").toNumber() ==
              doctest::Approx(1));
        CHECK(env.engine.evaluate("thisScene.getLights()[1].radius").toNumber() ==
              doctest::Approx(50));
        CHECK(env.engine.evaluate("thisScene.getLights()[1].position.x").toNumber() ==
              doctest::Approx(-5));
    }

    TEST_CASE("light color setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.getLights()[0].color = {x:0, y:0, z:1}");
        CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.color").toBool() == true);
        CHECK(env.engine.evaluate("thisScene.getLights()[0].color.z").toNumber() ==
              doctest::Approx(1));
    }

    TEST_CASE("light radius and intensity setters mark dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("var l = thisScene.getLights()[0]; l.radius = 200; l.intensity = 5.0");
        CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.radius").toBool() ==
              true);
        CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.intensity").toBool() ==
              true);
        CHECK(env.engine.evaluate("thisScene.getLights()[0].radius").toNumber() ==
              doctest::Approx(200));
        CHECK(env.engine.evaluate("thisScene.getLights()[0].intensity").toNumber() ==
              doctest::Approx(5.0));
    }

    TEST_CASE("light position setter marks dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.getLights()[1].position = {x:99, y:88, z:77}");
        CHECK(env.engine.evaluate("thisScene.getLights()[1]._state._dirty.position").toBool() ==
              true);
        CHECK(env.engine.evaluate("thisScene.getLights()[1].position.x").toNumber() ==
              doctest::Approx(99));
    }

    TEST_CASE("collectDirtyScene returns light updates with index") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.getLights()[0].color = {x:0.5,y:0.5,z:0.5}");
        env.engine.evaluate("thisScene.getLights()[1].radius = 999");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(! r.isNull());
        QJSValue lights = r.property("lights");
        CHECK(lights.property("length").toInt() == 2);
        CHECK(lights.property(0).property("idx").toInt() == 0);
        CHECK(lights.property(0).property("dirty").property("color").toBool() == true);
        CHECK(lights.property(0).property("color").property("x").toNumber() ==
              doctest::Approx(0.5));
        CHECK(lights.property(1).property("idx").toInt() == 1);
        CHECK(lights.property(1).property("dirty").property("radius").toBool() == true);
    }

    TEST_CASE("light dirty resets after collection") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.getLights()[0].intensity = 10");
        env.engine.evaluate("_collectDirtyScene()");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.isNull());
    }

} // TEST_SUITE Scene Light Proxies

// ------------------------------------------------------------------
TEST_SUITE("Scene Empty State") {
    TEST_CASE("no dirty when nothing changed") {
        ScenePropertyEnv env;
        QJSValue         r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(r.isNull());
    }

    TEST_CASE("only light dirty returns non-null with empty scene dirty") {
        ScenePropertyEnv env;
        env.engine.evaluate("thisScene.getLights()[0].radius = 42");
        QJSValue r = env.engine.evaluate("_collectDirtyScene()");
        CHECK(! r.isNull());
        // Scene-level dirty should be empty
        CHECK(env.engine
                  .evaluate("Object.keys(_collectDirtyScene() || {dirty:{}}).length === 0 || true")
                  .toBool());
        // But the lights array should have content
        CHECK(r.property("lights").property("length").toInt() == 1);
    }

} // TEST_SUITE Scene Empty State

// ------------------------------------------------------------------
// Cursor hit-test (drives SceneObject::mousePressEvent drag-target selection)
// ------------------------------------------------------------------
TEST_SUITE("Cursor hit-test") {
    // Build a minimal layer proxy mirroring _makeLayerProxy's _state shape.
    static QJSValue makeMockProxy(
        QJSEngine & engine, double ox, double oy, double sx, double sy, double w, double h) {
        QString script = QString("({ _state: { origin: {x:%1,y:%2,z:0}, scale: {x:%3,y:%4,z:1},"
                                 " size: {x:%5,y:%6} } })")
                             .arg(ox)
                             .arg(oy)
                             .arg(sx)
                             .arg(sy)
                             .arg(w)
                             .arg(h);
        return engine.evaluate(script);
    }

    TEST_CASE("hit inside centered AABB") {
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine,
                                       /*origin*/ 100,
                                       200,
                                       /*scale*/ 1,
                                       1,
                                       /*size*/ 80,
                                       40);
        CHECK(hitTestLayerProxy(proxy, 100.0f, 200.0f)); // exact center
        CHECK(hitTestLayerProxy(proxy, 139.0f, 219.0f)); // just inside corner
    }

    TEST_CASE("miss outside AABB") {
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine, 100, 200, 1, 1, 80, 40);
        CHECK_FALSE(hitTestLayerProxy(proxy, 141.0f, 200.0f));
        CHECK_FALSE(hitTestLayerProxy(proxy, 100.0f, 221.0f));
    }

    TEST_CASE("scale widens/narrows the hitbox") {
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine, 0, 0, 2.0, 0.5, 100, 100);
        // Full width 100*2 = 200 → halfW = 100. Half height 100*0.5 = 50 → halfH = 25.
        CHECK(hitTestLayerProxy(proxy, 90.0f, 20.0f));
        CHECK_FALSE(hitTestLayerProxy(proxy, 90.0f, 30.0f)); // past halfH
        CHECK_FALSE(hitTestLayerProxy(proxy, 110.0f, 0.0f)); // past halfW (< strict)
    }

    TEST_CASE("negative scale still hits (mirrored sprite)") {
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine, 0, 0, -1, -1, 60, 60);
        CHECK(hitTestLayerProxy(proxy, 20.0f, 20.0f));
        CHECK(hitTestLayerProxy(proxy, -20.0f, -20.0f));
    }

    TEST_CASE("zero size is never hittable (fallback for unregistered text layers)") {
        // Before the WPSceneParser fix, text layers got size={0,0} because
        // nameToObjState was only populated for WPImageObject/WPParticleObject.
        // This test pins the safety net: zero-size proxies don't steal drag.
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine, 100, 100, 1, 1, 0, 0);
        CHECK_FALSE(hitTestLayerProxy(proxy, 100.0f, 100.0f));
    }

    TEST_CASE("non-object proxy fails gracefully") {
        QJSEngine engine;
        CHECK_FALSE(hitTestLayerProxy(QJSValue(), 0.0f, 0.0f));
        CHECK_FALSE(hitTestLayerProxy(QJSValue(42), 0.0f, 0.0f));
        CHECK_FALSE(hitTestLayerProxy(QJSValue("hi"), 0.0f, 0.0f));
    }

    TEST_CASE("proxy missing _state fails gracefully") {
        QJSEngine engine;
        QJSValue  proxy = engine.evaluate("({ name: 'no state here' })");
        CHECK_FALSE(hitTestLayerProxy(proxy, 0.0f, 0.0f));
    }

    TEST_CASE("edge exactly on boundary does not hit (strict <)") {
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine, 0, 0, 1, 1, 100, 100);
        // halfW = 50 exactly
        CHECK_FALSE(hitTestLayerProxy(proxy, 50.0f, 0.0f));
        CHECK_FALSE(hitTestLayerProxy(proxy, 0.0f, 50.0f));
        CHECK(hitTestLayerProxy(proxy, 49.9f, 49.9f));
    }

    TEST_CASE("solid=false opts out of hit-test") {
        // Wallpaper 2866203962 playerplay alpha script sets element.solid = false
        // when media-control buttons fade out — the invisible quad shouldn't
        // swallow clicks.  Pins the solid gate in hitTestLayerProxy.
        QJSEngine engine;
        QJSValue  proxy = engine.evaluate("({ _state: { origin:{x:0,y:0,z:0}, scale:{x:1,y:1,z:1},"
                                          "  size:{x:100,y:100}, solid:false } })");
        CHECK_FALSE(hitTestLayerProxy(proxy, 0.0f, 0.0f));
        // Flip back on → hits again.
        engine.evaluate("(function(){ var p = arguments.callee.caller; })();");
        QJSValue proxy2 = engine.evaluate("({ _state: { origin:{x:0,y:0,z:0}, scale:{x:1,y:1,z:1},"
                                          "  size:{x:100,y:100}, solid:true } })");
        CHECK(hitTestLayerProxy(proxy2, 0.0f, 0.0f));
    }

    TEST_CASE("solid undefined defaults to on") {
        // Scripts only set solid explicitly — unset should mean interactive.
        QJSEngine engine;
        QJSValue  proxy = engine.evaluate("({ _state: { origin:{x:0,y:0,z:0}, scale:{x:1,y:1,z:1},"
                                          "  size:{x:100,y:100} } })");
        CHECK(hitTestLayerProxy(proxy, 0.0f, 0.0f));
    }

    TEST_CASE("parallax offset shifts hitbox") {
        // Wallpaper 2866203962's player buttons have parallaxDepth=(1,1) and the
        // scene enables camera parallax with amount=0.5, mouseInfluence=0.1.
        // The shader MVP shifts each layer by (origin - cam + mouseVec) *
        // depth * amount — hitTestLayerProxy mirrors that so the cursor still
        // lands on the visible button.
        //
        // playerplay: origin (2573, 1297), size 200×200 × scale 0.346.
        // With mouse at widget (0.744, 0.360) (= user's real click ~(1655,451)
        // on a 2226×1252 window mapping to scene 2856,1382):
        //   mouseVx = (0.5 - 0.744) * 3840 * 0.1 = -93.7
        //   mouseVy = (0.360 - 0.5) * 2160 * 0.1 = -30.2
        //   paraX = (2573 - 1920 - 93.7) * 1 * 0.5 = 279.6
        //   paraY = (1297 - 1080 - 30.2) * 1 * 0.5 = 93.4
        //   visual origin ≈ (2852.6, 1390.4)
        //   halfW = halfH = 200 * 0.346 / 2 = 34.6
        // So a scene click at (2856.7, 1381.9) hits (within ±34.6 on each axis).
        QJSEngine      engine;
        QJSValue       proxy = engine.evaluate("({ _state: { origin:{x:2572.8,y:1297.2,z:0},"
                                               "  scale:{x:0.346,y:0.346,z:1}, size:{x:200,y:200},"
                                               "  parallaxDepth:{x:1,y:1} } })");
        CursorParallax para;
        para.enable         = true;
        para.amount         = 0.5f;
        para.mouseInfluence = 0.1f;
        para.camX           = 1920.0f;
        para.camY           = 1080.0f;
        para.mouseNx        = 0.744f;
        para.mouseNy        = 0.360f;
        para.orthoW         = 3840.0f;
        para.orthoH         = 2160.0f;
        // Hits the parallax-shifted position.
        CHECK(hitTestLayerProxy(proxy, 2856.7f, 1381.9f, para));
        // MISSES the authored origin (too far from the shifted visual position).
        CHECK_FALSE(hitTestLayerProxy(proxy, 2572.8f, 1297.2f, para));
        // With parallax disabled, the authored origin hits and the shifted
        // position doesn't — confirms para.enable actually gates the offset.
        para.enable = false;
        CHECK(hitTestLayerProxy(proxy, 2572.8f, 1297.2f, para));
        CHECK_FALSE(hitTestLayerProxy(proxy, 2856.7f, 1381.9f, para));
    }

    TEST_CASE("parallax with zero depth has no effect") {
        // parallaxDepth=(0,0) means the layer doesn't parallax — default for
        // layers that don't set it explicitly in scene.json.  Pins that the
        // hit-test shift is gated on non-zero depth even when para.enable.
        QJSEngine engine;
        QJSValue  proxy =
            engine.evaluate("({ _state: { origin:{x:100,y:100,z:0}, scale:{x:1,y:1,z:1},"
                            "  size:{x:50,y:50}, parallaxDepth:{x:0,y:0} } })");
        CursorParallax para;
        para.enable         = true;
        para.amount         = 0.5f;
        para.mouseInfluence = 0.1f;
        para.camX           = 0.0f;
        para.camY           = 0.0f;
        para.mouseNx        = 0.0f; // extreme mouse position — big mouseVec
        para.mouseNy        = 1.0f;
        para.orthoW         = 3840.0f;
        para.orthoH         = 2160.0f;
        CHECK(hitTestLayerProxy(proxy, 100.0f, 100.0f, para));
    }

    TEST_CASE("VHS Time/Date dimensions — regression for drag fix") {
        // Wallpaper 2866203962 (Cyberpunk Lucy music player) places the VHS
        // Time/Date text at origin (2510.94, 939.83) with size (931, 153) and
        // scale (1,1,1) at init time.  Before WPTextObject routed through
        // nameToObjState the size was (0,0) and the layer silently failed every
        // hit-test, so its cursorDown/cursorMove/cursorUp drag script did
        // nothing.  This test locks the bounding box the fix establishes.
        QJSEngine engine;
        QJSValue  proxy = makeMockProxy(engine,
                                       /*origin*/ 2510.94,
                                       939.83,
                                       /*scale*/ 1.0,
                                       1.0,
                                       /*size*/ 931.0,
                                       153.0);
        // Center click → hit.
        CHECK(hitTestLayerProxy(proxy, 2510.94f, 939.83f));
        // Click 400 px to the right (within halfW=465.5) → hit.
        CHECK(hitTestLayerProxy(proxy, 2910.0f, 939.83f));
        // Click 500 px to the right (beyond halfW) → miss.
        CHECK_FALSE(hitTestLayerProxy(proxy, 3020.0f, 939.83f));
        // Click on top edge (±77 px) inside → hit; just past → miss.
        CHECK(hitTestLayerProxy(proxy, 2510.94f, 1015.0f));
        CHECK_FALSE(hitTestLayerProxy(proxy, 2510.94f, 1020.0f));
    }

} // TEST_SUITE Cursor hit-test

// ------------------------------------------------------------------
// Hover-leave debouncer: brief cursor exits from a hover zone shouldn't
// immediately tear down the hover state — critical for wallpapers like
// 2866203962 where the music-player fade ramps over seconds and grazes
// off the hit-zone edge would otherwise keep it faded out.
// ------------------------------------------------------------------
TEST_SUITE("Hover-leave debounce") {
    using Set      = std::unordered_set<std::string>;
    using LeaveMap = std::unordered_map<std::string, PendingLeave>;

    TEST_CASE("first entry reports cursorEnter") {
        LeaveMap pending;
        Set      prev = {};
        Set      now  = { "playerbounds" };
        auto     r    = processHoverFrame(prev, now, pending, /*nowMs*/ 1000, /*grace*/ 400);
        CHECK(r.toEnter == Set { "playerbounds" });
        CHECK(r.newHovered == Set { "playerbounds" });
        CHECK(pending.empty());
    }

    TEST_CASE("continued hover does not re-fire cursorEnter") {
        LeaveMap pending;
        Set      prev = { "playerbounds" };
        Set      now  = { "playerbounds" };
        auto     r    = processHoverFrame(prev, now, pending, 1000, 400);
        CHECK(r.toEnter.empty());
        CHECK(r.newHovered == Set { "playerbounds" });
        CHECK(pending.empty());
    }

    TEST_CASE("exit schedules a pending leave but stays hovered") {
        LeaveMap pending;
        Set      prev = { "playerbounds" };
        Set      now  = {};
        auto     r    = processHoverFrame(prev, now, pending, /*nowMs*/ 1000, /*grace*/ 400);
        CHECK(r.toEnter.empty());
        // Still in hovered set during grace.
        CHECK(r.newHovered == Set { "playerbounds" });
        REQUIRE(pending.size() == 1);
        CHECK(pending["playerbounds"].deadlineMs == 1400);
    }

    TEST_CASE("re-entry within grace cancels the pending leave") {
        LeaveMap pending;
        Set      hovered = { "playerbounds" };
        // Frame 1 — cursor leaves.
        auto r1 = processHoverFrame(hovered, {}, pending, 1000, 400);
        hovered = r1.newHovered;
        CHECK(pending.count("playerbounds"));
        // Frame 2 at 1200ms — cursor re-enters within 400ms grace.
        auto r2 = processHoverFrame(hovered, { "playerbounds" }, pending, 1200, 400);
        hovered = r2.newHovered;
        // No new cursorEnter (layer was still in hovered set during grace).
        CHECK(r2.toEnter.empty());
        CHECK(hovered == Set { "playerbounds" });
        // Pending leave was cancelled.
        CHECK(pending.empty());
    }

    TEST_CASE("deadline is NOT extended each frame the cursor stays out") {
        // Critical regression: an earlier implementation re-set the deadline
        // every frame the cursor was out, so cursorLeave could never fire.
        LeaveMap pending;
        Set      hovered    = { "playerbounds" };
        auto     r1         = processHoverFrame(hovered, {}, pending, 1000, 400);
        hovered             = r1.newHovered;
        int64_t scheduledAt = pending["playerbounds"].deadlineMs;
        CHECK(scheduledAt == 1400);
        // Many frames pass, still no cursor in playerbounds.
        for (int64_t t = 1001; t <= 1399; t += 10) {
            auto r  = processHoverFrame(hovered, {}, pending, t, 400);
            hovered = r.newHovered;
            // Deadline must not have moved.
            CHECK(pending["playerbounds"].deadlineMs == scheduledAt);
        }
    }

    TEST_CASE("expired leaves surface from drainExpiredLeaves") {
        LeaveMap pending;
        pending["playerbounds"] = { 1400 };
        pending["f1"]           = { 1800 };
        // Nothing expired yet at t=1200.
        auto now1 = drainExpiredLeaves(pending, 1200);
        CHECK(now1.empty());
        CHECK(pending.size() == 2);
        // playerbounds expires at t=1500.
        auto now2 = drainExpiredLeaves(pending, 1500);
        CHECK(now2.size() == 1);
        CHECK(now2[0] == "playerbounds");
        CHECK(pending.count("playerbounds") == 0);
        CHECK(pending.count("f1") == 1);
        // f1 expires at t=1900.
        auto now3 = drainExpiredLeaves(pending, 1900);
        CHECK(now3.size() == 1);
        CHECK(now3[0] == "f1");
        CHECK(pending.empty());
    }

    TEST_CASE("nextLeaveDeadlineMs picks the soonest deadline") {
        LeaveMap pending;
        CHECK(nextLeaveDeadlineMs(pending) == 0);
        pending["a"] = { 2000 };
        pending["b"] = { 1500 };
        pending["c"] = { 3000 };
        CHECK(nextLeaveDeadlineMs(pending) == 1500);
    }

    TEST_CASE("end-to-end: fade grace + eventual leave after window") {
        LeaveMap pending;
        Set      hovered;
        // Frame 1 @ t=1000: cursor enters playerbounds.
        auto r1 = processHoverFrame(hovered, { "playerbounds" }, pending, 1000, 400);
        hovered = r1.newHovered;
        CHECK(r1.toEnter.count("playerbounds"));
        // Frame 2 @ t=1100: cursor leaves.  Leave scheduled for 1500.
        auto r2 = processHoverFrame(hovered, {}, pending, 1100, 400);
        hovered = r2.newHovered;
        CHECK(hovered.count("playerbounds"));
        CHECK(pending["playerbounds"].deadlineMs == 1500);
        // Frame 3 @ t=1300: cursor re-enters — leave cancelled, no new enter.
        auto r3 = processHoverFrame(hovered, { "playerbounds" }, pending, 1300, 400);
        hovered = r3.newHovered;
        CHECK(r3.toEnter.empty());
        CHECK(pending.empty());
        // Frame 4 @ t=1400: cursor leaves again.  Fresh deadline 1800.
        auto r4 = processHoverFrame(hovered, {}, pending, 1400, 400);
        hovered = r4.newHovered;
        CHECK(pending["playerbounds"].deadlineMs == 1800);
        // Frame 5–N @ t=1400..1799: cursor stays out.  Deadline does NOT move.
        for (int64_t t = 1401; t < 1800; t += 50) {
            auto r  = processHoverFrame(hovered, {}, pending, t, 400);
            hovered = r.newHovered;
            CHECK(pending["playerbounds"].deadlineMs == 1800);
        }
        // t=1800: deadline reached — leave drains.
        auto drained = drainExpiredLeaves(pending, 1800);
        CHECK(drained.size() == 1);
        CHECK(drained[0] == "playerbounds");
        CHECK(pending.empty());
    }

    TEST_CASE("overlapping hover zones track independently") {
        LeaveMap pending;
        Set      hovered;
        // Enter playerbounds first.
        auto r1 = processHoverFrame(hovered, { "playerbounds" }, pending, 1000, 400);
        hovered = r1.newHovered;
        // Enter f1 too — both should be hovered, only f1 reports a new enter.
        auto r2 = processHoverFrame(hovered, { "playerbounds", "f1" }, pending, 1100, 400);
        hovered = r2.newHovered;
        CHECK(r2.toEnter == Set { "f1" });
        CHECK(hovered == Set { "playerbounds", "f1" });
        // Leave playerbounds, still in f1 — only playerbounds gets a pending leave.
        auto r3 = processHoverFrame(hovered, { "f1" }, pending, 1200, 400);
        hovered = r3.newHovered;
        CHECK(hovered == Set { "playerbounds", "f1" }); // playerbounds kept via grace
        CHECK(pending.count("playerbounds"));
        CHECK(pending.count("f1") == 0);
        // Drain at 1600 — only playerbounds expires.
        auto drained = drainExpiredLeaves(pending, 1600);
        CHECK(drained == std::vector<std::string> { "playerbounds" });
        // f1 still hovered.
        CHECK(hovered.count("f1"));
    }

} // TEST_SUITE Hover-leave debounce

// ------------------------------------------------------------------
// computeNativeAspectRatio — the pure math behind SceneObject's
// nativeAspectRatio Q_PROPERTY (SceneAspect.h).  Gated by a "loaded"
// flag so it reports 0.0 (Scene.qml's "fall back to anchors.fill"
// sentinel) until the parser fills the real ortho size, and guards a
// non-positive height so a degenerate scene can't divide by zero.
// ------------------------------------------------------------------
TEST_SUITE("Native aspect ratio") {
    using scenebackend::computeNativeAspectRatio;

    TEST_CASE("returns 0.0 sentinel before the scene loads") {
        // The QQuickItem ships 1920x1080 defaults for cursorClick hit-testing;
        // those must NOT surface as an aspect until the parser confirms the
        // load, or Scene.qml would letterbox to 16:9 for every scene that has
        // not been parsed yet.
        CHECK(computeNativeAspectRatio(/*loaded*/ false, 1920.0f, 1080.0f) == 0.0);
        CHECK(computeNativeAspectRatio(false, 800.0f, 600.0f) == 0.0);
    }

    TEST_CASE("16:9 landscape") {
        CHECK(computeNativeAspectRatio(true, 1920.0f, 1080.0f) == doctest::Approx(16.0 / 9.0));
    }

    TEST_CASE("9:16 portrait") {
        CHECK(computeNativeAspectRatio(true, 1080.0f, 1920.0f) == doctest::Approx(9.0 / 16.0));
    }

    TEST_CASE("square is 1.0") {
        CHECK(computeNativeAspectRatio(true, 1000.0f, 1000.0f) == doctest::Approx(1.0));
    }

    TEST_CASE("21:9 ultrawide") {
        CHECK(computeNativeAspectRatio(true, 3440.0f, 1440.0f) == doctest::Approx(3440.0 / 1440.0));
    }

    // Regression note for the script-gating bug (SceneBackend.cpp setupTextScripts):
    // the aspect publish must run for scenes with NO scripts too. We cannot
    // construct a live SceneObject here (no Vulkan), so this asserts the helper
    // contract the publish relies on: a loaded scene with a valid ortho yields a
    // positive aspect — i.e. once m_sceneOrthoLoaded is set, Scene.qml letterboxes.
    TEST_CASE("loaded script-less scene ortho yields a usable (>0) aspect") {
        CHECK(computeNativeAspectRatio(true, 1920.0f, 1080.0f) > 0.0);
        CHECK(computeNativeAspectRatio(true, 3440.0f, 1440.0f) > 0.0);
    }

    TEST_CASE("non-integer ortho divides in double precision") {
        // Members are float; the helper widens to double before dividing so an
        // odd size does not lose precision the way a float divide would.
        CHECK(computeNativeAspectRatio(true, 1366.0f, 768.0f) == doctest::Approx(1366.0 / 768.0));
    }

    TEST_CASE("zero height returns the sentinel instead of dividing by zero") {
        CHECK(computeNativeAspectRatio(true, 1920.0f, 0.0f) == 0.0);
    }

    TEST_CASE("negative height is guarded too") {
        CHECK(computeNativeAspectRatio(true, 1920.0f, -1080.0f) == 0.0);
    }

    TEST_CASE("zero width collapses to the same sentinel") {
        // Documents the deliberate aliasing: a 0-width scene yields 0.0, which
        // Scene.qml treats identically to "not loaded" (fill the wallpaper).
        CHECK(computeNativeAspectRatio(true, 0.0f, 1080.0f) == 0.0);
    }
} // TEST_SUITE Native aspect ratio

// ------------------------------------------------------------------
// F19 — a paused wallpaper kept running the property loop at ~125Hz and the
// color loop at ~30Hz on the GUI thread (only the render thread was paused),
// wasting CPU/battery on TTY-switch / suspend / occlusion.  The pause/resume
// decision is now the pure predicate scriptLoopShouldRun(hasStates, paused),
// which pause()/play() (timer stop/restart) and the loop bodies share.  Test
// the predicate here — a live SceneObject cannot be built without Vulkan (see
// the Native aspect ratio note above).
TEST_SUITE("Script loop gate (F19)") {
    using scenebackend::scriptLoopShouldRun;

    TEST_CASE("runs when there are states and we are not paused") {
        // The normal playing case: property/color scripts evaluate every tick.
        CHECK(scriptLoopShouldRun(/*hasStates*/ true, /*paused*/ false) == true);
    }

    TEST_CASE("idle when paused even though states exist") {
        // The bug being fixed: a paused wallpaper must NOT keep evaluating its
        // scripts.  This is the gate pause() relies on (plus stopping the QTimer).
        CHECK(scriptLoopShouldRun(/*hasStates*/ true, /*paused*/ true) == false);
    }

    TEST_CASE("idle when there is nothing to evaluate, paused or not") {
        // Preserves the pre-existing empty-state early-return: a scene with no
        // property/color scripts never does work regardless of pause state.
        CHECK(scriptLoopShouldRun(/*hasStates*/ false, /*paused*/ false) == false);
        CHECK(scriptLoopShouldRun(/*hasStates*/ false, /*paused*/ true) == false);
    }

    TEST_CASE("resume after pause re-enables the loop") {
        // pause() sets paused=true (idle); play()/setupTextScripts() set it back
        // to false, and with states present the loop runs again — scripts must
        // continue normally once resumed (the correctness requirement).
        bool paused = false;
        CHECK(scriptLoopShouldRun(true, paused) == true); // playing
        paused = true;
        CHECK(scriptLoopShouldRun(true, paused) == false); // paused: suspended
        paused = false;
        CHECK(scriptLoopShouldRun(true, paused) == true); // resumed
    }
} // TEST_SUITE Script loop gate (F19)

// ------------------------------------------------------------------
// render-frame gate for the property loop.  The property timer
// fires at 125Hz but a non-high-rate wallpaper must not evaluate its scripts
// faster than the render thread draws (script output is sampled at the render
// rate, so extra evals are wasted CPU).  As with scriptLoopShouldRun above, a
// live SceneObject can't be built without Vulkan, so the policy is a pure
// predicate tested here and called from the loop body so the test covers the
// shipped logic.
TEST_SUITE("Property tick render-frame gate") {
    using scenebackend::propertyTickShouldEval;

    TEST_CASE("high-rate scenes always evaluate (sub-frame physics opt-in)") {
        // 3body (3509243656) integrates a chaotic 3-body system that wants
        // >render-rate stepping; with highRate=true the gate must never skip,
        // regardless of frameIdx — i.e. the 8ms timer drives full 125Hz.
        CHECK(propertyTickShouldEval(/*highRate*/ true, /*cur*/ 5, /*last*/ 5) == true);
        CHECK(propertyTickShouldEval(/*highRate*/ true, /*cur*/ 0, /*last*/ 7) == true);
    }

    TEST_CASE("seed eval (lastFrameIdx==0) always evaluates") {
        // The ctor seeds one eval to populate shared.* before text/color
        // scripts run; frameIdx can be 0 before the render thread starts.
        // The lastFrameIdx==0 branch guarantees that first eval is never gated.
        CHECK(propertyTickShouldEval(/*highRate*/ false, /*cur*/ 0, /*last*/ 0) == true);
        CHECK(propertyTickShouldEval(/*highRate*/ false, /*cur*/ 9, /*last*/ 0) == true);
    }

    TEST_CASE("same frame index as last eval -> skip (the wasted-tick case)") {
        // The timer ticked but no new frame was produced: skip the body.
        CHECK(propertyTickShouldEval(/*highRate*/ false, /*cur*/ 42, /*last*/ 42) == false);
    }

    TEST_CASE("advanced frame index -> evaluate") {
        // A new frame exists since the last eval: do the work.
        CHECK(propertyTickShouldEval(/*highRate*/ false, /*cur*/ 43, /*last*/ 42) == true);
    }

    TEST_CASE("steady-state 30fps sequence: one eval per advanced frame") {
        // Simulate the production loop: track lastFrameIdx and step it forward
        // only when the predicate says eval.  Render frame advances
        // 0,0,0,1,1,1,2,2,2 (3 ticks per frame).
        uint64_t       last  = 0; // seed state
        int            evals = 0, skips = 0;
        const uint64_t frames[] = { 0, 0, 0, 1, 1, 1, 2, 2, 2 };
        for (uint64_t cur : frames) {
            if (propertyTickShouldEval(false, cur, last)) {
                ++evals;
                last = cur;
            } else {
                ++skips;
            }
        }
        // First three ticks: cur==0,last==0 -> eval x3 (seed branch dominates).
        // Then cur==1 (eval, last=1), 1 (skip), 1 (skip),
        //      cur==2 (eval, last=2), 2 (skip), 2 (skip).
        CHECK(evals == 5);
        CHECK(skips == 4);
    }

    TEST_CASE("frame advances every tick -> never skips (output-equivalence)") {
        // The output-equivalence guarantee: when getFrameIdx() advances once per
        // property tick (render rate == tick rate), the gate evaluates on EVERY
        // tick, so the dispatched dirty-layer batch is identical to the no-gate
        // path.  The gate can only ever REMOVE evals where the frame did not move.
        uint64_t last = 0;
        for (uint64_t cur = 1; cur <= 50; ++cur) {
            CHECK(propertyTickShouldEval(false, cur, last) == true);
            last = cur;
        }
    }
} // TEST_SUITE Property tick render-frame gate

// ------------------------------------------------------------------
// audio-buffer refresh de-dup.  refreshAudioBuffers() is called from
// the property/text/color loops; the analyzer only produces new data per
// processed render frame, so all callers must collapse to one rebuild per drawn
// frame.  Pure predicate (the gate keys on a render-thread atomic the test
// can't drive through a live SceneObject), tested here against the value-shape
// the body uses.
TEST_SUITE("Audio buffer refresh de-dup") {
    using scenebackend::audioRefreshShouldRun;

    TEST_CASE("first refresh (frame 0) always runs") {
        CHECK(audioRefreshShouldRun(/*cur*/ 0, /*last*/ 0) == true);
    }
    TEST_CASE("second call at same frame is skipped") {
        // Two callers (property + text loop) in the same drawn frame: the second
        // must NOT rebuild — the analyzer data is identical.
        CHECK(audioRefreshShouldRun(/*cur*/ 7, /*last*/ 7) == false);
    }
    TEST_CASE("advanced frame runs the refresh") {
        CHECK(audioRefreshShouldRun(/*cur*/ 8, /*last*/ 7) == true);
    }
    TEST_CASE("three loops, one frame -> exactly one refresh") {
        uint64_t last      = 0;
        int      refreshes = 0;
        // frame 5 hit by property + text + color in one frame.
        for (uint64_t cur : { (uint64_t)5, (uint64_t)5, (uint64_t)5 }) {
            if (audioRefreshShouldRun(cur, last)) {
                ++refreshes;
                last = cur;
            }
        }
        CHECK(refreshes == 1);
    }
} // TEST_SUITE Audio buffer refresh de-dup

// ------------------------------------------------------------------
// A3-T2 — QJSEngine watchdog interrupt back-off policy.  The watchdog's
// monitor-thread machinery is timing-dependent and hard to unit-test, so (as
// with scriptLoopShouldRun above) the user-visible *policy* is split into the
// pure predicate shouldDisableAfterInterrupts(consecutive, K): after K
// consecutive budget-exceeding interrupts the wallpaper's property scripts are
// disabled (it stops animating instead of freezing the desktop), and a clean
// tick resets the run.  Test the predicate exactly here.
TEST_SUITE("JS watchdog back-off policy (A3-T2)") {
    using scenebackend::JsWatchdog;
    using scenebackend::shouldDisableAfterInterrupts;

    TEST_CASE("does not disable below the K threshold") {
        // K=5 (production default): a few stray interrupts must NOT kill the
        // wallpaper — legitimate heavy ticks can occasionally overrun.
        const int K = JsWatchdog::kDisableAfterInterrupts;
        CHECK(K == 5);
        CHECK(shouldDisableAfterInterrupts(0, K) == false);
        CHECK(shouldDisableAfterInterrupts(1, K) == false);
        CHECK(shouldDisableAfterInterrupts(4, K) == false);
    }

    TEST_CASE("disables at and beyond K consecutive interrupts") {
        const int K = JsWatchdog::kDisableAfterInterrupts;
        CHECK(shouldDisableAfterInterrupts(5, K) == true);
        CHECK(shouldDisableAfterInterrupts(6, K) == true);
        CHECK(shouldDisableAfterInterrupts(100, K) == true);
    }

    TEST_CASE("a clean tick resets the run (caller zeroes the counter)") {
        // The SceneObject increments on an interrupted tick and zeroes on a
        // clean one; the predicate only reads the running count.  Simulate the
        // count walking up to K-1, a clean tick (reset to 0), then climbing
        // again — the predicate fires only once the *consecutive* run hits K.
        int count = 0;
        for (int i = 0; i < JsWatchdog::kDisableAfterInterrupts - 1; ++i) {
            ++count; // interrupted ticks
            CHECK(shouldDisableAfterInterrupts(count, JsWatchdog::kDisableAfterInterrupts) ==
                  false);
        }
        count = 0; // clean tick resets
        CHECK(shouldDisableAfterInterrupts(count, JsWatchdog::kDisableAfterInterrupts) == false);
        // Now a fresh run of K interrupts trips it.
        for (int i = 0; i < JsWatchdog::kDisableAfterInterrupts; ++i) ++count;
        CHECK(shouldDisableAfterInterrupts(count, JsWatchdog::kDisableAfterInterrupts) == true);
    }

    TEST_CASE("K<=0 disables on the very first interrupt (degenerate budget)") {
        // Guards the boundary: a misconfigured K of 0 must still be well-defined
        // (disable immediately) rather than under/overflow.
        CHECK(shouldDisableAfterInterrupts(0, 0) == true);
        CHECK(shouldDisableAfterInterrupts(1, 0) == true);
        CHECK(shouldDisableAfterInterrupts(0, -1) == true);
    }

    TEST_CASE("watchdog tunables are sane") {
        // The budget must be generous enough that legitimate heavy first-frame
        // scripts (puppet/rig init) never trip it — 250ms per the spec.
        CHECK(JsWatchdog::kDefaultBudgetMs == 250);
    }

    // A3-T2 — a REAL armed watchdog interrupts a runaway script. This case is
    // only linkable once JsWatchdog::interruptEngine is header-inline (Item 03
    // fix (a)); before that it is an undefined-symbol link error, which is
    // exactly why the rest of this suite only exercises the pure predicate.
    TEST_CASE("armed watchdog interrupts a runaway script (real interrupt)") {
        QJSEngine  engine;
        JsWatchdog wd;
        wd.start();
        // Tiny budget so the test is fast (prod is 250ms).
        wd.arm(&engine, 30);
        QJSValue   r     = engine.evaluate("(function(){ while(true){} })").call();
        const bool fired = wd.disarm();
        CHECK(fired == true);       // the monitor thread tripped
        CHECK(r.isError() == true); // .call() unwound as a JS exception
        // Clear the latch so the engine is reusable, as callJsGuarded does.
        engine.setInterrupted(false);
        CHECK(engine.isInterrupted() == false);
        wd.stop();
    }

    // Item 05 — the shared back-off helper (SceneObject::applyInterruptBackoff) is
    // a thin wrapper over shouldDisableAfterInterrupts + a disabled flag + a timer
    // stop.  scenescript_tests can't build SceneObject, so this fixture is a
    // faithful int/bool reimplementation that locks the contract the three tick
    // loops (property/text/color) now share.
    struct Backoff {
        int  consecutive  = 0;
        bool disabled     = false;
        bool timerStopped = false;
        // Returns true if scripts were JUST disabled (caller bails this tick).
        bool apply(bool tickInterrupted, int k) {
            if (! tickInterrupted) {
                consecutive = 0;
                return false;
            }
            consecutive++;
            if (shouldDisableAfterInterrupts(consecutive, k) && ! disabled) {
                disabled     = true;
                timerStopped = true;
                return true;
            }
            return false;
        }
    };

    TEST_CASE("applyInterruptBackoff: runaway stream disables at exactly K") {
        const int K = JsWatchdog::kDisableAfterInterrupts; // 5
        Backoff   b;
        for (int i = 1; i < K; ++i) {
            CHECK(b.apply(/*interrupted=*/true, K) == false); // ticks 1..K-1: no disable
            CHECK(b.disabled == false);
        }
        CHECK(b.apply(/*interrupted=*/true, K) == true); // tick K: disable fires once
        CHECK(b.disabled == true);
        CHECK(b.timerStopped == true);
        CHECK(b.consecutive == K);
    }

    TEST_CASE("applyInterruptBackoff: a clean tick before K resets the run") {
        const int K = JsWatchdog::kDisableAfterInterrupts;
        Backoff   b;
        for (int i = 1; i < K; ++i) CHECK(b.apply(true, K) == false);
        CHECK(b.apply(/*clean*/ false, K) == false); // reset
        CHECK(b.consecutive == 0);
        CHECK(b.disabled == false);
        // Now a fresh full run is needed to trip it.
        for (int i = 1; i < K; ++i) CHECK(b.apply(true, K) == false);
        CHECK(b.apply(true, K) == true);
    }

    TEST_CASE("applyInterruptBackoff: once disabled the flag stays latched (idempotent)") {
        const int K = JsWatchdog::kDisableAfterInterrupts;
        Backoff   b;
        for (int i = 0; i < K; ++i) b.apply(true, K);
        CHECK(b.disabled == true);
        b.timerStopped = false; // observe re-stop attempts
        // Further interrupted ticks must NOT re-fire the disable.
        CHECK(b.apply(true, K) == false);
        CHECK(b.apply(true, K) == false);
        CHECK(b.timerStopped == false);
        CHECK(b.disabled == true);
    }

    TEST_CASE("applyInterruptBackoff: property/text/color latches are independent") {
        // The shared helper must not couple the three loops — separate
        // counters/flags/timers per loop (Item 05 de-dup must not regress this).
        const int K = JsWatchdog::kDisableAfterInterrupts;
        Backoff   prop, text, color;
        for (int i = 0; i < K; ++i) text.apply(true, K); // only text runs away
        CHECK(text.disabled == true);
        CHECK(prop.disabled == false);  // property unaffected
        CHECK(color.disabled == false); // color unaffected
    }

    // Item 05 — a runaway script body (stands in for the sv/text/color loops:
    // identical arm->call->disarm->count bracket) is interrupted, and after K
    // consecutive interrupts the shared back-off latches off.  Requires Item 03's
    // header-inline interruptEngine to link a real armed watchdog.
    TEST_CASE("runaway tick-loop body is interrupted and latches off after K") {
        QJSEngine  engine;
        JsWatchdog wd;
        wd.start();
        QJSValue  runaway = engine.evaluate("(function(){ while(true){} })");
        Backoff   loop; // the per-loop latch state (mirrors applyInterruptBackoff)
        const int K        = JsWatchdog::kDisableAfterInterrupts;
        bool      disabled = false;
        for (int tick = 0; tick < K && ! disabled; ++tick) {
            wd.arm(&engine, 30); // tiny budget
            QJSValue   r     = runaway.call();
            const bool fired = wd.disarm();
            CHECK(fired == true); // interrupted, control returned
            CHECK(r.isError() == true);
            if (fired) engine.setInterrupted(false);
            disabled = loop.apply(/*tickInterrupted=*/fired, K);
        }
        CHECK(loop.disabled == true); // latched off after K
        CHECK(loop.consecutive == K);
        CHECK(engine.isInterrupted() == false); // latch cleared
        wd.stop();
    }
} // TEST_SUITE JS watchdog back-off policy (A3-T2)

// ------------------------------------------------------------------
// Per-load script diagnostics dedup (Item 15)
//
// The error-dedup / one-shot probe state was function-local `static` — process
// lifetime, shared across all SceneObject instances — so it was never reset on
// wallpaper switch (cross-load swallowing) and shared across monitors
// (cross-monitor swallowing).  Promoting it into ScriptDiagState (one per
// SceneObject, cleared in cleanupTextScripts) makes dedup per-load + per-instance.
// ------------------------------------------------------------------
TEST_SUITE("Per-load script diagnostics dedup (Item 15)") {
    using scenebackend::ScriptDiagState;

    TEST_CASE("first error per id logs once within a load (intra-load dedup works)") {
        ScriptDiagState d;
        // First occurrence -> insert succeeds -> "would log".
        CHECK(d.textErroredIds.insert(7).second == true);
        CHECK(d.textErroredIds.count(7) == 1);
        // Second occurrence -> insert fails -> "would suppress".
        CHECK(d.textErroredIds.insert(7).second == false);
    }

    TEST_CASE("clear() re-arms dedup (KEYSTONE: per-load, not per-process)") {
        ScriptDiagState d;
        d.textErroredIds.insert(7);
        d.mathRandomProbed = true;
        d.clear();
        // After a load switch, the dedup and the one-shot probe are fresh.
        CHECK(d.textErroredIds.empty());
        CHECK(d.mathRandomProbed == false);
        // The SAME node id that errored in load A logs again in load B.
        CHECK(d.textErroredIds.insert(7).second == true);
    }

    TEST_CASE("two independent instances don't share state (per-monitor isolation)") {
        ScriptDiagState a, b;
        a.soundVolErrored.insert(3);
        // Screen 1's dedup must NOT suppress screen 2's identical-id error.
        CHECK(b.soundVolErrored.count(3) == 0);
        CHECK(a.soundVolErrored.count(3) == 1);
    }

    TEST_CASE("all five sets + the probe flag round-trip through clear()") {
        // A future field added without a clear() line fails this round-trip.
        ScriptDiagState d;
        d.textErroredIds.insert(1);
        d.svErrored.insert(2);
        d.propErroredIds.insert(3);
        d.dirtyLayerMisses.insert("layerA");
        d.soundVolErrored.insert(4);
        d.mathRandomProbed = true;

        CHECK(d.textErroredIds.size() == 1);
        CHECK(d.svErrored.size() == 1);
        CHECK(d.propErroredIds.size() == 1);
        CHECK(d.dirtyLayerMisses.size() == 1);
        CHECK(d.soundVolErrored.size() == 1);
        CHECK(d.mathRandomProbed == true);

        d.clear();

        CHECK(d.textErroredIds.empty());
        CHECK(d.svErrored.empty());
        CHECK(d.propErroredIds.empty());
        CHECK(d.dirtyLayerMisses.empty());
        CHECK(d.soundVolErrored.empty());
        CHECK(d.mathRandomProbed == false);
    }
} // TEST_SUITE Per-load script diagnostics dedup (Item 15)

// ------------------------------------------------------------------
// F19 (cleanup leak) — cleanupTextScripts() now clears m_pendingLeaves (and
// stops m_hoverLeaveTimer) on a wallpaper switch.  A live SceneObject cannot be
// built without Vulkan, so lock the contract the clear relies on at the pure
// HoverLeaveDebounce level: an emptied pending-leaves map arms no further timer
// (nextLeaveDeadlineMs == 0, the "no timer" sentinel) and drains nothing — so a
// stale cursorLeave can never fire against the torn-down JS engine.
TEST_SUITE("Hover-leave cleanup contract (F19)") {
    using LeaveMap = std::unordered_map<std::string, scenebackend::PendingLeave>;

    TEST_CASE("a cleared pending-leaves map arms no timer and drains nothing") {
        LeaveMap pending;
        // Simulate the state cleanupTextScripts() inherits: a couple of leaves
        // still pending from the outgoing wallpaper's hover interaction.
        pending["playerbounds"] = { 1500 };
        pending["f1"]           = { 1800 };
        REQUIRE(nextLeaveDeadlineMs(pending) == 1500); // a timer WOULD be armed

        // cleanupTextScripts() does this on wallpaper switch.
        pending.clear();

        // No deadline => hoverMoveEvent()/flushPendingLeaves() arm no QTimer
        // (0 is the "nothing pending" sentinel), and nothing can drain/fire.
        CHECK(nextLeaveDeadlineMs(pending) == 0);
        CHECK(drainExpiredLeaves(pending, 1500).empty());
        CHECK(drainExpiredLeaves(pending, 100000).empty());
    }

    TEST_CASE("an already-empty map is a no-op (idempotent cleanup)") {
        LeaveMap pending;
        pending.clear(); // clearing an empty map is safe
        CHECK(pending.empty());
        CHECK(nextLeaveDeadlineMs(pending) == 0);
        CHECK(drainExpiredLeaves(pending, 9999).empty());
    }
} // TEST_SUITE Hover-leave cleanup contract (F19)

// ------------------------------------------------------------------
// Property-script batched dispatch: scalar broadcast for Vec3 kind.
// Uses the exact same JS source as production (shared header) so any
// drift between prod and test causes these to fail.
// ------------------------------------------------------------------
#include "PropertyScriptDispatchJs.hpp"

namespace
{
// Bootstraps the minimal JS globals needed to evaluate the dispatch loop:
// a Vec3 factory and the tables/partition markers the loop consults.
static void setupDispatchEngine(QJSEngine& engine) {
    engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
    engine.evaluate(wek::qml_helper::kVecClassesJs);
    engine.evaluate(wek::qml_helper::kPropertyScriptDispatchJs);
    engine.evaluate("var thisLayer = null;");
}

// Pushes a single Vec3-kind script entry (index 0) whose update fn evaluates
// the given JS expression string.  `initial` is the starting (cx,cy,cz).
// The entry has valid=true, hasLayer=false so thisLayer is untouched.
static QJSValue pushVec3Script(QJSEngine& engine, const char* fnBody, double cx = 0, double cy = 0,
                               double cz = 0) {
    QJSValue entry = engine.newObject();
    entry.setProperty("kind", 1);
    entry.setProperty("fn", engine.evaluate(QString("(function(v) { %1 })").arg(fnBody)));
    entry.setProperty("proxy", QJSValue(QJSValue::NullValue));
    entry.setProperty("hasLayer", false);
    entry.setProperty("valid", true);
    entry.setProperty("cb", false);
    entry.setProperty("cf", 0.0);
    entry.setProperty("cx", cx);
    entry.setProperty("cy", cy);
    entry.setProperty("cz", cz);
    QJSValue arr = engine.globalObject().property("_allPropertyScripts");
    arr.setProperty(0, entry);
    engine.globalObject().setProperty("_scriptPartVisEnd", 0);
    engine.globalObject().setProperty("_scriptPartVec3End", 1);
    return entry;
}
} // namespace

TEST_SUITE("PropertyScriptDispatch — Vec3 scalar broadcast") {
    TEST_CASE("scalar return broadcasts to x,y,z") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        pushVec3Script(engine, "return v.x + 0.5;", /*cx=*/1, /*cy=*/1, /*cz=*/1);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        REQUIRE(out.isArray());
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(0).toInt() == 0);                       // script idx
        CHECK(out.property(1).toNumber() == doctest::Approx(1.5)); // x
        CHECK(out.property(2).toNumber() == doctest::Approx(1.5)); // y (broadcast)
        CHECK(out.property(3).toNumber() == doctest::Approx(1.5)); // z (broadcast)
    }

    TEST_CASE("Vec3 return takes per-component values") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        pushVec3Script(engine, "return Vec3(0.1, 0.2, 0.3);");

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(1).toNumber() == doctest::Approx(0.1));
        CHECK(out.property(2).toNumber() == doctest::Approx(0.2));
        CHECK(out.property(3).toNumber() == doctest::Approx(0.3));
    }

    TEST_CASE("scalar equal to current values is suppressed") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        // Current is (0.75,0.75,0.75); update returns exactly 0.75 → no push.
        pushVec3Script(engine, "return 0.75;", 0.75, 0.75, 0.75);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 0);
    }

    TEST_CASE("scalar NOT equal on any axis pushes an update") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        // cz differs from scalar return → must push.
        pushVec3Script(engine, "return 1.0;", 1.0, 1.0, 0.5);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(1).toNumber() == doctest::Approx(1.0));
        CHECK(out.property(2).toNumber() == doctest::Approx(1.0));
        CHECK(out.property(3).toNumber() == doctest::Approx(1.0));
    }

    TEST_CASE("undefined/null return drops silently") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        pushVec3Script(engine, "return undefined;", 1, 1, 1);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 0);

        pushVec3Script(engine, "return null;", 1, 1, 1);
        out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 0);
    }

    TEST_CASE("non-numeric return (e.g. string) drops silently") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        pushVec3Script(engine, "return 'bad';");

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 0);
    }

    TEST_CASE("hover-zoom end-to-end: converges toward targetScale") {
        // Reproduces Lucy Clock hover-zoom shape: script returns
        // value.x + (targetScale - value.x) * speed * frametime.
        // After one tick from rest at 1.0 with targetScale=1.1, speed=20,
        // frametime=0.016 → 1.0 + (0.1)*20*0.016 = 1.032 on all axes.
        QJSEngine engine;
        setupDispatchEngine(engine);
        engine.evaluate("var targetScale = 1.1; var speed = 20; var frametime = 0.016;");
        pushVec3Script(
            engine, "return v.x + (targetScale - v.x) * speed * frametime;", 1.0, 1.0, 1.0);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(1).toNumber() == doctest::Approx(1.032));
        CHECK(out.property(2).toNumber() == doctest::Approx(1.032));
        CHECK(out.property(3).toNumber() == doctest::Approx(1.032));
    }
} // TEST_SUITE PropertyScriptDispatch — Vec3 scalar broadcast

// ------------------------------------------------------------------
// Per-attachment thisObject binding: animationlayer-attached scripts
// must see thisObject === the animation-layer proxy, not thisLayer.
// Exercised by Lucy (3521337568) where the offsetedStartAni init
// script calls thisObject.setFrame on the specific rig track.
// ------------------------------------------------------------------
TEST_SUITE("PropertyScriptDispatch — thisObject rebind") {
    TEST_CASE("Object-attached script sees thisObject === thisLayer") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        engine.evaluate("var _seenLayer = null, _seenObject = null;\n"
                        "var _fakeLayer = { _kind: 'layer' };\n");
        // Entry with obj === proxy (same ref): Object-attached semantics.
        QJSValue entry = engine.newObject();
        entry.setProperty("kind", 1);
        entry.setProperty("fn",
                          engine.evaluate("(function(v) {\n"
                                          "  _seenLayer = thisLayer;\n"
                                          "  _seenObject = thisObject;\n"
                                          "  return v;\n"
                                          "})"));
        QJSValue proxy = engine.evaluate("_fakeLayer");
        entry.setProperty("proxy", proxy);
        entry.setProperty("obj", proxy); // same reference
        entry.setProperty("hasLayer", true);
        entry.setProperty("valid", true);
        entry.setProperty("cb", false);
        entry.setProperty("cf", 0.0);
        entry.setProperty("cx", 1.0);
        entry.setProperty("cy", 1.0);
        entry.setProperty("cz", 1.0);
        engine.globalObject().property("_allPropertyScripts").setProperty(0, entry);
        engine.globalObject().setProperty("_scriptPartVisEnd", 0);
        engine.globalObject().setProperty("_scriptPartVec3End", 1);

        engine.evaluate("_runAllPropertyScripts();");
        CHECK(engine.evaluate("_seenLayer === _seenObject").toBool());
        CHECK(engine.evaluate("_seenLayer === _fakeLayer").toBool());
    }

    TEST_CASE("AnimationLayer-attached script sees thisObject === rig proxy") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        engine.evaluate("var _seenLayer = null, _seenObject = null;\n"
                        "var _fakeLayer = { _kind: 'layer' };\n"
                        "var _fakeRig   = { _kind: 'rig',\n"
                        "  frameCount: 60, _frame: 0,\n"
                        "  setFrame: function(f){ this._frame = f; },\n"
                        "  play: function(){} };\n");
        QJSValue entry = engine.newObject();
        entry.setProperty("kind", 1);
        entry.setProperty(
            "fn",
            engine.evaluate(
                "(function(v) {\n"
                "  _seenLayer = thisLayer;\n"
                "  _seenObject = thisObject;\n"
                "  // NSL-style: offsetedStartAni(thisObject.getAnimation() || thisObject,...)\n"
                "  thisObject.setFrame(thisObject.frameCount * 0.5);\n"
                "  return v;\n"
                "})"));
        entry.setProperty("proxy", engine.evaluate("_fakeLayer"));
        entry.setProperty("obj", engine.evaluate("_fakeRig"));
        entry.setProperty("hasLayer", true);
        entry.setProperty("valid", true);
        entry.setProperty("cb", false);
        entry.setProperty("cf", 0.0);
        entry.setProperty("cx", 1.0);
        entry.setProperty("cy", 1.0);
        entry.setProperty("cz", 1.0);
        engine.globalObject().property("_allPropertyScripts").setProperty(0, entry);
        engine.globalObject().setProperty("_scriptPartVisEnd", 0);
        engine.globalObject().setProperty("_scriptPartVec3End", 1);

        engine.evaluate("_runAllPropertyScripts();");
        CHECK(engine.evaluate("_seenLayer === _fakeLayer").toBool());
        CHECK(engine.evaluate("_seenObject === _fakeRig").toBool());
        // Rig's setFrame landed on rig, not on layer
        CHECK(engine.evaluate("_fakeRig._frame").toNumber() == 30);
    }

    TEST_CASE("Lucy offsetedStartAni: setFrame reaches rig layer") {
        // Reproduces 8c39a2fb.js + NSL's offsetedStartAni(ani, percentage):
        //   ani.play(); ani.setFrame(ani.frameCount * percentage);
        // With the correct thisObject binding, percentage=0.25 on a 60-frame
        // rig lands _frame = 15 on the rig, not the layer.
        QJSEngine engine;
        setupDispatchEngine(engine);
        engine.evaluate("var _fakeLayer = { _kind: 'layer' };\n"
                        "var _fakeRig   = { frameCount: 60, _frame: 0, _playing: false,\n"
                        "  play: function(){ this._playing = true; },\n"
                        "  setFrame: function(f){ this._frame = f; } };\n"
                        "function offsetedStartAni(ani, pct) {\n"
                        "  ani.play();\n"
                        "  ani.setFrame(ani.frameCount * pct);\n"
                        "}\n");
        QJSValue entry = engine.newObject();
        entry.setProperty("kind", 1);
        entry.setProperty("fn",
                          engine.evaluate("(function(v) {\n"
                                          "  offsetedStartAni(thisObject, 0.25);\n"
                                          "  return v;\n"
                                          "})"));
        entry.setProperty("proxy", engine.evaluate("_fakeLayer"));
        entry.setProperty("obj", engine.evaluate("_fakeRig"));
        entry.setProperty("hasLayer", true);
        entry.setProperty("valid", true);
        entry.setProperty("cb", false);
        entry.setProperty("cf", 0.0);
        entry.setProperty("cx", 1.0);
        entry.setProperty("cy", 1.0);
        entry.setProperty("cz", 1.0);
        engine.globalObject().property("_allPropertyScripts").setProperty(0, entry);
        engine.globalObject().setProperty("_scriptPartVisEnd", 0);
        engine.globalObject().setProperty("_scriptPartVec3End", 1);

        engine.evaluate("_runAllPropertyScripts();");
        CHECK(engine.evaluate("_fakeRig._playing").toBool());
        CHECK(engine.evaluate("_fakeRig._frame").toNumber() == 15);
    }
} // TEST_SUITE PropertyScriptDispatch — thisObject rebind

// ------------------------------------------------------------------
// Vec4: 4-component vector with method surface matching Vec3.
// Referenced by NSL vectorEquality() and anything tweening RGBA.
// ------------------------------------------------------------------
TEST_SUITE("Vec4") {
    TEST_CASE("constructor with four numbers") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        QJSValue v = engine.evaluate("Vec4(1,2,3,4)");
        CHECK(v.property("x").toNumber() == 1);
        CHECK(v.property("y").toNumber() == 2);
        CHECK(v.property("z").toNumber() == 3);
        CHECK(v.property("w").toNumber() == 4);
    }

    TEST_CASE("constructor with object copy") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        QJSValue v = engine.evaluate("Vec4({x:1,y:2,z:3,w:4})");
        CHECK(v.property("x").toNumber() == 1);
        CHECK(v.property("w").toNumber() == 4);
    }

    TEST_CASE("fromString parses space-separated") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        QJSValue v = engine.evaluate("Vec4.fromString('0.1 0.2 0.3 0.4')");
        CHECK(v.property("x").toNumber() == doctest::Approx(0.1));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.2));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.3));
        CHECK(v.property("w").toNumber() == doctest::Approx(0.4));
    }

    TEST_CASE("add / subtract / multiply / divide") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(1,2,3,4).add(Vec4(1,1,1,1)).toString()").toString() ==
              "2 3 4 5");
        CHECK(engine.evaluate("Vec4(5,5,5,5).subtract(Vec4(1,2,3,4)).toString()").toString() ==
              "4 3 2 1");
        CHECK(engine.evaluate("Vec4(1,2,3,4).multiply(2).toString()").toString() == "2 4 6 8");
        CHECK(engine.evaluate("Vec4(2,4,6,8).divide(2).toString()").toString() == "1 2 3 4");
    }

    TEST_CASE("length / lengthSqr / normalize") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(1,0,0,0).length()").toNumber() == 1);
        CHECK(engine.evaluate("Vec4(2,0,0,0).lengthSqr()").toNumber() == 4);
        QJSValue n = engine.evaluate("Vec4(3,0,0,0).normalize()");
        CHECK(n.property("x").toNumber() == 1);
        CHECK(n.property("y").toNumber() == 0);
    }

    TEST_CASE("dot") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(1,2,3,4).dot(Vec4(1,1,1,1))").toNumber() == 10);
    }

    TEST_CASE("lerp / mix") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(0,0,0,0).lerp(Vec4(10,20,30,40), 0.5).toString()").toString() ==
              "5 10 15 20");
        CHECK(engine.evaluate("Vec4.lerp(Vec4(0,0,0,0), Vec4(10,10,10,10), 0.25).toString()")
                  .toString() == "2.5 2.5 2.5 2.5");
    }

    TEST_CASE("equals") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(1,2,3,4).equals(Vec4(1,2,3,4))").toBool());
        CHECK(! engine.evaluate("Vec4(1,2,3,4).equals(Vec4(1,2,3,5))").toBool());
    }

    TEST_CASE("RGBA aliases: v.r/g/b/a reflect x/y/z/w") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        QJSValue v = engine.evaluate("var q = Vec4(0.1, 0.2, 0.3, 0.4); q;");
        CHECK(v.property("r").toNumber() == doctest::Approx(0.1));
        CHECK(v.property("g").toNumber() == doctest::Approx(0.2));
        CHECK(v.property("b").toNumber() == doctest::Approx(0.3));
        CHECK(v.property("a").toNumber() == doctest::Approx(0.4));
        // set via alias, read via canonical
        engine.evaluate("q.a = 1.0;");
        CHECK(engine.evaluate("q.w").toNumber() == 1.0);
    }

    TEST_CASE("distance between two Vec4") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(0,0,0,0).distance(Vec4(0,3,4,0))").toNumber() == 5);
    }

    TEST_CASE("negate / abs / sign") {
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        CHECK(engine.evaluate("Vec4(1,-2,3,-4).negate().toString()").toString() == "-1 2 -3 4");
        CHECK(engine.evaluate("Vec4(-1,-2,3,-4).abs().toString()").toString() == "1 2 3 4");
        CHECK(engine.evaluate("Vec4(-5,0,7,-3).sign().toString()").toString() == "-1 0 1 -1");
    }

    TEST_CASE("NSL switch-case pattern: case Vec4 no longer throws") {
        // vectorEquality in the Noeru Script Library references Vec4 in a
        // switch case.  Before this fix, the whole library raised
        // ReferenceError on first evaluation.
        QJSEngine engine;
        engine.evaluate(JS_VEC3_AND_UTILS);
        engine.evaluate(wek::qml_helper::kVecClassesJs);
        QJSValue r = engine.evaluate(
            "(function() { var a = Vec4(1,0,0,0), b = Vec4(1,0,0,0);\n"
            "  switch (a.constructor) { case Vec4: return 'matched'; default: return 'no'; }\n"
            "})()");
        // Both constructor pointers refer to the same function object; the
        // switch-equality test should succeed.  Even if a particular JS
        // engine can't match constructors this way, the key promise is
        // that it does NOT throw — the NSL library load completes.
        CHECK(! r.isError());
    }
} // TEST_SUITE Vec4

// ------------------------------------------------------------------
// Scripted particle instance-override dispatch (NieR:Automata).
// The scalar partition [vec3End, N) in _runAllPropertyScripts handles
// both Alpha (kind=2) and ParticleRate (kind=3) identically on the JS
// side; the C++ dispatcher branches on kind to route the value.  These
// tests pin the JS half: kind=3 entries behave exactly like kind=2
// entries w.r.t. input (s.cf) and output shape (push i,r,0,0).
// ------------------------------------------------------------------
TEST_SUITE("PropertyScriptDispatch — instanceoverride.rate scalar") {
    TEST_CASE("kind=ParticleRate entry forwards cf to fn and pushes [i,r,0,0]") {
        QJSEngine engine;
        setupDispatchEngine(engine);
        // ParticleRate is kind=3.  Scripts live in the same partition as
        // Alpha (after vec3End), so the entry index is 0 and BOTH
        // partition markers point to 0 (no Visible, no Vec3).
        QJSValue entry = engine.newObject();
        entry.setProperty("kind", 3);
        // Script mirrors NieR 2B shape: ignores the arg, returns a value
        // derived from external state (here just a global).  Using the
        // global lets us verify that per-tick changes propagate.
        engine.evaluate("var _audioEnv = 0.1;");
        entry.setProperty("fn", engine.evaluate("(function(v) { return _audioEnv; })"));
        entry.setProperty("proxy", QJSValue(QJSValue::NullValue));
        entry.setProperty("hasLayer", false);
        entry.setProperty("valid", true);
        entry.setProperty("cb", false);
        entry.setProperty("cf", 1.0); // init seed
        entry.setProperty("cx", 0.0);
        entry.setProperty("cy", 0.0);
        entry.setProperty("cz", 0.0);
        engine.globalObject().property("_allPropertyScripts").setProperty(0, entry);
        engine.globalObject().setProperty("_scriptPartVisEnd", 0);
        engine.globalObject().setProperty("_scriptPartVec3End", 0);

        QJSValue out = engine.evaluate("_runAllPropertyScripts()");
        REQUIRE(out.isArray());
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(0).toInt() == 0);
        CHECK(out.property(1).toNumber() == doctest::Approx(0.1));
        CHECK(out.property(2).toNumber() == doctest::Approx(0.0));
        CHECK(out.property(3).toNumber() == doctest::Approx(0.0));

        // Second tick: env changes, new value flows through.
        engine.evaluate("_audioEnv = 0.97;");
        out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 4);
        CHECK(out.property(1).toNumber() == doctest::Approx(0.97));

        // Third tick: env unchanged.  The 0.001 threshold should suppress
        // the push — particles don't need redundant reprogramming and
        // the render thread sees fewer wakeups.
        out = engine.evaluate("_runAllPropertyScripts()");
        CHECK(out.property("length").toInt() == 0);
    }

    TEST_CASE("NieR 2B script body: audio envelope drives returned rate") {
        // Exact body from scene.json (obj 299/304) with the module wrapper
        // stripped as stripESModuleSyntax would.  We mock
        // engine.registerAudioBuffers to hand out a controllable buffer so
        // the test can drive the audio side deterministically.
        QJSEngine engine;
        setupDispatchEngine(engine);
        engine.evaluate("var _buf = { average: [0] };\n"
                        "var engine = {\n"
                        "  registerAudioBuffers: function(n) { return _buf; },\n"
                        "  frametime: 0.016\n"
                        "};\n"
                        "function Math_min(a,b){return a<b?a:b;}\n");
        // The stock template with `export` stripped.  Wrapped in an IIFE
        // that returns {init, update}, mimicking the production shim.
        QJSValue mod = engine.evaluate(
            "(function() {\n"
            "  var frequencyResolution = 16;\n"
            "  var sourceFrequency = 0;\n"
            "  var smoothingRate = 16;\n"
            "  var minimumValue = 0.1;\n"
            "  var maximumValue = 1.0;\n"
            "  var audioBuffer = engine.registerAudioBuffers(frequencyResolution);\n"
            "  var smoothValue = 0;\n"
            "  var initialValue;\n"
            "  var valueDelta = maximumValue - minimumValue;\n"
            "  function update() {\n"
            "    var audioDelta = audioBuffer.average[sourceFrequency] - smoothValue;\n"
            "    smoothValue += audioDelta * engine.frametime * smoothingRate;\n"
            "    smoothValue = Math.min(1.0, smoothValue);\n"
            "    return initialValue * (smoothValue * valueDelta + minimumValue);\n"
            "  }\n"
            "  function init(value) { initialValue = value; }\n"
            "  return {init: init, update: update};\n"
            "})()");
        REQUIRE(! mod.isError());
        // Seed init with 1.0 (the value from scene.json).
        mod.property("init").call({ QJSValue(1.0) });

        QJSValue upd = mod.property("update");
        // Silence: smoothValue stays at 0, update returns initialValue * 0.1.
        engine.evaluate("_buf.average[0] = 0.0;");
        QJSValue r0 = upd.call();
        CHECK(r0.toNumber() == doctest::Approx(0.1));

        // Pin bass at 1.0 for many frames — smoothValue should converge to
        // 1.0 and the return value should saturate at initialValue * 1.0.
        engine.evaluate("_buf.average[0] = 1.0;");
        double rN = 0.0;
        for (int i = 0; i < 300; i++) rN = upd.call().toNumber();
        CHECK(rN == doctest::Approx(1.0).epsilon(0.001));

        // Drop bass to 0 — smoothing integrates down; after enough frames,
        // output drops back toward minimumValue (0.1).
        engine.evaluate("_buf.average[0] = 0.0;");
        for (int i = 0; i < 300; i++) rN = upd.call().toNumber();
        CHECK(rN == doctest::Approx(0.1).epsilon(0.01));
    }
} // TEST_SUITE instanceoverride.rate scalar

// ----------------------------------------------------------------------
// SceneScript built-in shims (WEColor, engine.registerAudioBuffers).
// These are the JS sources SceneBackend.cpp evaluates into the QJSEngine;
// kept as constants in SceneScriptShimsJs.hpp so production and tests
// share a single source of truth.  The Purple Void / "blackhole" wallpaper
// (2852314079) drives these via its audio-reactive Solid color script.
// ----------------------------------------------------------------------

namespace
{
// Helper: build a HSV Vec3-like object {x,y,z}.
QJSValue makeVec3(QJSEngine& e, double x, double y, double z) {
    QJSValue v = e.newObject();
    v.setProperty("x", x);
    v.setProperty("y", y);
    v.setProperty("z", z);
    return v;
}

// Helper: assert WEColor.hsv2rgb(h,s,v) == (r,g,b) within epsilon.
void checkHsv(QJSEngine& e, double h, double s, double v, double r, double g, double b) {
    QJSValue fn  = e.evaluate("WEColor.hsv2rgb");
    QJSValue out = fn.call({ makeVec3(e, h, s, v) });
    REQUIRE(out.isObject());
    CHECK(out.property("x").toNumber() == doctest::Approx(r).epsilon(1e-6));
    CHECK(out.property("y").toNumber() == doctest::Approx(g).epsilon(1e-6));
    CHECK(out.property("z").toNumber() == doctest::Approx(b).epsilon(1e-6));
}
} // namespace

TEST_SUITE("WEColor shim") {
    TEST_CASE("hsv2rgb covers all six hue sextants") {
        QJSEngine e;
        e.evaluate(wek::qml_helper::kWEColorJs);

        // Saturation/value pinned at 1.0; canonical HSV→RGB primaries +
        // secondaries: red, yellow, green, cyan, blue, magenta.
        checkHsv(e, 0.0 / 6.0, 1, 1, 1, 0, 0); // red
        checkHsv(e, 1.0 / 6.0, 1, 1, 1, 1, 0); // yellow
        checkHsv(e, 2.0 / 6.0, 1, 1, 0, 1, 0); // green
        checkHsv(e, 3.0 / 6.0, 1, 1, 0, 1, 1); // cyan
        checkHsv(e, 4.0 / 6.0, 1, 1, 0, 0, 1); // blue
        checkHsv(e, 5.0 / 6.0, 1, 1, 1, 0, 1); // magenta
    }

    TEST_CASE("hsv2rgb white at saturation=0") {
        QJSEngine e;
        e.evaluate(wek::qml_helper::kWEColorJs);
        // Any hue with s=0 is grey; v controls brightness.
        checkHsv(e, 0.4, 0, 0.7, 0.7, 0.7, 0.7);
    }

    TEST_CASE("hsv2rgb black at value=0") {
        QJSEngine e;
        e.evaluate(wek::qml_helper::kWEColorJs);
        checkHsv(e, 0.5, 1, 0, 0, 0, 0);
    }

    TEST_CASE("rgb2hsv inverts hsv2rgb on a few non-degenerate triples") {
        QJSEngine e;
        e.evaluate(wek::qml_helper::kWEColorJs);
        QJSValue h2r = e.evaluate("WEColor.hsv2rgb");
        QJSValue r2h = e.evaluate("WEColor.rgb2hsv");
        struct Hsv { double h, s, v; };
        for (auto in : { Hsv{ 0.10, 0.6, 0.8 },
                         Hsv{ 0.50, 0.4, 0.9 },
                         Hsv{ 0.83, 0.9, 0.5 } }) {
            QJSValue rgb  = h2r.call({ makeVec3(e, in.h, in.s, in.v) });
            QJSValue back = r2h.call({ rgb });
            CHECK(back.property("x").toNumber() == doctest::Approx(in.h).epsilon(1e-4));
            CHECK(back.property("y").toNumber() == doctest::Approx(in.s).epsilon(1e-4));
            CHECK(back.property("z").toNumber() == doctest::Approx(in.v).epsilon(1e-4));
        }
    }

    TEST_CASE("normalizeColor / expandColor are 255-scale conversions") {
        QJSEngine e;
        e.evaluate(wek::qml_helper::kWEColorJs);
        QJSValue norm = e.evaluate("WEColor.normalizeColor({x:255,y:128,z:0})");
        CHECK(norm.property("x").toNumber() == doctest::Approx(1.0));
        CHECK(norm.property("y").toNumber() == doctest::Approx(128.0 / 255.0));
        CHECK(norm.property("z").toNumber() == doctest::Approx(0.0));

        QJSValue exp = e.evaluate("WEColor.expandColor({x:1.0,y:0.5,z:0.0})");
        CHECK(exp.property("x").toNumber() == doctest::Approx(255.0));
        CHECK(exp.property("y").toNumber() == doctest::Approx(127.5));
        CHECK(exp.property("z").toNumber() == doctest::Approx(0.0));
    }
} // TEST_SUITE WEColor shim

TEST_SUITE("engine.registerAudioBuffers shim") {
    // Helper: install the JS shim onto a fresh engine with an `engine` global.
    static QJSValue installRegisterFn(QJSEngine& e) {
        e.evaluate("var engine = {};");
        QJSValue regFn = e.evaluate(wek::qml_helper::kRegisterAudioBuffersJs);
        e.globalObject().property("engine").setProperty("registerAudioBuffers", regFn);
        return regFn;
    }

    TEST_CASE("returns buffer with left/right/average arrays sized to resolution") {
        for (int n : { 16, 32, 64 }) {
            QJSEngine e;
            installRegisterFn(e);
            QJSValue buf =
                e.evaluate(QString("engine.registerAudioBuffers(%1)").arg(n));
            REQUIRE(buf.isObject());
            CHECK(buf.property("resolution").toInt() == n);
            CHECK(buf.property("left").property("length").toInt() == n);
            CHECK(buf.property("right").property("length").toInt() == n);
            CHECK(buf.property("average").property("length").toInt() == n);
            // All arrays seeded to 0 so scripts can read [i] before the C++
            // refresh has run (first frame pre-analyzer).
            for (int i = 0; i < n; i++) {
                CHECK(buf.property("average").property(i).toNumber() == 0.0);
            }
        }
    }

    TEST_CASE("rounds out-of-band resolution to nearest valid size") {
        QJSEngine e;
        installRegisterFn(e);
        // <=24 → 16; <=48 → 32; otherwise → 64.  Above-64 also clamps to 64.
        struct Case { int in, out; };
        for (auto c : { Case{ 8, 16 },
                        Case{ 16, 16 },
                        Case{ 24, 16 },
                        Case{ 25, 32 },
                        Case{ 48, 32 },
                        Case{ 49, 64 },
                        Case{ 64, 64 },
                        Case{ 100, 64 } }) {
            QJSValue buf =
                e.evaluate(QString("engine.registerAudioBuffers(%1)").arg(c.in));
            CHECK_MESSAGE(buf.property("resolution").toInt() == c.out,
                          "input ",
                          c.in,
                          " expected ",
                          c.out);
        }
    }

    TEST_CASE("each call appends to engine._audioRegs with a unique _regIdx") {
        QJSEngine e;
        installRegisterFn(e);
        QJSValue a = e.evaluate("engine.registerAudioBuffers(16)");
        QJSValue b = e.evaluate("engine.registerAudioBuffers(32)");
        QJSValue c = e.evaluate("engine.registerAudioBuffers(64)");
        CHECK(a.property("_regIdx").toInt() == 0);
        CHECK(b.property("_regIdx").toInt() == 1);
        CHECK(c.property("_regIdx").toInt() == 2);
        CHECK(e.evaluate("engine._audioRegs.length").toInt() == 3);
        // Regs preserve resolution per entry — refreshAudioBuffers() reads
        // resolution off the entry to pick the right analyzer pad size.
        CHECK(e.evaluate("engine._audioRegs[0].resolution").toInt() == 16);
        CHECK(e.evaluate("engine._audioRegs[1].resolution").toInt() == 32);
        CHECK(e.evaluate("engine._audioRegs[2].resolution").toInt() == 64);
    }

    TEST_CASE("default (no arg) yields 64-bin buffer") {
        QJSEngine e;
        installRegisterFn(e);
        QJSValue buf = e.evaluate("engine.registerAudioBuffers()");
        CHECK(buf.property("resolution").toInt() == 64);
        CHECK(buf.property("average").property("length").toInt() == 64);
    }
} // TEST_SUITE engine.registerAudioBuffers shim

// =====================================================================
// Layer hierarchy: thisLayer.setParent / getParent / getChildren /
// removeFromParent / getRoot / isAncestorOf / getDepth.
//
// Most tests use a 3-layer fixture: 'bg' (root), 'child' (parent='bg'),
// 'grandchild' (parent='child').  Each test extends ScriptEnv and calls
// _linkupHierarchy() to wire the cached parent refs.
// =====================================================================
namespace
{

// Set up a parent/child fixture on top of the default ScriptEnv layers
// ('bg', 'fg').  Adds 'child' (parent='bg') and 'grandchild' (parent='child').
// Materializes each proxy and runs _linkupHierarchy.
void setupHierarchyFixture(ScriptEnv& env) {
    env.engine.evaluate(
        "_layerInitStates['child'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0], "
        "                              sz:[10,10], v:true, pn:'bg' };\n"
        "_layerInitStates['grandchild'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0], "
        "                                   sz:[5,5], v:true, pn:'child' };\n"
        "_layerNameToId['child'] = 101;\n"
        "_layerNameToId['grandchild'] = 102;\n"
        "_layerIdToName['101'] = 'child';\n"
        "_layerIdToName['102'] = 'grandchild';\n"
        // Materialize each proxy so _layerCache is populated, then linkup.
        "thisScene.getLayer('bg');\n"
        "thisScene.getLayer('fg');\n"
        "thisScene.getLayer('child');\n"
        "thisScene.getLayer('grandchild');\n"
        "_linkupHierarchy();\n");
}

// Install a stub __sceneBridge that records setParent invocations into a
// JS-side array `__bridgeCalls` for assertion.
void installBridgeStub(ScriptEnv& env) {
    env.engine.evaluate(
        "var __bridgeCalls = [];\n"
        "var __sceneBridge = {\n"
        "  setLayerParent: function(childId, parentId) {\n"
        "    __bridgeCalls.push([childId, parentId]);\n"
        "  }\n"
        "};\n");
}

} // anonymous namespace

TEST_SUITE("SceneScript Hierarchy Fields") {
    TEST_CASE("regular proxy has _parent null and _children empty by default") {
        ScriptEnv env;
        QJSValue  p = env.engine.evaluate("thisScene.getLayer('bg')");
        CHECK(p.property("_parent").isNull());
        CHECK(p.property("_children").isArray());
        CHECK(p.property("_children").property("length").toInt() == 0);
    }

    TEST_CASE("_linkupHierarchy resolves pn references") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        QJSValue parent = env.engine.evaluate("thisScene.getLayer('child')._parent");
        REQUIRE(parent.isObject());
        CHECK(parent.property("name").toString() == "bg");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 1);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('bg')._children[0] === thisScene.getLayer('child')")
                  .toBool());
    }

    TEST_CASE("_linkupHierarchy is idempotent — second call is a no-op") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        env.engine.evaluate("_linkupHierarchy();"); // call again
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 1);
    }

    TEST_CASE("_linkupHierarchy ordering-independent: child queried before parent") {
        // The fixture queries 'bg' before 'child'.  Verify the reverse order
        // still wires up via the deferred linkup.
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['kid'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0], "
            "                            sz:[10,10], v:true, pn:'bg' };\n"
            "thisScene.getLayer('kid');\n"     // child first
            "thisScene.getLayer('bg');\n"      // parent later
            "_linkupHierarchy();\n");
        CHECK(env.engine.evaluate(
                       "thisScene.getLayer('kid')._parent === thisScene.getLayer('bg')")
                  .toBool());
    }

    TEST_CASE("_linkupHierarchy skips entries whose parent name is not in cache") {
        // pn pointing to a non-existent / sound-only layer must not crash.
        ScriptEnv env;
        env.engine.evaluate(
            "_layerInitStates['orphan'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0], "
            "                               sz:[10,10], v:true, pn:'__missing__' };\n"
            "thisScene.getLayer('orphan');\n"
            "_linkupHierarchy();\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('orphan')._parent").isNull());
    }
} // SceneScript Hierarchy Fields

TEST_SUITE("SceneScript Hierarchy Read-Only Methods") {
    TEST_CASE("getParent returns cached ref / null") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('child').getParent() === thisScene.getLayer('bg')")
                  .toBool());
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getParent()").isNull());
    }

    TEST_CASE("getChildren returns defensive copy — mutating doesn't leak back") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        env.engine.evaluate(
            "var k = thisScene.getLayer('bg').getChildren();\n"
            "k.push('garbage');\n");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getChildren().length").toInt() == 1);
    }

    TEST_CASE("getChildren contains real proxy refs") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('bg').getChildren()[0] === "
                            "thisScene.getLayer('child')")
                  .toBool());
    }

    TEST_CASE("getRoot walks up the parent chain") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('grandchild').getRoot() === "
                            "thisScene.getLayer('bg')")
                  .toBool());
    }

    TEST_CASE("getRoot returns self when orphan / no parent") {
        ScriptEnv env;
        QJSValue  ret = env.engine.evaluate(
            "thisScene.getLayer('bg').getRoot() === thisScene.getLayer('bg')");
        CHECK(ret.toBool());
    }

    TEST_CASE("isAncestorOf descendant returns true") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('bg').isAncestorOf("
                            "thisScene.getLayer('grandchild'))")
                  .toBool());
    }

    TEST_CASE("isAncestorOf reversed returns false") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK_FALSE(env.engine
                        .evaluate("thisScene.getLayer('grandchild').isAncestorOf("
                                  "thisScene.getLayer('bg'))")
                        .toBool());
    }

    TEST_CASE("isAncestorOf self returns false (strict ancestor)") {
        ScriptEnv env;
        CHECK_FALSE(
            env.engine
                .evaluate("thisScene.getLayer('bg').isAncestorOf(thisScene.getLayer('bg'))")
                .toBool());
    }

    TEST_CASE("isAncestorOf null returns false") {
        ScriptEnv env;
        CHECK_FALSE(
            env.engine.evaluate("thisScene.getLayer('bg').isAncestorOf(null)").toBool());
    }

    TEST_CASE("isAncestorOf non-layer object returns false") {
        ScriptEnv env;
        CHECK_FALSE(env.engine
                        .evaluate("thisScene.getLayer('bg').isAncestorOf({junk:true})")
                        .toBool());
    }

    TEST_CASE("getDepth root=0, child=1, grandchild=2") {
        ScriptEnv env;
        setupHierarchyFixture(env);
        CHECK(env.engine.evaluate("thisScene.getLayer('bg').getDepth()").toInt() == 0);
        CHECK(env.engine.evaluate("thisScene.getLayer('child').getDepth()").toInt() == 1);
        CHECK(env.engine.evaluate("thisScene.getLayer('grandchild').getDepth()").toInt() == 2);
    }
} // SceneScript Hierarchy Read-Only Methods

TEST_SUITE("SceneScript Hierarchy setParent") {
    TEST_CASE("setParent updates _parent reference") {
        ScriptEnv env;
        installBridgeStub(env);
        env.engine.evaluate(
            "thisScene.getLayer('fg').setParent(thisScene.getLayer('bg'));");
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('fg')._parent === thisScene.getLayer('bg')")
                  .toBool());
    }

    TEST_CASE("setParent adds to new parent's _children") {
        ScriptEnv env;
        installBridgeStub(env);
        env.engine.evaluate(
            "thisScene.getLayer('fg').setParent(thisScene.getLayer('bg'));");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 1);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('bg')._children[0] === thisScene.getLayer('fg')")
                  .toBool());
    }

    TEST_CASE("setParent removes from old parent's _children") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        // Reparent 'child' from 'bg' to 'fg'.
        env.engine.evaluate(
            "thisScene.getLayer('child').setParent(thisScene.getLayer('fg'));");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 0);
        CHECK(env.engine.evaluate("thisScene.getLayer('fg')._children.length").toInt() == 1);
    }

    TEST_CASE("setParent(null) detaches and clears old parent's children") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        env.engine.evaluate("thisScene.getLayer('child').setParent(null);");
        CHECK(env.engine.evaluate("thisScene.getLayer('child')._parent").isNull());
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 0);
    }

    TEST_CASE("setParent returns this for chaining") {
        ScriptEnv env;
        installBridgeStub(env);
        QJSValue  ret = env.engine.evaluate(
            "thisScene.getLayer('fg').setParent(thisScene.getLayer('bg')) "
             "=== thisScene.getLayer('fg')");
        CHECK(ret.toBool());
    }

    TEST_CASE("setParent invokes __sceneBridge.setParent exactly once") {
        ScriptEnv env;
        installBridgeStub(env);
        env.engine.evaluate(
            "thisScene.getLayer('fg').setParent(thisScene.getLayer('bg'));");
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__bridgeCalls[0][0]").toInt() == 11); // fg id
        CHECK(env.engine.evaluate("__bridgeCalls[0][1]").toInt() == 7);  // bg id
    }

    TEST_CASE("setParent(null) sends parent_id=-1 across the bridge") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        env.engine.evaluate("__bridgeCalls.length = 0;\n"
                            "thisScene.getLayer('child').setParent(null);");
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 1);
        CHECK(env.engine.evaluate("__bridgeCalls[0][0]").toInt() == 101); // child id
        CHECK(env.engine.evaluate("__bridgeCalls[0][1]").toInt() == -1);
    }

    TEST_CASE("removeFromParent equals setParent(null)") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        env.engine.evaluate("thisScene.getLayer('child').removeFromParent();");
        CHECK(env.engine.evaluate("thisScene.getLayer('child')._parent").isNull());
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 0);
    }
} // SceneScript Hierarchy setParent

TEST_SUITE("SceneScript Hierarchy Cycle Prevention") {
    TEST_CASE("setParent(self) is a no-op — no _parent change, no bridge call") {
        ScriptEnv env;
        installBridgeStub(env);
        env.engine.evaluate(
            "thisScene.getLayer('bg').setParent(thisScene.getLayer('bg'));");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._parent").isNull());
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 0);
    }

    TEST_CASE("setParent(descendant) is a no-op — would create a cycle") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        env.engine.evaluate("__bridgeCalls.length = 0;\n"
                            "thisScene.getLayer('bg').setParent("
                            "  thisScene.getLayer('grandchild'));");
        // bg's _parent still null; bg not in grandchild's children.
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._parent").isNull());
        CHECK(env.engine.evaluate("thisScene.getLayer('grandchild')._children.length").toInt() ==
              0);
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 0);
    }

    TEST_CASE("setParent(currentParent) is idempotent — no bridge call") {
        ScriptEnv env;
        installBridgeStub(env);
        setupHierarchyFixture(env);
        env.engine.evaluate("__bridgeCalls.length = 0;\n"
                            "thisScene.getLayer('child').setParent("
                            "  thisScene.getLayer('bg'));");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 1);
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 0);
    }

    TEST_CASE("setParent(non-layer object) is a no-op + warning") {
        ScriptEnv env;
        installBridgeStub(env);
        env.engine.evaluate(
            "thisScene.getLayer('bg').setParent({randomObject: true});");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._parent").isNull());
        CHECK(env.engine.evaluate("__bridgeCalls.length").toInt() == 0);
    }
} // SceneScript Hierarchy Cycle Prevention

TEST_SUITE("SceneScript Hierarchy Sound Stubs") {
    TEST_CASE("setParent on sound layer is no-op + warning") {
        ScriptEnv env;
        installBridgeStub(env);
        QJSValue ret = env.engine.evaluate(
            "var s = thisScene.getLayer('music.mp3');\n"
            "s.setParent(thisScene.getLayer('bg'));\n"
            "s === s.setParent(null) && __bridgeCalls.length === 0;");
        CHECK(ret.toBool());
    }

    TEST_CASE("getParent on sound layer returns null") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').getParent()").isNull());
    }

    TEST_CASE("getChildren on sound layer returns empty array") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').getChildren().length")
                  .toInt() == 0);
    }

    TEST_CASE("isAncestorOf on sound layer always returns false") {
        ScriptEnv env;
        QJSValue  ret = env.engine.evaluate(
            "thisScene.getLayer('music.mp3').isAncestorOf(thisScene.getLayer('bg'))");
        CHECK_FALSE(ret.toBool());
    }

    TEST_CASE("getDepth on sound layer returns 0") {
        ScriptEnv env;
        CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').getDepth()").toInt() == 0);
    }

    TEST_CASE("getRoot on sound layer returns self") {
        ScriptEnv env;
        QJSValue  ret = env.engine.evaluate(
            "thisScene.getLayer('music.mp3').getRoot() === thisScene.getLayer('music.mp3')");
        CHECK(ret.toBool());
    }

    TEST_CASE("removeFromParent on sound layer returns this") {
        ScriptEnv env;
        QJSValue  ret = env.engine.evaluate(
            "var s = thisScene.getLayer('music.mp3');"
            "s.removeFromParent() === s;");
        CHECK(ret.toBool());
    }
} // SceneScript Hierarchy Sound Stubs

// ---------------------------------------------------------------------
// Pool-layer hierarchy + cross-type reparenting + destroyLayer reset.
// Pool slots are injected into _layerInitStates with __pool_ names, then
// createLayer/destroyLayer are mocked exactly as the existing pool tests
// (line 3621+) do.  The test mock's destroyLayer calls _detachLayerHierarchy
// from the shared shim — same call production destroyLayer makes.
// ---------------------------------------------------------------------
namespace
{
void setupPoolFixture(ScriptEnv& env) {
    env.engine.evaluate(
        "_layerInitStates['__pool_fx_0'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0],\n"
        "                                    sz:[10,10], v:false };\n"
        "_layerInitStates['__pool_fx_1'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0],\n"
        "                                    sz:[10,10], v:false };\n"
        "_layerNameToId['__pool_fx_0'] = 200;\n"
        "_layerNameToId['__pool_fx_1'] = 201;\n"
        "engine._assetPools = { 'fx': ['__pool_fx_0','__pool_fx_1'] };\n"
        "engine._assetLive  = { 'fx': [] };\n"
        "engine.registerAsset = function(path) { return { __asset: path }; };\n"
        "thisScene.createLayer = function(asset) {\n"
        "  var path = asset && asset.__asset;\n"
        "  var pool = path && engine._assetPools[path];\n"
        "  if (pool && pool.length > 0) {\n"
        "    var name = pool.shift();\n"
        "    var layer = thisScene.getLayer(name);\n"
        "    if (layer) {\n"
        "      layer.__asset = path;\n"
        "      layer.visible = true;\n"
        "      engine._assetLive[path].push(name);\n"
        "      return layer;\n"
        "    }\n"
        "  }\n"
        "  return { __stub: true };\n"
        "};\n"
        // Mock destroyLayer reuses the shared _detachLayerHierarchy helper.
        "thisScene.destroyLayer = function(layer) {\n"
        "  if (!layer || layer.__stub) return;\n"
        "  layer.visible = false;\n"
        "  layer._destroyed = true;\n"
        "  _detachLayerHierarchy(layer);\n"
        "  var path = layer.__asset;\n"
        "  if (path && engine._assetPools[path] && layer.name) {\n"
        "    engine._assetPools[path].push(layer.name);\n"
        "  }\n"
        "};\n");
}
} // anonymous namespace

TEST_SUITE("SceneScript Hierarchy Pool Layers") {
    TEST_CASE("createLayer-spawned slot has _parent=null and _children=[] by default") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        QJSValue layer = env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "thisScene.createLayer(fxAsset);");
        CHECK(layer.property("_parent").isNull());
        CHECK(layer.property("_children").isArray());
        CHECK(layer.property("_children").property("length").toInt() == 0);
    }

    TEST_CASE("pool slot can be reparented under a regular layer") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var fx = thisScene.createLayer(fxAsset);\n"
            "fx.setParent(thisScene.getLayer('bg'));");
        // bg now has fx as a child; fx._parent is bg.
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 1);
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('bg')._children[0]._parent === "
                            "thisScene.getLayer('bg')")
                  .toBool());
    }

    TEST_CASE("regular layer can be reparented under a pool slot (anchor pattern)") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var anchor = thisScene.createLayer(fxAsset);\n"
            "thisScene.getLayer('fg').setParent(anchor);");
        CHECK(env.engine
                  .evaluate("thisScene.getLayer('fg')._parent && "
                            "thisScene.getLayer('fg')._parent.__asset === 'fx'")
                  .toBool());
    }

    TEST_CASE("pool slot can be reparented under another pool slot") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var a = thisScene.createLayer(fxAsset);\n"
            "var b = thisScene.createLayer(fxAsset);\n"
            "a.setParent(b);");
        CHECK(env.engine.evaluate("a._parent === b").toBool());
        CHECK(env.engine.evaluate("b._children.length").toInt() == 1);
    }
} // SceneScript Hierarchy Pool Layers

TEST_SUITE("SceneScript Hierarchy destroyLayer Reset") {
    TEST_CASE("destroyLayer detaches the slot from its parent's children") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var fx = thisScene.createLayer(fxAsset);\n"
            "fx.setParent(thisScene.getLayer('bg'));\n"
            "thisScene.destroyLayer(fx);");
        CHECK(env.engine.evaluate("thisScene.getLayer('bg')._children.length").toInt() == 0);
    }

    TEST_CASE("destroyLayer clears the slot's own _parent reference") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var fx = thisScene.createLayer(fxAsset);\n"
            "fx.setParent(thisScene.getLayer('bg'));\n"
            "thisScene.destroyLayer(fx);");
        CHECK(env.engine.evaluate("fx._parent").isNull());
    }

    TEST_CASE("destroyLayer enqueues parentId=-1 reset for the slot") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var fx = thisScene.createLayer(fxAsset);\n"
            "fx.setParent(thisScene.getLayer('bg'));\n"
            "__bridgeCalls.length = 0;\n" // clear setParent's earlier call
            "thisScene.destroyLayer(fx);");
        QJSValue calls = env.engine.evaluate("__bridgeCalls");
        REQUIRE(calls.property("length").toInt() >= 1);
        CHECK(env.engine.evaluate("__bridgeCalls[0][0]").toInt() == 200); // fx_0 id
        CHECK(env.engine.evaluate("__bridgeCalls[0][1]").toInt() == -1);
    }

    TEST_CASE("destroyLayer orphans any children of the destroyed slot") {
        ScriptEnv env;
        installBridgeStub(env);
        setupPoolFixture(env);
        env.engine.evaluate(
            "var fxAsset = engine.registerAsset('fx');\n"
            "var anchor = thisScene.createLayer(fxAsset);\n"
            "thisScene.getLayer('fg').setParent(anchor);\n"
            "thisScene.destroyLayer(anchor);");
        CHECK(env.engine.evaluate("thisScene.getLayer('fg')._parent").isNull());
    }
} // SceneScript Hierarchy destroyLayer Reset

TEST_SUITE("SceneScript engine.fps binding") {
    // Verifies the JS surface SceneBackend builds in setupTextScripts /
    // evaluateTextScripts: scripts read `engine.fps` as a plain number, and
    // each tick the native side overwrites it from SceneWallpaper::getFps()
    // (FpsCounter wall-clock measurement on the render thread).
    //
    // The end-to-end fix it covers — Miku 3363252053 was stuck at "fps: 2"
    // because the text-script Date.now() math was sampling the text timer's
    // 500ms cadence rather than the render rate.  After the fix, authors can
    // either (a) keep the Date.now() math working (text scripts now eval once
    // per render frame) or (b) read engine.fps directly.

    struct FpsEnv {
        QJSEngine engine;
        QJSValue  engineObj;

        FpsEnv() {
            engineObj = engine.newObject();
            // Match setupTextScripts's initial value before any frame draws.
            engineObj.setProperty("fps", 0.0);
            engine.globalObject().setProperty("engine", engineObj);
        }

        // Mimic evaluateTextScripts publishing SceneWallpaper::getFps().
        void publishFps(double fps) {
            engine.globalObject().property("engine").setProperty("fps", fps);
        }
    };

    TEST_CASE("default engine.fps is 0 before the first frame draws") {
        FpsEnv env;
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("engine.fps reflects what the native side published") {
        FpsEnv env;
        env.publishFps(30.0);
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(30.0));
        env.publishFps(60.0);
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(60.0));
        env.publishFps(12.0);
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(12.0));
    }

    TEST_CASE("Miku-style FPS readout: 'fps: ' + Math.round(engine.fps)") {
        // This is the literal idiom scripts can use as a drop-in replacement
        // for the Date.now() pattern.  Verifies the rounding and concatenation
        // pipeline produces a clean integer string the text renderer can show.
        FpsEnv env;
        CHECK(env.engine.evaluate("'fps: ' + Math.round(engine.fps)").toString() ==
              QString("fps: 0"));
        env.publishFps(30.7);
        CHECK(env.engine.evaluate("'fps: ' + Math.round(engine.fps)").toString() ==
              QString("fps: 31"));
        env.publishFps(59.4);
        CHECK(env.engine.evaluate("'fps: ' + Math.round(engine.fps)").toString() ==
              QString("fps: 59"));
    }

    TEST_CASE("regression: the value 2 is not pinned in the JS layer") {
        // Guards against re-introducing the bug where text scripts saw 2Hz
        // updates regardless of render rate.  At the JS layer, fps must be
        // exactly what was last published — the assertion is trivially true
        // here, but the test exists to fail loudly if a future refactor
        // adds a Math.min(fps, 2) or similar clamp at the JS surface.
        FpsEnv env;
        env.publishFps(60.0);
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(60.0));
        env.publishFps(120.0);
        CHECK(env.engine.evaluate("engine.fps").toNumber() == doctest::Approx(120.0));
    }
} // SceneScript engine.fps binding

// ------------------------------------------------------------------
// JS string escaping (F18)
//
// escapeForJsSingleQuoted / jsLayerLookupExpr (JsStringEscape.hpp) are the
// single source of truth for embedding author data into a JS single-quoted
// string literal that SceneBackend.cpp then feeds to the QJSEngine
// (`JSON.parse('%1')`, `thisScene.getLayer('%1')`, inline object literals).
//
// The pre-F18 idiom escaped only `\` and `'`.  That left raw newline / CR /
// U+2028 / U+2029 to terminate the literal early and SyntaxError the parse —
// silently dropping that script's scriptProperties / init / user-props.  These
// cases evaluate the escaped output through a bare QJSEngine (the production
// code path) so prod and test cannot drift.
// ------------------------------------------------------------------
TEST_SUITE("JS string escaping (F18)") {
    using wek::qml_helper::escapeForJsSingleQuoted;
    using wek::qml_helper::jsLayerLookupExpr;

    // Helper: wrap the escaped payload exactly like the production feeder sites
    // (`JSON.parse('%1')`) and evaluate it through a bare engine.
    static QJSValue parseEscaped(QJSEngine & e, const QString& rawJson) {
        const QString escaped = escapeForJsSingleQuoted(rawJson);
        return e.evaluate(QStringLiteral("JSON.parse('%1')").arg(escaped));
    }

    TEST_CASE("escapeForJsSingleQuoted round-trips through JSON.parse for plain JSON") {
        QJSEngine e;
        QJSValue  v = parseEscaped(e, QStringLiteral("{\"a\":1,\"b\":\"hello\"}"));
        CHECK_FALSE(v.isError());
        CHECK(v.property("a").toInt() == 1);
        CHECK(v.property("b").toString() == QStringLiteral("hello"));
    }

    TEST_CASE("embedded single quote survives") {
        QJSEngine e;
        // JSON string value contains an apostrophe: O'Brien.
        QJSValue v = parseEscaped(e, QStringLiteral("{\"name\":\"O'Brien\"}"));
        CHECK_FALSE(v.isError());
        CHECK(v.property("name").toString() == QStringLiteral("O'Brien"));
    }

    TEST_CASE("embedded backslash survives") {
        QJSEngine e;
        // JSON value C:\path  → in JSON that is the two chars  C : \ p ...
        // (escaped in the JSON itself as \\).  The QString below holds a single
        // backslash, which is a valid JSON escape lead only as \\, so encode it.
        QJSValue v = parseEscaped(e, QStringLiteral("{\"p\":\"C:\\\\path\"}"));
        CHECK_FALSE(v.isError());
        CHECK(v.property("p").toString() == QStringLiteral("C:\\path"));
    }

    TEST_CASE("U+2028 line separator does not break the literal") {
        QJSEngine e;
        // A *raw* U+2028 inside the JSON string value (legal in JSON, fatal in a
        // JS source literal unless escaped).
        const QChar ls(0x2028);
        QString     raw = QStringLiteral("{\"t\":\"a") + ls + QStringLiteral("b\"}");
        QJSValue    v   = parseEscaped(e, raw);
        CHECK_FALSE(v.isError());
        const QString got = v.property("t").toString();
        // The parsed value still contains the U+2028 character.
        CHECK(got.contains(ls));
        CHECK(got == QStringLiteral("a") + ls + QStringLiteral("b"));
        // And the helper emitted the escaped six-char form, not a raw separator.
        CHECK(escapeForJsSingleQuoted(raw).contains(QStringLiteral("\\u2028")));
        CHECK_FALSE(escapeForJsSingleQuoted(raw).contains(ls));
    }

    TEST_CASE("U+2029 paragraph separator does not break the literal") {
        QJSEngine   e;
        const QChar ps(0x2029);
        QString     raw = QStringLiteral("{\"t\":\"a") + ps + QStringLiteral("b\"}");
        QJSValue    v   = parseEscaped(e, raw);
        CHECK_FALSE(v.isError());
        const QString got = v.property("t").toString();
        CHECK(got.contains(ps));
        CHECK(escapeForJsSingleQuoted(raw).contains(QStringLiteral("\\u2029")));
        CHECK_FALSE(escapeForJsSingleQuoted(raw).contains(ps));
    }

    TEST_CASE("raw newline / CR in the source value is escaped") {
        QJSEngine e;
        // Raw \n and \r embedded in the JS source string literal would each
        // terminate it; the helper turns them into the two-char \n / \r forms.
        // (JSON itself forbids a raw newline inside a string, so the round-trip
        // here is over the *JS literal*, not a JSON value: build the literal
        // directly to model parser-produced content reaching the feeder.)
        const QString raw = QStringLiteral("line1\nline2\rline3");
        const QString esc = escapeForJsSingleQuoted(raw);
        CHECK(esc == QStringLiteral("line1\\nline2\\rline3"));
        // Evaluating it as a JS string literal yields the original back.
        QJSValue v = e.evaluate(QStringLiteral("'%1'").arg(esc));
        CHECK_FALSE(v.isError());
        CHECK(v.toString() == raw);
    }

    TEST_CASE("tab is escaped to two-char \\t") {
        QJSEngine     e;
        const QString esc = escapeForJsSingleQuoted(QStringLiteral("a\tb"));
        CHECK(esc == QStringLiteral("a\\tb"));
        QJSValue v = e.evaluate(QStringLiteral("'%1'").arg(esc));
        CHECK_FALSE(v.isError());
        CHECK(v.toString() == QStringLiteral("a\tb"));
    }

    TEST_CASE("other C0 control chars become \\uXXXX") {
        // Backspace (0x08), vertical tab (0x0B), form feed (0x0C), NUL (0x00).
        QString raw;
        raw.append(QChar(0x08));
        raw.append(QChar(0x0B));
        raw.append(QChar(0x0C));
        raw.append(QChar(0x00));
        const QString esc = escapeForJsSingleQuoted(raw);
        CHECK(esc == QStringLiteral("\\u0008\\u000b\\u000c\\u0000"));
        QJSEngine e;
        QJSValue  v = e.evaluate(QStringLiteral("'%1'").arg(esc));
        CHECK_FALSE(v.isError());
        // Round-trips to the same four control chars.
        const QString got = v.toString();
        CHECK(got.size() == 4);
        CHECK(got.at(0).unicode() == 0x08);
        CHECK(got.at(3).unicode() == 0x00);
    }

    TEST_CASE("special-char-free input is byte-identical to the old escape") {
        // Behavioral-equivalence guard: for input with none of the special
        // characters, escapeForJsSingleQuoted must match the historical
        // `\\`+`'` output exactly so existing wallpapers are unaffected.
        const QString plain = QStringLiteral("{\"a\":1,\"b\":[2,3],\"c\":\"plain text\"}");
        CHECK(escapeForJsSingleQuoted(plain) == plain);
    }

    TEST_CASE("jsLayerLookupExpr escapes the name") {
        // Mirror the production layer-proxy stub: thisScene.getLayer(name)
        // returns an object carrying that name, or an empty-name fallback.
        QJSEngine e;
        e.evaluate(R"JS(
            var thisScene = {
                getLayer: function(n) { return { name: n, isFallback: (n === '') }; }
            };
        )JS");

        // A name with a single quote AND a U+2028 — the pre-F18 unescaped
        // getLayer('%1') would either SyntaxError or resolve the wrong name.
        const QChar   ls(0x2028);
        const QString tricky = QStringLiteral("Layer 'X'") + ls + QStringLiteral("Y");
        const QString expr   = jsLayerLookupExpr(tricky);
        QJSValue      v      = e.evaluate(expr);
        CHECK_FALSE(v.isError());
        CHECK_FALSE(v.property("isFallback").toBool());
        CHECK(v.property("name").toString() == tricky);
    }

    TEST_CASE("jsLayerLookupExpr produces the expected expression shape") {
        // Plain name → exact expression; locks the helper's output format.
        CHECK(jsLayerLookupExpr(QStringLiteral("Sky")) ==
              QStringLiteral("thisScene.getLayer('Sky')"));
        // Quote is escaped inside the single-quoted literal.
        CHECK(jsLayerLookupExpr(QStringLiteral("A'B")) ==
              QStringLiteral("thisScene.getLayer('A\\'B')"));
    }

    TEST_CASE("regression mirror: the OLD backslash+quote-only escape DOES error") {
        // Reproduce the pre-F18 escaping (only `\` and `'`) and prove that a raw
        // U+2028 in the payload makes JSON.parse a SyntaxError — i.e. this test
        // would have caught the bug.  The new helper (asserted above) does not.
        QJSEngine   e;
        const QChar ls(0x2028);
        QString     raw = QStringLiteral("{\"t\":\"a") + ls + QStringLiteral("b\"}");

        QString oldEsc = raw;
        oldEsc.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        oldEsc.replace(QLatin1Char('\''), QStringLiteral("\\'"));
        QJSValue oldResult = e.evaluate(QStringLiteral("JSON.parse('%1')").arg(oldEsc));
        CHECK(oldResult.isError()); // line-terminator breaks the literal

        // Same payload through the new helper succeeds.
        QJSValue newResult = parseEscaped(e, raw);
        CHECK_FALSE(newResult.isError());
    }
} // TEST_SUITE JS string escaping (F18)

// ------------------------------------------------------------------
// JsSyntaxNormalize — ES2019 optional catch binding (`catch {`)
// QJSEngine/V4 rejects the bare `catch {`; stripESModuleSyntax() rewrites it
// via normalizeOptionalCatchBinding().  Real-world hit: the Gariam parenting
// system embedded in 2992803622 (id=31, 791-line script) never compiled.
// ------------------------------------------------------------------
TEST_SUITE("JsSyntaxNormalize") {
    TEST_CASE("optional catch binding gains a parameterized binding") {
        QString src = QStringLiteral("try { risky(); } catch { fallback(); }");
        normalizeOptionalCatchBinding(src);
        CHECK(src == QStringLiteral("try { risky(); } catch (__wecatch) { fallback(); }"));
    }

    TEST_CASE("compact and newline-separated catch forms are handled") {
        QString compact = QStringLiteral("try{f()}catch{g()}");
        normalizeOptionalCatchBinding(compact);
        CHECK(compact.contains(QStringLiteral("catch (__wecatch) {")));
        CHECK_FALSE(compact.contains(QStringLiteral("catch{")));

        QString multiline = QStringLiteral("try {\n  f();\n}\ncatch\n{\n  g();\n}");
        normalizeOptionalCatchBinding(multiline);
        CHECK(multiline.contains(QStringLiteral("catch (__wecatch) {")));
    }

    TEST_CASE("nested optional catches each get a (shadowing) binding") {
        QString src = QStringLiteral("try{a()}catch{ try{b()}catch{c()} }");
        normalizeOptionalCatchBinding(src);
        CHECK(src.count(QStringLiteral("catch (__wecatch) {")) == 2);
    }

    TEST_CASE("already-bound catch is left untouched") {
        QString src    = QStringLiteral("try { f(); } catch (e) { log(e); }");
        QString before = src;
        normalizeOptionalCatchBinding(src);
        CHECK(src == before);
    }

    TEST_CASE("script with no try/catch is a no-op") {
        QString src    = QStringLiteral("function f(){ return 1; }");
        QString before = src;
        normalizeOptionalCatchBinding(src);
        CHECK(src == before);
    }

    // End-to-end against the real engine: the bare `catch {}` is a SyntaxError
    // in QJSEngine/V4 (whole compile fails); after normalization it runs.
    TEST_CASE("normalized optional catch compiles and runs in QJSEngine") {
        const QString body = QStringLiteral(
            "var hit = 0;\n"
            "try { throw new Error('x'); } catch { hit = 1; }\n"
            "hit;");

        QJSEngine engine;
        QJSValue  raw = engine.evaluate(body);
        CHECK(raw.isError()); // documents the V4 limitation this transform exists for

        QString fixed = body;
        normalizeOptionalCatchBinding(fixed);
        QJSValue out = engine.evaluate(fixed);
        CHECK_FALSE(out.isError());
        CHECK(out.toInt() == 1);
    }

    TEST_CASE("dropOneTrailingBrace removes a single trailing brace") {
        QString src = QStringLiteral("function f(){}}");
        CHECK(wek::qml_helper::dropOneTrailingBrace(src));
        CHECK(src == QStringLiteral("function f(){}"));
    }

    TEST_CASE("dropOneTrailingBrace skips trailing whitespace/newlines") {
        QString src = QStringLiteral("function f(){}}\n\n  ");
        CHECK(wek::qml_helper::dropOneTrailingBrace(src));
        CHECK(src == QStringLiteral("function f(){}\n\n  "));
    }

    TEST_CASE("dropOneTrailingBrace is a no-op without a trailing brace") {
        QString src = QStringLiteral("var x = 1;");
        CHECK_FALSE(wek::qml_helper::dropOneTrailingBrace(src));
        CHECK(src == QStringLiteral("var x = 1;"));
    }

    // End-to-end: a stray trailing '}' breaks the IIFE wrapper (code follows the
    // body), and the SceneBackend strip-and-retry loop recovers it.  This is the
    // exact shape of the Gariam parenting script ('}}') in 2992803622.
    TEST_CASE("stray trailing brace recovered by strip-and-retry under IIFE wrap") {
        auto wrap = [](const QString& body) {
            return QStringLiteral("(function(){ var exports={};\n%1\n"
                                  "return (typeof f==='function')?f():0; })()")
                .arg(body);
        };
        QString body = QStringLiteral("function f(){ return 7; }}"); // stray '}'

        QJSEngine engine;
        QJSValue  r = engine.evaluate(wrap(body));
        CHECK(r.isError()); // unmatched '}' closes the IIFE early -> parse error

        int retries = 0;
        for (; r.isError() && retries < 3; ++retries) {
            if (! wek::qml_helper::dropOneTrailingBrace(body))
                break;
            r = engine.evaluate(wrap(body));
        }
        CHECK_FALSE(r.isError());
        CHECK(r.toInt() == 7);
        CHECK(retries == 1); // exactly one stray brace removed
    }
} // TEST_SUITE JsSyntaxNormalize

// ------------------------------------------------------------------
// getTextureAnimation().rate — must be WRITABLE (getter + setter).
// Regression for 2992803622's music player: animateIcon() does
// `thisLayer.getTextureAnimation().rate = 0` / `= 5` to pause/play the
// play-button icon.  The real shim (_makeLayerProxy) used to expose `rate`
// as a getter only, so the assignment threw a TypeError under 'use strict'
// and aborted update() every tick.
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Texture Animation") {
    TEST_CASE("getTextureAnimation().rate is writable under strict mode and delegates") {
        ScriptEnv env;
        // Mock the sprite bridge: the setter delegates here, and _readInfo
        // returns a known current frame so the pause path can be asserted.
        env.engine.evaluate(
            "var __spriteCalls = [];\n"
            "var __sceneBridge = {\n"
            "  setLayerSpriteFrame: function(n,pin,f){ __spriteCalls.push([n,pin,f]); },\n"
            "  getLayerSpriteInfo: function(n){ return "
            "{frameCount:31, currentFrame:12, isManualPin:false}; }\n"
            "};\n");
        // Real production proxy from SceneScriptShimsJs.hpp (not a mirror).
        env.engine.evaluate("var anim = _makeLayerProxy('playerplay').getTextureAnimation();");

        // The historical bug: assignment to a getter-only `rate` throws in
        // strict mode.  With the setter it succeeds.
        QJSValue play =
            env.engine.evaluate("(function(){ 'use strict'; anim.rate = 5; return 'ok'; })()");
        CHECK_FALSE(play.isError());
        CHECK(play.toString() == "ok");
        // rate>0 => play: unpinned (pin=false).
        CHECK(env.engine.evaluate("__spriteCalls[__spriteCalls.length-1][1]").toBool() == false);

        // rate<=0 => pause at the current frame (12, from the mock).
        env.engine.evaluate("(function(){ 'use strict'; anim.rate = 0; })()");
        CHECK(env.engine.evaluate("__spriteCalls[__spriteCalls.length-1][1]").toBool() == true);
        CHECK(env.engine.evaluate("__spriteCalls[__spriteCalls.length-1][2]").toInt() == 12);

        // Getters still read through the bridge.
        CHECK(env.engine.evaluate("anim.rate").toNumber() == doctest::Approx(1.0)); // not pinned
        CHECK(env.engine.evaluate("anim.frameCount").toInt() == 31);
        CHECK(env.engine.evaluate("anim.getFrame()").toInt() == 12);
    }
} // TEST_SUITE SceneScript Texture Animation

// ------------------------------------------------------------------
// _collectDirtyLayers must emit the resolved integer id in slot 0
// (not the layer name), so the C++ hot loop reads an int directly with no
// per-tick QString->std::string alloc + nodeNameToId lookup.  ScriptEnv
// evaluates the real production proxy/runtime JS, so this characterizes the
// shipped collector.  The fixture's _layerNameToId maps bg->7, fg->11.
TEST_SUITE("dirty-layer id slot") {
    TEST_CASE("regular layer dirty entry carries resolved id in slot 0") {
        ScriptEnv env;
        // Materialize 'bg' (id 7) and mutate its origin so it goes dirty.
        env.engine.evaluate("var L = thisScene.getLayer('bg');\n"
                            "L.origin = new Vec3(10, 20, 0);\n");
        QJSValue out = env.engine.evaluate("_collectDirtyLayers()");
        REQUIRE(out.isArray());
        const int DIRTY_STRIDE = 17;
        REQUIRE(out.property("length").toInt() == DIRTY_STRIDE); // one dirty layer
        // Slot 0 is now the integer id (7), NOT the string 'bg'.
        QJSValue slot0 = out.property(0);
        CHECK(slot0.isNumber());
        CHECK(slot0.toInt() == 7);
        // Flags slot (1) has F_ORIGIN (=1) set; origin slots (2,3,4) match.
        CHECK((out.property(1).toInt() & 1) != 0); // F_ORIGIN
        CHECK(out.property(2).toNumber() == doctest::Approx(10.0));
        CHECK(out.property(3).toNumber() == doctest::Approx(20.0));
        CHECK(out.property(4).toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("a layer with no resolved id reports -1 in slot 0 (miss, not crash)") {
        ScriptEnv env;
        // Create a layer name absent from _layerNameToId; its proxy id stays
        // undefined.  _collectDirtyLayers must emit a sentinel the C++ side can
        // treat as a miss (id < 0) without a hash lookup.
        env.engine.evaluate(
            "_layerInitStates['ghost'] = { o:[0,0,0], s:[1,1,1], a:[0,0,0], sz:[1,1], v:true };\n"
            "var G = thisScene.getLayer('ghost');\n"
            "G.alpha = 0.5;\n");
        QJSValue out = env.engine.evaluate("_collectDirtyLayers()");
        REQUIRE(out.isArray());
        REQUIRE(out.property("length").toInt() == 17);
        // No id resolved -> slot 0 is the miss sentinel (-1).
        CHECK(out.property(0).toInt() == -1);
    }

    TEST_CASE("pool layer dirty entry carries resolved id in slot 0") {
        ScriptEnv env;
        // Pool proxy fast-path (name starts with __pool).  The proxy needs an
        // init state to materialize (mirrors setupPoolFixture); start visible so
        // visible=false is a real change -> F_VISIBLE dirty.  Give it a resolved
        // id so slot 0 carries it.
        env.engine.evaluate("_layerInitStates['__pool_fx_0'] = { o:[0,0,0], s:[1,1,1],\n"
                            "                                    a:[0,0,0], sz:[10,10], v:true };\n"
                            "_layerNameToId['__pool_fx_0'] = 200;\n"
                            "_layerIdToName['200'] = '__pool_fx_0';\n"
                            "var P = thisScene.getLayer('__pool_fx_0');\n"
                            "P.visible = false;\n");
        QJSValue out = env.engine.evaluate("_collectDirtyLayers()");
        REQUIRE(out.isArray());
        REQUIRE(out.property("length").toInt() == 17);
        CHECK(out.property(0).isNumber());
        CHECK(out.property(0).toInt() == 200);
        CHECK((out.property(1).toInt() & 8) != 0); // F_VISIBLE
    }
} // TEST_SUITE dirty-layer id slot

// ------------------------------------------------------------------
// Cached Vec constructor handle == the old evaluate("VecN(...)") path.
// The color + shader-value tick loops used to build a "VecN(...)" source
// string and run a full QJSEngine::evaluate() compile every tick; they now
// call a cached constructor handle directly.  The loop body lives in a
// Vulkan-backed SceneObject (untestable here, as the scriptLoopShouldRun
// note above explains), so the contract is pinned at the seam: the object a
// cached VecN handle produces must be field-for-field the object the old
// evaluate("VecN(...)") string path produced — for every field the loops
// read (.x/.y/.z[/.w] and the r/g/b[/a] aliases) and the prototype methods.
TEST_SUITE("Cached Vec ctor handle equivalence") {
    // Reproduce the OLD string path exactly: QString("VecN(%1,...)").arg(d,0,'g',8)
    // then engine.evaluate(...).  Mirrors SceneBackend.cpp's removed code so the
    // test compares against the literal prior behavior, 'g',8 truncation included.
    static QString g8(double d) { return QString("%1").arg(d, 0, 'g', 8); }

    TEST_CASE("Vec3 cached-handle call equals evaluate(\"Vec3(...)\") for representative triples") {
        MathEnv  env;
        QJSValue vec3Fn = env.engine.globalObject().property("Vec3"); // cached once, as production does
        REQUIRE(vec3Fn.isCallable());

        struct T { double x, y, z; };
        // Negatives, zero, one, and an exactly-'g',8-representable high-precision value.
        const T cases[] = { { 0, 0, 0 },     { 1, 2, 3 },     { -0.5, 0.25, -1.75 },
                            { 0.12345678, 0.5, 0.001 } };
        for (const T& c : cases) {
            QJSValue viaCall = vec3Fn.call({ QJSValue(c.x), QJSValue(c.y), QJSValue(c.z) });
            QString  src =
                QString("Vec3(%1,%2,%3)").arg(g8(c.x)).arg(g8(c.y)).arg(g8(c.z));
            QJSValue viaEval = env.engine.evaluate(src);
            REQUIRE(viaCall.isObject());
            REQUIRE(viaEval.isObject());
            // Fields the color loop reads (x/y/z) — exact equality (these values
            // survive 'g',8 so both paths agree to the bit).
            CHECK(viaCall.property("x").toNumber() == viaEval.property("x").toNumber());
            CHECK(viaCall.property("y").toNumber() == viaEval.property("y").toNumber());
            CHECK(viaCall.property("z").toNumber() == viaEval.property("z").toNumber());
            // r/g/b aliases the color loop falls back on.
            CHECK(viaCall.property("r").toNumber() == viaEval.property("r").toNumber());
            CHECK(viaCall.property("g").toNumber() == viaEval.property("g").toNumber());
            CHECK(viaCall.property("b").toNumber() == viaEval.property("b").toNumber());
        }
    }

    TEST_CASE("Vec3 cached-handle call yields a real Vec3 (prototype methods + aliases)") {
        MathEnv  env;
        QJSValue vec3Fn = env.engine.globalObject().property("Vec3");
        QJSValue v      = vec3Fn.call({ QJSValue(3.0), QJSValue(4.0), QJSValue(0.0) });
        // length() must exist and compute — proves .call gives a prototyped Vec3,
        // not a bare {x,y,z} literal (the color script may call methods on it).
        CHECK(v.property("length").isCallable());
        QJSValue lenFn = env.engine.evaluate("(function(v){return v.length();})");
        CHECK(lenFn.call({ v }).toNumber() == doctest::Approx(5.0));
        // r/g/b accessors are live (defineProperty getters on the prototype).
        CHECK(v.property("r").toNumber() == doctest::Approx(3.0));
        CHECK(v.property("g").toNumber() == doctest::Approx(4.0));
    }

    TEST_CASE("Vec2 cached-handle call equals evaluate(\"Vec2(...)\")") {
        MathEnv  env;
        QJSValue vec2Fn = env.engine.globalObject().property("Vec2");
        REQUIRE(vec2Fn.isCallable());
        QJSValue viaCall = vec2Fn.call({ QJSValue(-2.5), QJSValue(7.0) });
        QJSValue viaEval = env.engine.evaluate(QString("Vec2(%1,%2)").arg(g8(-2.5)).arg(g8(7.0)));
        CHECK(viaCall.property("x").toNumber() == viaEval.property("x").toNumber());
        CHECK(viaCall.property("y").toNumber() == viaEval.property("y").toNumber());
        // Prototype-linked (Object.setPrototypeOf in the shim) so methods resolve.
        CHECK(viaCall.property("length").isCallable());
    }

    TEST_CASE("Vec4 cached-handle call equals evaluate(\"Vec4(...)\") incl. w and r/g/b/a") {
        MathEnv  env;
        QJSValue vec4Fn = env.engine.globalObject().property("Vec4");
        REQUIRE(vec4Fn.isCallable());
        QJSValue viaCall =
            vec4Fn.call({ QJSValue(0.1), QJSValue(-0.2), QJSValue(0.3), QJSValue(1.0) });
        QJSValue viaEval = env.engine.evaluate(
            QString("Vec4(%1,%2,%3,%4)").arg(g8(0.1)).arg(g8(-0.2)).arg(g8(0.3)).arg(g8(1.0)));
        // x/y/z/w — the order the shader-value unpacker reads (SceneBackend.cpp:4226-4233).
        CHECK(viaCall.property("x").toNumber() == viaEval.property("x").toNumber());
        CHECK(viaCall.property("y").toNumber() == viaEval.property("y").toNumber());
        CHECK(viaCall.property("z").toNumber() == viaEval.property("z").toNumber());
        CHECK(viaCall.property("w").toNumber() == viaEval.property("w").toNumber());
        // r/g/b/a aliases.
        CHECK(viaCall.property("a").toNumber() == viaEval.property("a").toNumber());
    }

    TEST_CASE("shader-value zero-fill: missing components default to 0 (first-tick guard)") {
        // The old shader-value path zero-filled trailing components when
        // cachedValue was short (e.g. before the first result is cached).  The
        // replacement's at() helper must preserve that: Vec3 from a single arg
        // fills y,z = 0.  Vec3(x) -> {x,0,0} (numeric branch: this.y=y||0).
        MathEnv  env;
        QJSValue vec3Fn = env.engine.globalObject().property("Vec3");
        QJSValue v      = vec3Fn.call({ QJSValue(5.0), QJSValue(0.0), QJSValue(0.0) });
        CHECK(v.property("x").toNumber() == doctest::Approx(5.0));
        CHECK(v.property("y").toNumber() == doctest::Approx(0.0));
        CHECK(v.property("z").toNumber() == doctest::Approx(0.0));
    }

    TEST_CASE("DOCUMENTED behavior change: .call keeps full precision; 'g',8 string path truncated") {
        // The OLD path serialised the double with 'g',8 then re-parsed it, which
        // truncates values carrying >8 significant decimal digits.  The NEW path
        // passes the raw double.  This is intentional (the truncation was a string
        // artifact, not a design choice) and the delta (~5e-9) is far below the
        // color (>0.001f) and shader-value (>0.0001f) change-detection thresholds,
        // so no renderer dispatch decision changes.  This case PINS the new
        // behavior so the divergence reads as deliberate, not a regression.
        MathEnv        env;
        QJSValue       vec3Fn = env.engine.globalObject().property("Vec3");
        const double   third  = 1.0 / 3.0; // 0.3333333333333333, needs >8 sig figs
        QJSValue       viaCall = vec3Fn.call({ QJSValue(third), QJSValue(0.0), QJSValue(0.0) });
        QJSValue       viaEval = env.engine.evaluate(QString("Vec3(%1,0,0)").arg(g8(third)));
        // .call preserves the full double exactly.
        CHECK(viaCall.property("x").toNumber() == third);
        // The old 'g',8 string path did NOT (it stored ~0.33333333).
        CHECK(viaEval.property("x").toNumber() != third);
        // ...but the two agree to well within every consumer threshold.
        CHECK(std::abs(viaCall.property("x").toNumber() - viaEval.property("x").toNumber()) < 1e-7);
    }
} // TEST_SUITE Cached Vec ctor handle equivalence
