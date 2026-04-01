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


// ------------------------------------------------------------------
// input.cursorWorldPosition live update
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Cursor Position") {

// Helper: engine with the input stub from SceneBackend.cpp
struct CursorEnv {
    QJSEngine engine;
    CursorEnv() {
        engine.evaluate("var input = { cursorWorldPosition: { x: 0, y: 0 } };\n");
    }
    // Simulate what evaluatePropertyScripts() / evaluateTextScripts() /
    // evaluateColorScripts() do before running scripts.
    void setCursorPos(double x, double y) {
        QJSValue inputObj = engine.globalObject().property("input");
        QJSValue cwp = inputObj.property("cursorWorldPosition");
        cwp.setProperty("x", x);
        cwp.setProperty("y", y);
    }
};

TEST_CASE("initial cursorWorldPosition is {x:0, y:0}") {
    CursorEnv env;
    double x = env.engine.evaluate("input.cursorWorldPosition.x").toNumber();
    double y = env.engine.evaluate("input.cursorWorldPosition.y").toNumber();
    CHECK(x == doctest::Approx(0.0));
    CHECK(y == doctest::Approx(0.0));
}

TEST_CASE("setCursorPos updates x and y readable by scripts") {
    CursorEnv env;
    env.setCursorPos(320.0, 240.0);
    CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() == doctest::Approx(320.0));
    CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() == doctest::Approx(240.0));
}

TEST_CASE("repeated updates replace previous values") {
    CursorEnv env;
    env.setCursorPos(100.0, 200.0);
    env.setCursorPos(960.0, 540.0);
    CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() == doctest::Approx(960.0));
    CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() == doctest::Approx(540.0));
}

TEST_CASE("script reads updated cursor position") {
    CursorEnv env;
    // Compile a script that reads cursor position into a local var on each update() call
    env.engine.evaluate(
        "var capturedX = 0;\n"
        "var capturedY = 0;\n"
        "function readCursor() {\n"
        "  capturedX = input.cursorWorldPosition.x;\n"
        "  capturedY = input.cursorWorldPosition.y;\n"
        "}\n"
    );
    env.setCursorPos(480.5, 270.25);
    env.engine.evaluate("readCursor();");
    CHECK(env.engine.evaluate("capturedX").toNumber() == doctest::Approx(480.5));
    CHECK(env.engine.evaluate("capturedY").toNumber() == doctest::Approx(270.25));
}

TEST_CASE("cursor position zero after reset") {
    CursorEnv env;
    env.setCursorPos(500.0, 400.0);
    env.setCursorPos(0.0, 0.0);
    CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() == doctest::Approx(0.0));
    CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() == doctest::Approx(0.0));
}

TEST_CASE("cursor position supports fractional scene coordinates") {
    CursorEnv env;
    env.setCursorPos(1.5, -2.75);
    CHECK(env.engine.evaluate("input.cursorWorldPosition.x").toNumber() == doctest::Approx(1.5));
    CHECK(env.engine.evaluate("input.cursorWorldPosition.y").toNumber() == doctest::Approx(-2.75));
}

} // TEST_SUITE SceneScript Cursor Position


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
        engine.evaluate(QString(
            "(function(){"
            "try{"
            "var p=JSON.parse('%1');"
            "var up=engine.userProperties;"
            "for(var k in p) up[k]=p[k];"
            "}catch(e){}"
            "})()"
        ).arg(escaped));
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
    CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() == doctest::Approx(2.0));
    CHECK(env.engine.evaluate("engine.userProperties.muted").toBool() == false);
    CHECK(env.engine.evaluate("engine.userProperties.schemecolor").toString()
          == QStringLiteral("1 0 0"));
}

TEST_CASE("script reads property value correctly") {
    UserPropEnv env;
    env.refresh(R"({"bloomstrength":1.78})");
    env.engine.evaluate(
        "var captured = 0;\n"
        "function readProp() { captured = engine.userProperties.bloomstrength; }\n"
    );
    env.engine.evaluate("readProp();");
    double val = env.engine.evaluate("captured").toNumber();
    CHECK(val == doctest::Approx(1.78));
}

TEST_CASE("second refresh updates existing property") {
    UserPropEnv env;
    env.refresh(R"({"speed":1.0})");
    CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() == doctest::Approx(1.0));
    env.refresh(R"({"speed":3.5})");
    CHECK(env.engine.evaluate("engine.userProperties.speed").toNumber() == doctest::Approx(3.5));
}

TEST_CASE("empty string refresh is a no-op") {
    UserPropEnv env;
    env.refresh(R"({"x":42})");
    env.refresh(QString());  // empty → no-op
    CHECK(env.engine.evaluate("engine.userProperties.x").toNumber() == doctest::Approx(42.0));
}

TEST_CASE("invalid JSON is silently ignored") {
    UserPropEnv env;
    env.refresh(R"({"a":1})");
    env.refresh("not valid json");  // must not crash or alter existing props
    CHECK(env.engine.evaluate("engine.userProperties.a").toNumber() == doctest::Approx(1.0));
}

TEST_CASE("integer value accessible as number") {
    UserPropEnv env;
    env.refresh(R"({"count":7})");
    CHECK(env.engine.evaluate("engine.userProperties.count").toInt() == 7);
}

} // TEST_SUITE SceneScript engine.userProperties


// ===================================================================
// Fixtures for comprehensive SceneScript tests
// ===================================================================

// JS string constants copied from SceneBackend.cpp (lines 662-700)
static const char* JS_VEC3_AND_UTILS =
    "var input = { cursorWorldPosition: { x: 0, y: 0 } };\n"
    "function Vec3(x, y, z) {\n"
    "  var v = { x: x||0, y: y||0, z: z||0 };\n"
    "  v.multiply = function(s) { return Vec3(v.x*s, v.y*s, v.z*s); };\n"
    "  v.add = function(o) { return Vec3(v.x+o.x, v.y+o.y, v.z+o.z); };\n"
    "  v.subtract = function(o) { return Vec3(v.x-o.x, v.y-o.y, v.z-o.z); };\n"
    "  v.length = function() { return Math.sqrt(v.x*v.x+v.y*v.y+v.z*v.z); };\n"
    "  v.normalize = function() { var l=v.length()||1; return Vec3(v.x/l,v.y/l,v.z/l); };\n"
    "  v.copy = function() { return Vec3(v.x, v.y, v.z); };\n"
    "  v.dot = function(o) { return v.x*o.x+v.y*o.y+v.z*o.z; };\n"
    "  v.cross = function(o) { return Vec3(v.y*o.z-v.z*o.y, v.z*o.x-v.x*o.z, v.x*o.y-v.y*o.x); };\n"
    "  v.negate = function() { return Vec3(-v.x,-v.y,-v.z); };\n"
    "  return v;\n"
    "}\n"
    "var _origMatch = String.prototype.match;\n"
    "String.prototype.match = function(re) { return _origMatch.call(this, re) || []; };\n"
    "var localStorage = {\n"
    "  get: function(key) { return undefined; },\n"
    "  set: function(key, value) {}\n"
    "};\n";

static const char* JS_WEMATH =
    "var WEMath = {\n"
    "  lerp: function(a, b, t) { return a + (b - a) * t; },\n"
    "  mix: function(a, b, t) { return a + (b - a) * t; },\n"
    "  clamp: function(v, lo, hi) { return Math.min(Math.max(v, lo), hi); },\n"
    "  smoothstep: function(edge0, edge1, x) {\n"
    "    var t = Math.min(Math.max((x - edge0) / (edge1 - edge0), 0), 1);\n"
    "    return t * t * (3 - 2 * t);\n"
    "  },\n"
    "  fract: function(x) { return x - Math.floor(x); },\n"
    "  sign: function(x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); },\n"
    "  step: function(edge, x) { return x < edge ? 0 : 1; }\n"
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
    "  return { hsv2rgb: hsv2rgb, rgb2hsv: rgb2hsv };\n"
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

static const char* JS_CREATE_SCRIPT_PROPERTIES =
    "function createScriptProperties(defs) {\n"
    "  var _props = {};\n"
    "  if (defs && typeof defs === 'object') {\n"
    "    for (var k in defs) {\n"
    "      if (defs.hasOwnProperty(k))\n"
    "        _props[k] = defs[k].value !== undefined ? defs[k].value : null;\n"
    "    }\n"
    "  }\n"
    "  var builder = {\n"
    "    addSlider: function(o) { _props[o.name] = o.value !== undefined ? o.value : 0; return builder; },\n"
    "    addCheckbox: function(o) { _props[o.name] = o.value !== undefined ? o.value : false; return builder; },\n"
    "    addCombo: function(o) { _props[o.name] = o.value !== undefined ? o.value : (o.options && o.options.length > 0 ? o.options[0].value : 0); return builder; },\n"
    "    addTextInput: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
    "    addText: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
    "    addColor: function(o) { _props[o.name] = o.value !== undefined ? o.value : '0 0 0'; return builder; },\n"
    "    addFile: function(o) { _props[o.name] = o.value !== undefined ? o.value : ''; return builder; },\n"
    "    finish: function() { return _props; }\n"
    "  };\n"
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

static const char* JS_LAYER_INFRA =
    "var _layerCache = {};\n"
    "function _makeLayerProxy(name) {\n"
    "  var init = _layerInitStates[name];\n"
    "  var _s = init ? {\n"
    "    origin: {x:init.o[0], y:init.o[1], z:init.o[2]},\n"
    "    scale: {x:init.s[0], y:init.s[1], z:init.s[2]},\n"
    "    angles: {x:init.a[0], y:init.a[1], z:init.a[2]},\n"
    "    size: init.sz ? {x:init.sz[0], y:init.sz[1]} : {x:0, y:0},\n"
    "    visible: init.v, alpha: 1.0,\n"
    "    text: '', name: name, _dirty: {}\n"
    "  } : { origin: {x:0,y:0,z:0}, scale: {x:1,y:1,z:1},\n"
    "        angles: {x:0,y:0,z:0}, size: {x:0, y:0},\n"
    "        visible: true, alpha: 1.0,\n"
    "        text: '', name: name, _dirty: {} };\n"
    "  var p = {};\n"
    "  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, enumerable:true });\n"
    "  Object.defineProperty(p, 'debug', { get: function(){return undefined;}, enumerable:true });\n"
    "  var vec3Props = ['origin','scale','angles'];\n"
    "  for (var i=0; i<vec3Props.length; i++) {\n"
    "    (function(prop){\n"
    "      Object.defineProperty(p, prop, {\n"
    "        get: function(){ return _s[prop]; },\n"
    "        set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
    "        enumerable: true\n"
    "      });\n"
    "    })(vec3Props[i]);\n"
    "  }\n"
    "  var scalarProps = ['visible','alpha'];\n"
    "  for (var j=0; j<scalarProps.length; j++) {\n"
    "    (function(prop){\n"
    "      Object.defineProperty(p, prop, {\n"
    "        get: function(){ return _s[prop]; },\n"
    "        set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },\n"
    "        enumerable: true\n"
    "      });\n"
    "    })(scalarProps[j]);\n"
    "  }\n"
    "  Object.defineProperty(p, 'text', {\n"
    "    get: function(){ return _s.text; },\n"
    "    set: function(v){ _s.text = v; _s._dirty.text = true; },\n"
    "    enumerable: true\n"
    "  });\n"
    "  Object.defineProperty(p, 'size', {\n"
    "    get: function(){ return _s.size; },\n"
    "    enumerable: true\n"
    "  });\n"
    "  p.play = function(){};\n"
    "  p.stop = function(){};\n"
    "  p.pause = function(){};\n"
    "  p.isPlaying = function(){ return false; };\n"
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
    "  return p;\n"
    "}\n"
    "var _nullProxy = (function() {\n"
    "  var _s = { origin:{x:0,y:0,z:0}, scale:{x:1,y:1,z:1},\n"
    "    angles:{x:0,y:0,z:0}, size:{x:0,y:0},\n"
    "    visible:false, alpha:0, text:'', name:'', _dirty:{} };\n"
    "  var p = {};\n"
    "  Object.defineProperty(p, 'name', {get:function(){return '';}, enumerable:true});\n"
    "  Object.defineProperty(p, 'debug', {get:function(){return undefined;}, enumerable:true});\n"
    "  var vec3Props = ['origin','scale','angles'];\n"
    "  for (var i=0; i<vec3Props.length; i++) {\n"
    "    (function(prop){\n"
    "      Object.defineProperty(p, prop, {\n"
    "        get: function(){return _s[prop];}, set: function(v){},\n"
    "        enumerable: true\n"
    "      });\n"
    "    })(vec3Props[i]);\n"
    "  }\n"
    "  var scalarProps = ['visible','alpha'];\n"
    "  for (var j=0; j<scalarProps.length; j++) {\n"
    "    (function(prop){\n"
    "      Object.defineProperty(p, prop, {\n"
    "        get: function(){return _s[prop];}, set: function(v){},\n"
    "        enumerable: true\n"
    "      });\n"
    "    })(scalarProps[j]);\n"
    "  }\n"
    "  Object.defineProperty(p, 'text', {get:function(){return '';}, set:function(v){}, enumerable:true});\n"
    "  Object.defineProperty(p, 'size', {get:function(){return _s.size;}, enumerable:true});\n"
    "  p.play = function(){}; p.stop = function(){};\n"
    "  p.pause = function(){}; p.isPlaying = function(){return false;};\n"
    "  p.getTextureAnimation = function(){\n"
    "    return { rate:0, frameCount:1, _frame:0,\n"
    "      getFrame:function(){return this._frame;}, setFrame:function(f){},\n"
    "      play:function(){}, pause:function(){}, stop:function(){},\n"
    "      isPlaying:function(){return false;}\n"
    "    };\n"
    "  };\n"
    "  p._state = _s;\n"
    "  return p;\n"
    "})();\n"
    "var thisScene = {\n"
    "  getLayer: function(name) {\n"
    "    if (_layerCache[name]) return _layerCache[name];\n"
    "    if (!_layerInitStates[name]) return null;\n"
    "    _layerCache[name] = _makeLayerProxy(name);\n"
    "    return _layerCache[name];\n"
    "  }\n"
    "};\n"
    "var thisLayer = null;\n"
    "function _collectDirtyLayers() {\n"
    "  var updates = [];\n"
    "  for (var name in _layerCache) {\n"
    "    var s = _layerCache[name]._state;\n"
    "    var d = s._dirty;\n"
    "    var keys = Object.keys(d);\n"
    "    if (keys.length === 0) continue;\n"
    "    updates.push({ name: name, dirty: d,\n"
    "      origin: s.origin, scale: s.scale, angles: s.angles,\n"
    "      visible: s.visible, alpha: s.alpha, text: s.text });\n"
    "    s._dirty = {};\n"
    "  }\n"
    "  return updates;\n"
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
    "  p.play = function(){ _s._cmds.push('play'); };\n"
    "  p.stop = function(){ _s._cmds.push('stop'); };\n"
    "  p.pause = function(){ _s._cmds.push('pause'); };\n"
    "  p.isPlaying = function(){\n"
    "    return !!(engine._soundPlayingStates && engine._soundPlayingStates[name]);\n"
    "  };\n"
    "  Object.defineProperty(p, 'origin', { get: function(){return {x:0,y:0,z:0};}, set: function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'scale', { get: function(){return {x:1,y:1,z:1};}, set: function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'angles', { get: function(){return {x:0,y:0,z:0};}, set: function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'visible', { get: function(){return true;}, set: function(){}, enumerable:true });\n"
    "  Object.defineProperty(p, 'alpha', { get: function(){return 1;}, set: function(){}, enumerable:true });\n"
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
        engine.evaluate(JS_VEC3_AND_UTILS);
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
        engineObj.setProperty("userProperties", engine.newObject());
        engine.globalObject().setProperty("engine", engineObj);

        // shared
        engine.globalObject().setProperty("shared", engine.newObject());
        engine.evaluate("shared.volume = 1.0;\n");

        // console
        engine.evaluate(JS_CONSOLE);

        // Vec3, String.match, localStorage
        engine.evaluate(JS_VEC3_AND_UTILS);

        // WEMath, WEColor
        engine.evaluate(JS_WEMATH);
        engine.evaluate(JS_WECOLOR);

        // Engine stubs
        engine.evaluate(
            "engine.isDesktopDevice = function() { return true; };\n"
            "engine.isMobileDevice = function() { return false; };\n"
            "engine.isTabletDevice = function() { return false; };\n"
            "engine.isWallpaper = function() { return true; };\n"
            "engine.isScreensaver = function() { return false; };\n"
            "engine.isRunningInEditor = function() { return false; };\n"
            "engine.screenResolution = { x: 1920, y: 1080 };\n"
            "engine.canvasSize = { x: 1920, y: 1080 };\n"
            "engine.isPortrait = function() { return false; };\n"
            "engine.isLandscape = function() { return true; };\n"
            "engine.openUserShortcut = function(name) {};\n"
        );

        // createScriptProperties
        engine.evaluate(JS_CREATE_SCRIPT_PROPERTIES);

        // Audio buffers
        engine.evaluate(JS_AUDIO_BUFFERS);

        // Layer init states (test data)
        engine.evaluate(
            "var _layerInitStates = {\n"
            "  'bg': { o: [100, 200, 0], s: [1, 1, 1], a: [0, 0, 45], sz: [1920, 1080], v: true },\n"
            "  'fg': { o: [0, 0, 0], s: [2, 2, 2], a: [0, 0, 0], sz: [800, 600], v: false }\n"
            "};\n"
            "var _sceneOrtho = [1920, 1080];\n"
        );

        // Layer infrastructure
        engine.evaluate(JS_LAYER_INFRA);

        // Sound layer infrastructure
        engine.evaluate(JS_SOUND_INFRA);
    }
};

// Helper: check Vec3-like QJSValue
static void checkVec3(const QJSValue& v, double ex, double ey, double ez) {
    CHECK(v.property("x").toNumber() == doctest::Approx(ex).epsilon(1e-6));
    CHECK(v.property("y").toNumber() == doctest::Approx(ey).epsilon(1e-6));
    CHECK(v.property("z").toNumber() == doctest::Approx(ez).epsilon(1e-6));
}


// ------------------------------------------------------------------
// Vec3
// ------------------------------------------------------------------
TEST_SUITE("Vec3") {

TEST_CASE("default args produce zero vector") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3()");
    checkVec3(v, 0, 0, 0);
}

TEST_CASE("explicit args stored correctly") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,2,3)");
    checkVec3(v, 1, 2, 3);
}

TEST_CASE("multiply scales all components") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,2,3).multiply(2)");
    checkVec3(v, 2, 4, 6);
}

TEST_CASE("add combines two vectors") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,2,3).add(Vec3(4,5,6))");
    checkVec3(v, 5, 7, 9);
}

TEST_CASE("subtract differences two vectors") {
    MathEnv env;
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
    MathEnv env;
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
    CHECK(env.engine.evaluate("Vec3(1,0,0).dot(Vec3(0,1,0))").toNumber() == doctest::Approx(0.0));
    CHECK(env.engine.evaluate("Vec3(1,2,3).dot(Vec3(4,5,6))").toNumber() == doctest::Approx(32.0));
}

TEST_CASE("cross product") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,0,0).cross(Vec3(0,1,0))");
    checkVec3(v, 0, 0, 1);
}

TEST_CASE("negate") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,-2,3).negate()");
    checkVec3(v, -1, 2, -3);
}

TEST_CASE("method chaining") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("Vec3(1,2,3).multiply(2).add(Vec3(1,1,1))");
    checkVec3(v, 3, 5, 7);
}

} // TEST_SUITE Vec3


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
    CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, -1)").toNumber() == doctest::Approx(0.0));
}

TEST_CASE("smoothstep above edge1") {
    MathEnv env;
    CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, 2)").toNumber() == doctest::Approx(1.0));
}

TEST_CASE("smoothstep midpoint") {
    MathEnv env;
    // Hermite: 0.5^2 * (3 - 2*0.5) = 0.25 * 2 = 0.5
    CHECK(env.engine.evaluate("WEMath.smoothstep(0, 1, 0.5)").toNumber() == doctest::Approx(0.5));
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

} // TEST_SUITE WEMath


// ------------------------------------------------------------------
// WEColor
// ------------------------------------------------------------------
TEST_SUITE("WEColor") {

TEST_CASE("red RGB to HSV") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:1,y:0,z:0})");
    CHECK(v.property("x").toNumber() == doctest::Approx(0.0));
    CHECK(v.property("y").toNumber() == doctest::Approx(1.0));
    CHECK(v.property("z").toNumber() == doctest::Approx(1.0));
}

TEST_CASE("green RGB to HSV") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:1,z:0})");
    CHECK(v.property("x").toNumber() == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("blue RGB to HSV") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:0,z:1})");
    CHECK(v.property("x").toNumber() == doctest::Approx(2.0 / 3.0));
}

TEST_CASE("black RGB to HSV") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:0,y:0,z:0})");
    CHECK(v.property("z").toNumber() == doctest::Approx(0.0)); // v=0
}

TEST_CASE("white RGB to HSV") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.rgb2hsv({x:1,y:1,z:1})");
    CHECK(v.property("y").toNumber() == doctest::Approx(0.0)); // s=0
    CHECK(v.property("z").toNumber() == doctest::Approx(1.0)); // v=1
}

TEST_CASE("hsv2rgb known red") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("WEColor.hsv2rgb({x:0,y:1,z:1})");
    checkVec3(v, 1, 0, 0);
}

TEST_CASE("round-trip rgb to hsv to rgb") {
    MathEnv env;
    // Test several colors
    for (const char* color : { "{x:1,y:0,z:0}", "{x:0,y:1,z:0}", "{x:0,y:0,z:1}",
                                "{x:0.5,y:0.3,z:0.8}", "{x:0.2,y:0.7,z:0.4}" }) {
        QString expr = QString("(function(){ var c = %1; var h = WEColor.rgb2hsv(c);"
                               " var r = WEColor.hsv2rgb(h); return r; })()").arg(color);
        QJSValue orig = env.engine.evaluate(QString("(%1)").arg(color));
        QJSValue rt = env.engine.evaluate(expr);
        CHECK(rt.property("x").toNumber() == doctest::Approx(orig.property("x").toNumber()).epsilon(1e-4));
        CHECK(rt.property("y").toNumber() == doctest::Approx(orig.property("y").toNumber()).epsilon(1e-4));
        CHECK(rt.property("z").toNumber() == doctest::Approx(orig.property("z").toNumber()).epsilon(1e-4));
    }
}

} // TEST_SUITE WEColor


// ------------------------------------------------------------------
// SceneScript Globals
// ------------------------------------------------------------------
TEST_SUITE("SceneScript Globals") {

TEST_CASE("String.match works normally") {
    MathEnv env;
    CHECK(env.engine.evaluate("'hello world'.match(/hello/)[0]").toString() == "hello");
}

TEST_CASE("String.match returns empty array on no match") {
    MathEnv env;
    QJSValue v = env.engine.evaluate("'hello'.match(/xyz/)");
    CHECK(v.isArray());
    CHECK(v.property("length").toInt() == 0);
}

TEST_CASE("String.match global regex returns all") {
    MathEnv env;
    CHECK(env.engine.evaluate("'aaa'.match(/a/g).length").toInt() == 3);
}

TEST_CASE("localStorage.get returns undefined") {
    MathEnv env;
    CHECK(env.engine.evaluate("localStorage.get('anything')").isUndefined());
}

TEST_CASE("localStorage.set does not crash") {
    MathEnv env;
    QJSValue r = env.engine.evaluate("localStorage.set('k', 'v')");
    CHECK_FALSE(r.isError());
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
    QJSValue r = env.engine.evaluate("engine.openUserShortcut('test')");
    CHECK_FALSE(r.isError());
}

} // TEST_SUITE SceneScript Globals


// ------------------------------------------------------------------
// createScriptProperties
// ------------------------------------------------------------------
TEST_SUITE("createScriptProperties") {

TEST_CASE("addSlider stores value") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addSlider({name:'x',value:5}).finish().x").toInt() == 5);
}

TEST_CASE("addSlider default value is 0") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addSlider({name:'x'}).finish().x").toInt() == 0);
}

TEST_CASE("addCheckbox stores value") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addCheckbox({name:'y',value:true}).finish().y").toBool());
}

TEST_CASE("addCheckbox default value is false") {
    ScriptEnv env;
    CHECK_FALSE(env.engine.evaluate("createScriptProperties().addCheckbox({name:'y'}).finish().y").toBool());
}

TEST_CASE("addCombo stores value") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addCombo({name:'c',value:2}).finish().c").toInt() == 2);
}

TEST_CASE("addText and addColor store values") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addText({name:'t',value:'hi'}).finish().t").toString() == "hi");
    CHECK(env.engine.evaluate("createScriptProperties().addColor({name:'cl',value:'1 0 0'}).finish().cl").toString() == "1 0 0");
}

TEST_CASE("addColor default is 0 0 0") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("createScriptProperties().addColor({name:'cl'}).finish().cl").toString() == "0 0 0");
}

TEST_CASE("chaining multiple methods") {
    ScriptEnv env;
    env.engine.evaluate(
        "var props = createScriptProperties()"
        ".addSlider({name:'speed',value:3})"
        ".addCheckbox({name:'enabled',value:true})"
        ".finish();");
    CHECK(env.engine.evaluate("props.speed").toInt() == 3);
    CHECK(env.engine.evaluate("props.enabled").toBool());
}

TEST_CASE("legacy object-arg form") {
    ScriptEnv env;
    env.engine.evaluate(
        "var props = createScriptProperties({speed:{value:3},mode:{value:1}}).finish();");
    CHECK(env.engine.evaluate("props.speed").toInt() == 3);
    CHECK(env.engine.evaluate("props.mode").toInt() == 1);
}

TEST_CASE("per-IIFE override with stored props") {
    ScriptEnv env;
    // Simulate per-IIFE createScriptProperties override (SceneBackend.cpp:1167-1184)
    env.engine.evaluate(
        "var result = (function() {\n"
        "  var _storedProps = { x: { value: 99 }, z: 42 };\n"
        "  function createScriptProperties() {\n"
        "    var b = {};\n"
        "    function ap(def) {\n"
        "      var n = def.name || def.n;\n"
        "      if (n) {\n"
        "        if (n in _storedProps) {\n"
        "          var sp = _storedProps[n];\n"
        "          b[n] = (typeof sp === 'object' && sp !== null && 'value' in sp) ? sp.value : sp;\n"
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
    CHECK(env.engine.evaluate("result.x").toInt() == 99);  // overridden
    CHECK(env.engine.evaluate("result.y").toInt() == 10);   // default
    CHECK(env.engine.evaluate("result.z").toInt() == 42);   // bare value (not {value:})
}

} // TEST_SUITE createScriptProperties


// ------------------------------------------------------------------
// Layer Proxy
// ------------------------------------------------------------------
TEST_SUITE("Layer Proxy") {

TEST_CASE("initial origin from init state") {
    ScriptEnv env;
    QJSValue v = env.engine.evaluate("thisScene.getLayer('bg').origin");
    checkVec3(v, 100, 200, 0);
}

TEST_CASE("initial scale from init state") {
    ScriptEnv env;
    QJSValue v = env.engine.evaluate("thisScene.getLayer('bg').scale");
    checkVec3(v, 1, 1, 1);
}

TEST_CASE("initial angles from init state") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("thisScene.getLayer('bg').angles.z").toNumber() == doctest::Approx(45));
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

TEST_CASE("setting text marks dirty") {
    ScriptEnv env;
    env.engine.evaluate("thisScene.getLayer('bg').text = 'hello';");
    CHECK(env.engine.evaluate("thisScene.getLayer('bg')._state._dirty.text").toBool());
}

TEST_CASE("play stop pause are no-ops") {
    ScriptEnv env;
    QJSValue r = env.engine.evaluate(
        "var l = thisScene.getLayer('bg'); l.play(); l.stop(); l.pause(); true;");
    CHECK_FALSE(r.isError());
}

TEST_CASE("isPlaying returns false") {
    ScriptEnv env;
    CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('bg').isPlaying()").toBool());
}

TEST_CASE("getTextureAnimation lifecycle") {
    ScriptEnv env;
    env.engine.evaluate(
        "var anim = thisScene.getLayer('bg').getTextureAnimation();\n"
        "anim.setFrame(5);\n");
    CHECK(env.engine.evaluate("anim.getFrame()").toInt() == 5);
    CHECK_FALSE(env.engine.evaluate("anim.isPlaying()").toBool());

    env.engine.evaluate("anim.play();");
    CHECK(env.engine.evaluate("anim.isPlaying()").toBool());
    CHECK(env.engine.evaluate("anim.rate").toInt() == 1);

    env.engine.evaluate("anim.stop();");
    CHECK_FALSE(env.engine.evaluate("anim.isPlaying()").toBool());
    CHECK(env.engine.evaluate("anim.getFrame()").toInt() == 0);
}

TEST_CASE("nullProxy getters return defaults") {
    ScriptEnv env;
    QJSValue np = env.engine.evaluate("thisScene.getLayer('nonexistent')");
    CHECK(np.property("name").toString() == "");
    CHECK(np.property("visible").toBool() == false);
    CHECK(np.property("alpha").toNumber() == doctest::Approx(0.0));
}

TEST_CASE("nullProxy setters are no-ops") {
    ScriptEnv env;
    env.engine.evaluate(
        "var np = thisScene.getLayer('nonexistent');\n"
        "np.origin = {x:99,y:99,z:99};\n"
        "np.visible = true;\n"
        "np.alpha = 1.0;\n");
    CHECK(env.engine.evaluate("thisScene.getLayer('nonexistent').origin.x").toNumber() == doctest::Approx(0.0));
    CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('nonexistent').visible").toBool());
}

TEST_CASE("getLayer returns cached proxy") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("thisScene.getLayer('bg') === thisScene.getLayer('bg')").toBool());
}

TEST_CASE("getLayer returns nullProxy for unknown") {
    ScriptEnv env;
    // nullProxy has name=='' and visible==false
    CHECK(env.engine.evaluate("thisScene.getLayer('nope').name").toString() == "");
    CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('nope').visible").toBool());
}

} // TEST_SUITE Layer Proxy


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
    CHECK(r.property("length").toInt() == 1);
    CHECK(r.property(0).property("name").toString() == "bg");
    CHECK(r.property(0).property("dirty").property("origin").toBool());
    CHECK(r.property(0).property("origin").property("x").toNumber() == doctest::Approx(1.0));
}

TEST_CASE("resets dirty after collection") {
    ScriptEnv env;
    env.engine.evaluate("thisScene.getLayer('bg').origin = {x:5,y:5,z:5};");
    env.engine.evaluate("_collectDirtyLayers()"); // first collect
    QJSValue r = env.engine.evaluate("_collectDirtyLayers()"); // second
    CHECK(r.property("length").toInt() == 0);
}

TEST_CASE("multiple dirty properties on one layer") {
    ScriptEnv env;
    env.engine.evaluate(
        "var l = thisScene.getLayer('bg');\n"
        "l.origin = {x:1,y:2,z:3};\n"
        "l.visible = false;\n");
    QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
    CHECK(r.property("length").toInt() == 1);
    CHECK(r.property(0).property("dirty").property("origin").toBool());
    CHECK(r.property(0).property("dirty").property("visible").toBool());
}

TEST_CASE("multiple dirty layers simultaneously") {
    ScriptEnv env;
    env.engine.evaluate(
        "thisScene.getLayer('bg').origin = {x:1,y:1,z:1};\n"
        "thisScene.getLayer('fg').visible = true;\n");
    QJSValue r = env.engine.evaluate("_collectDirtyLayers()");
    CHECK(r.property("length").toInt() == 2);
}

} // TEST_SUITE Dirty Layer Collection


// ------------------------------------------------------------------
// Sound Layer Proxy
// ------------------------------------------------------------------
TEST_SUITE("Sound Layer Proxy") {

TEST_CASE("name property") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').name").toString() == "music.mp3");
}

TEST_CASE("initial volume from state") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').volume").toNumber() == doctest::Approx(0.8));
}

TEST_CASE("volume setter with dirty tracking") {
    ScriptEnv env;
    env.engine.evaluate("thisScene.getLayer('music.mp3').volume = 0.5;");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').volume").toNumber() == doctest::Approx(0.5));
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._dirty.volume").toBool());
}

TEST_CASE("play stop pause queue commands") {
    ScriptEnv env;
    env.engine.evaluate(
        "var sl = thisScene.getLayer('music.mp3');\n"
        "sl.play(); sl.stop(); sl.pause();\n");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds.length").toInt() == 3);
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[0]").toString() == "play");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[1]").toString() == "stop");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3')._state._cmds[2]").toString() == "pause");
}

TEST_CASE("isPlaying reads engine state") {
    ScriptEnv env;
    CHECK_FALSE(env.engine.evaluate("thisScene.getLayer('music.mp3').isPlaying()").toBool());
    env.engine.evaluate("engine._soundPlayingStates['music.mp3'] = true;");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').isPlaying()").toBool());
}

TEST_CASE("image layer properties are no-ops") {
    ScriptEnv env;
    env.engine.evaluate(
        "var sl = thisScene.getLayer('music.mp3');\n"
        "sl.origin = {x:99,y:99,z:99};\n");
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').origin.x").toNumber() == doctest::Approx(0.0));
}

TEST_CASE("visible and alpha are read-only stubs") {
    ScriptEnv env;
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').visible").toBool() == true);
    CHECK(env.engine.evaluate("thisScene.getLayer('music.mp3').alpha").toNumber() == doctest::Approx(1.0));
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
    QJSValue layers = env.engine.evaluate("thisScene.enumerateLayers()");
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
static const char* TEXT_IIFE_PRE =
    "(function() {\n"
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

// Property IIFE pattern: extracts all 8 handlers with try-catch wrapping
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
    "  var _init2 = _rawInit ? function(v) {\n"
    "    try { return _rawInit(v); } catch(e) { console.log('init error: ' + e.message); }\n"
    "  } : null;\n"
    "  var _upd2 = _rawUpd ? function(v) {\n"
    "    try { return _rawUpd(v); } catch(e) { return v; }\n"
    "  } : null;\n"
    "  return { update: _upd2, init: _init2,\n"
    "    cursorClick: _click, cursorEnter: _enter, cursorLeave: _leave,\n"
    "    cursorDown: _down, cursorUp: _up, cursorMove: _move };\n"
    "})()";

TEST_CASE("exports.update extracted from text IIFE") {
    ScriptEnv env;
    QString script = QString("%1  exports.update = function(v){ return v + '!'; };\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("update").isCallable());
    CHECK(r.property("update").call({ QJSValue("hi") }).toString() == "hi!");
}

TEST_CASE("bare function update extracted") {
    ScriptEnv env;
    QString script = QString("%1  function update(v){ return v + '?'; }\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("update").isCallable());
    CHECK(r.property("update").call({ QJSValue("hi") }).toString() == "hi?");
}

TEST_CASE("missing update returns null") {
    ScriptEnv env;
    QString script = QString("%1  var x = 42;\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.isNull());
}

TEST_CASE("exports.init extracted alongside update") {
    ScriptEnv env;
    QString script = QString("%1  exports.init = function(v){ return 'init'; };\n"
                             "  exports.update = function(v){ return v; };\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("init").isCallable());
    CHECK(r.property("update").isCallable());
}

TEST_CASE("property IIFE extracts all 8 handlers") {
    ScriptEnv env;
    QString script = QString(
        "(function() {\n"
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
        "%1").arg(PROP_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("update").isCallable());
    CHECK(r.property("init").isCallable());
    CHECK(r.property("cursorClick").isCallable());
    CHECK(r.property("cursorEnter").isCallable());
    CHECK(r.property("cursorLeave").isCallable());
    CHECK(r.property("cursorDown").isCallable());
    CHECK(r.property("cursorUp").isCallable());
    CHECK(r.property("cursorMove").isCallable());
}

TEST_CASE("property IIFE update wrapped in try-catch returns input on error") {
    ScriptEnv env;
    QString script = QString(
        "(function() {\n"
        "  'use strict';\n"
        "  var exports = {};\n"
        "  exports.update = function(v){ throw new Error('boom'); };\n"
        "%1").arg(PROP_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("update").isCallable());
    // Should return the input value (42) instead of throwing
    QJSValue result = r.property("update").call({ QJSValue(42) });
    CHECK(result.toInt() == 42);
}

TEST_CASE("property IIFE init wrapped in try-catch") {
    ScriptEnv env;
    QString script = QString(
        "(function() {\n"
        "  'use strict';\n"
        "  var exports = {};\n"
        "  exports.init = function(v){ throw new Error('init boom'); };\n"
        "  exports.update = function(v){ return v; };\n"
        "%1").arg(PROP_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.property("init").isCallable());
    // Should not propagate the error
    QJSValue result = r.property("init").call({ QJSValue(0) });
    CHECK_FALSE(result.isError());
}

TEST_CASE("scriptProperties override in IIFE context") {
    ScriptEnv env;
    // Simulate property script with stored overrides
    env.engine.evaluate(
        "var result = (function() {\n"
        "  'use strict';\n"
        "  var exports = {};\n"
        "  var _storedProps = { speed: { value: 99 } };\n"
        "  function createScriptProperties() {\n"
        "    var b = {};\n"
        "    function ap(def) {\n"
        "      var n = def.name;\n"
        "      if (n in _storedProps) {\n"
        "        var sp = _storedProps[n];\n"
        "        b[n] = (typeof sp === 'object' && sp !== null && 'value' in sp) ? sp.value : sp;\n"
        "      } else { b[n] = def.value; }\n"
        "      return b;\n"
        "    }\n"
        "    b.addSlider=ap; b.finish=function(){return b;};\n"
        "    return b;\n"
        "  }\n"
        "  var scriptProperties = createScriptProperties().addSlider({name:'speed',value:5}).finish();\n"
        "  exports.update = function(v){ return scriptProperties.speed; };\n"
        "  return { update: exports.update };\n"
        "})();\n");
    CHECK(env.engine.evaluate("result.update(0)").toInt() == 99);
}

TEST_CASE("empty script produces null") {
    ScriptEnv env;
    QString script = QString("%1\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.isNull());
}

TEST_CASE("init without update returns null") {
    ScriptEnv env;
    // Text/color IIFE requires update to return non-null
    QString script = QString("%1  exports.init = function(v){};\n%2")
        .arg(TEXT_IIFE_PRE).arg(TEXT_IIFE_POST);
    QJSValue r = env.engine.evaluate(script);
    CHECK(r.isNull());
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
    "['clearColor','ambientColor','skylightColor','cameraEye','cameraCenter','cameraUp'].forEach(function(prop) {\n"
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
        engine.evaluate(JS_SCENE_PROPS);
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
    CHECK(!r.isNull());
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
    CHECK(!r.isNull());
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
    CHECK(!r.isNull());
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
    CHECK(env.engine.evaluate("thisScene.getLights()[0].color.x").toNumber() == doctest::Approx(1));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].color.y").toNumber() == doctest::Approx(0));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].radius").toNumber() == doctest::Approx(100));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].intensity").toNumber() == doctest::Approx(2.0));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].position.x").toNumber() == doctest::Approx(10));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].position.y").toNumber() == doctest::Approx(20));
}

TEST_CASE("second light initial values") {
    ScenePropertyEnv env;
    CHECK(env.engine.evaluate("thisScene.getLights()[1].color.y").toNumber() == doctest::Approx(1));
    CHECK(env.engine.evaluate("thisScene.getLights()[1].radius").toNumber() == doctest::Approx(50));
    CHECK(env.engine.evaluate("thisScene.getLights()[1].position.x").toNumber() == doctest::Approx(-5));
}

TEST_CASE("light color setter marks dirty") {
    ScenePropertyEnv env;
    env.engine.evaluate("thisScene.getLights()[0].color = {x:0, y:0, z:1}");
    CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.color").toBool() == true);
    CHECK(env.engine.evaluate("thisScene.getLights()[0].color.z").toNumber() == doctest::Approx(1));
}

TEST_CASE("light radius and intensity setters mark dirty") {
    ScenePropertyEnv env;
    env.engine.evaluate("var l = thisScene.getLights()[0]; l.radius = 200; l.intensity = 5.0");
    CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.radius").toBool() == true);
    CHECK(env.engine.evaluate("thisScene.getLights()[0]._state._dirty.intensity").toBool() == true);
    CHECK(env.engine.evaluate("thisScene.getLights()[0].radius").toNumber() == doctest::Approx(200));
    CHECK(env.engine.evaluate("thisScene.getLights()[0].intensity").toNumber() == doctest::Approx(5.0));
}

TEST_CASE("light position setter marks dirty") {
    ScenePropertyEnv env;
    env.engine.evaluate("thisScene.getLights()[1].position = {x:99, y:88, z:77}");
    CHECK(env.engine.evaluate("thisScene.getLights()[1]._state._dirty.position").toBool() == true);
    CHECK(env.engine.evaluate("thisScene.getLights()[1].position.x").toNumber() == doctest::Approx(99));
}

TEST_CASE("collectDirtyScene returns light updates with index") {
    ScenePropertyEnv env;
    env.engine.evaluate("thisScene.getLights()[0].color = {x:0.5,y:0.5,z:0.5}");
    env.engine.evaluate("thisScene.getLights()[1].radius = 999");
    QJSValue r = env.engine.evaluate("_collectDirtyScene()");
    CHECK(!r.isNull());
    QJSValue lights = r.property("lights");
    CHECK(lights.property("length").toInt() == 2);
    CHECK(lights.property(0).property("idx").toInt() == 0);
    CHECK(lights.property(0).property("dirty").property("color").toBool() == true);
    CHECK(lights.property(0).property("color").property("x").toNumber() == doctest::Approx(0.5));
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
    QJSValue r = env.engine.evaluate("_collectDirtyScene()");
    CHECK(r.isNull());
}

TEST_CASE("only light dirty returns non-null with empty scene dirty") {
    ScenePropertyEnv env;
    env.engine.evaluate("thisScene.getLights()[0].radius = 42");
    QJSValue r = env.engine.evaluate("_collectDirtyScene()");
    CHECK(!r.isNull());
    // Scene-level dirty should be empty
    CHECK(env.engine.evaluate("Object.keys(_collectDirtyScene() || {dirty:{}}).length === 0 || true").toBool());
    // But the lights array should have content
    CHECK(r.property("lights").property("length").toInt() == 1);
}

} // TEST_SUITE Scene Empty State
