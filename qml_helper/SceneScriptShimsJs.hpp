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

// Mat3 / Mat4 JS classes — transforms that SceneScripts can read/write.
// Without these, any `new Mat4()` call in a script ReferenceError's silently.
//
// Mat4 is a 4x4 with m[12..14] carrying the translation column.
// `translation(v)` is overloaded: call with a Vec3/Vec2 to set (returns this
// for chaining), call with no args to read it back as a Vec3.  right/up/
// forward return the basis vectors from the rotation block.
//
// Mat3 is 3x3 with m[6..7] carrying a 2D translation.  `angle()` returns
// degrees via atan2(m[0], -m[1]) on the first column.
//
// Requires Vec2 and Vec3 to already exist on the global object.
inline constexpr const char* kMatricesJs =
    "function Mat4() { this.m = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]; }\n"
    "Mat4.prototype.translation = function(v) {\n"
    "  if (v && typeof v === 'object' && 'z' in v) {\n"
    "    this.m[12]=v.x; this.m[13]=v.y; this.m[14]=v.z; return this;\n"
    "  } else if (v && typeof v === 'object') {\n"
    "    this.m[12]=v.x; this.m[13]=v.y; this.m[14]=0; return this;\n"
    "  }\n"
    "  return Vec3(this.m[12], this.m[13], this.m[14]);\n"
    "};\n"
    "Mat4.prototype.right    = function() { return Vec3(this.m[0], this.m[1], this.m[2]); };\n"
    "Mat4.prototype.up       = function() { return Vec3(this.m[4], this.m[5], this.m[6]); };\n"
    "Mat4.prototype.forward  = function() { return Vec3(this.m[8], this.m[9], this.m[10]); };\n"
    "Mat4.prototype.toString = function() { return this.m.join(' '); };\n"
    "Mat4.prototype.toJSONString = function() { return this.toString(); };\n"
    "function Mat3() { this.m = [1,0,0, 0,1,0, 0,0,1]; }\n"
    "Mat3.prototype.translation = function(v) {\n"
    "  if (v && typeof v === 'object') {\n"
    "    this.m[6]=v.x; this.m[7]=v.y; return this;\n"
    "  }\n"
    "  return Vec2(this.m[6], this.m[7]);\n"
    "};\n"
    "Mat3.prototype.angle    = function() {\n"
    "  return Math.atan2(this.m[0], -this.m[1]) * (180.0 / Math.PI);\n"
    "};\n"
    "Mat3.prototype.toString = function() { return this.m.join(' '); };\n"
    "Mat3.prototype.toJSONString = function() { return this.toString(); };\n";

// `_Internal` namespace — a few helpers wallpaper scripts may poke at.
// These wrap familiar patterns (writing script properties, coercing user-
// property JSON, stringifying configs with a toJSONString adapter) behind
// the names scripts expect.  Pure JS, depends on Vec3 being defined.
inline constexpr const char* kInternalNamespaceJs =
    // Coerce a Vec3-shaped input (string "x y z", {x,y,z}, or Vec3) to a
    // fresh Vec3, parsing the string form ourselves so we don't depend on
    // the hosting Vec3 constructor's string handling.
    "function _toVec3(v) {\n"
    "  if (typeof v === 'string') {\n"
    "    var p = v.split(/\\s+/);\n"
    "    return new Vec3(parseFloat(p[0])||0,\n"
    "                    parseFloat(p[1])||0,\n"
    "                    parseFloat(p[2])||0);\n"
    "  }\n"
    "  if (v && typeof v === 'object') {\n"
    "    return new Vec3(v.x||0, v.y||0, v.z||0);\n"
    "  }\n"
    "  return new Vec3(0, 0, 0);\n"
    "}\n"
    "function _isVec3Shape(v) {\n"
    "  return v && typeof v.x === 'number' && typeof v.y === 'number'\n"
    "           && typeof v.z === 'number';\n"
    "}\n"
    "var _Internal = {\n"
    "  updateScriptProperties: function(script, vars) {\n"
    "    if (typeof vars === 'string') vars = JSON.parse(vars);\n"
    "    if (!script || !script.scriptProperties) return;\n"
    "    var sp = script.scriptProperties;\n"
    "    for (var key in vars) {\n"
    "      if (!Object.prototype.hasOwnProperty.call(sp, key)) continue;\n"
    "      if (_isVec3Shape(sp[key])) {\n"
    "        sp[key] = _toVec3(vars[key]);\n"
    "      } else {\n"
    "        sp[key] = vars[key];\n"
    "      }\n"
    "    }\n"
    "  },\n"
    "  convertUserProperties: function(p) {\n"
    "    if (typeof p === 'string') p = JSON.parse(p);\n"
    "    var r = {};\n"
    "    for (var k in p) {\n"
    "      var t = p[k] ? p[k].type : undefined;\n"
    "      if (t === 'color') {\n"
    "        r[k] = _toVec3(p[k].value);\n"
    "      } else if (t === 'usershortcut') {\n"
    "        r[k] = { isbound: p[k].isbound,\n"
    "                 commandtype: p[k].commandtype,\n"
    "                 file: p[k].file };\n"
    "      } else {\n"
    "        r[k] = p[k] ? p[k].value : undefined;\n"
    "      }\n"
    "    }\n"
    "    return r;\n"
    "  },\n"
    "  stringifyConfig: function(obj) {\n"
    "    return JSON.stringify(obj, function(key, value) {\n"
    "      if (value && typeof value.toJSONString === 'function') {\n"
    "        return value.toJSONString();\n"
    "      }\n"
    "      return value;\n"
    "    });\n"
    "  }\n"
    "};\n";

} // namespace wek::qml_helper
