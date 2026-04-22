#pragma once

// JavaScript source for SceneScript built-in modules that production wires
// into the QJSEngine global object.  Extracted into header constants so that
// scenescript_tests can evaluate the exact same code paths as production
// without duplicating the source (drift between production and tests was
// the root cause of the audio/color regression on the Purple Void wallpaper
// — see [feedback_code_quality.md]).

namespace wek::qml_helper
{

// WEColor module (`import * as WEColor from 'WEColor'` in scripts).  Pure
// JS, no engine dependencies.  Used by audio-reactive color scripts via
// WEColor.hsv2rgb to map an audio-driven hue back into an RGB Vec3 that
// the engine pushes onto the material's g_Color4 uniform.
inline constexpr const char* kWEColorJs =
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

// engine.registerAudioBuffers(N) shim.  Returns a buffer object the script
// holds onto; C++ refreshAudioBuffers() walks engine._audioRegs each tick
// and writes per-bin spectrum values into buf.left / buf.right / buf.average.
//
// Resolution is clamped to {16, 32, 64} (the FFT pad sizes the analyzer
// publishes via GetRawSpectrum).  Anything else gets rounded to the nearest
// supported size.
//
// Requires `engine` to already exist on the global object.
inline constexpr const char* kRegisterAudioBuffersJs =
    "(function(resolution) {\n"
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
    "})\n";

} // namespace wek::qml_helper
