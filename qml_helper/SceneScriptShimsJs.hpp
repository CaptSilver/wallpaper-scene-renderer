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

// Vec2 / Vec3 / Vec4 JS classes — the shared source of truth for the three
// linear-algebra shims SceneScripts use.  Extracted from inline JS strings
// in SceneBackend.cpp so the class surface lives in one place; tests still
// ship their own simplified copies under JS_VEC3_AND_UTILS for now (a
// follow-up migrates the test fixtures to consume this shim).
//
// Vec2 is closure-based (fresh methods per instance); Vec3 is prototype-
// based so the ~24 methods + r/g/b accessors are shared across thousands
// of per-frame Vec3 allocations in busy scenes.  Vec4 is closure-based
// (infrequent, simpler).  All three accept "x y z[ w]" strings, another
// Vec, or numeric args; arithmetic overloads are scalar / Vec2 / Vec3 on
// Vec3.{add,subtract,multiply,divide}.
inline constexpr const char* kVecClassesJs = R"JS(
function Vec2(x, y) {
  if (typeof x === 'string') { var p=x.trim().split(/\s+/); x=parseFloat(p[0])||0; y=parseFloat(p[1])||0; }
  else if (x && typeof x === 'object') { y=x.y||0; x=x.x||0; }
  else if (y === undefined) { y = x||0; x = x||0; }
  var v = { x: x||0, y: y||0 };
  v.copy = function() { return Vec2(v.x, v.y); };
  v.length = function() { return Math.sqrt(v.x*v.x+v.y*v.y); };
  v.lengthSqr = function() { return v.x*v.x+v.y*v.y; };
  v.normalize = function() { var l=v.length()||1; return Vec2(v.x/l,v.y/l); };
  v.add = function(o) { return typeof o==='number'? Vec2(v.x+o,v.y+o): Vec2(v.x+o.x, v.y+o.y); };
  v.subtract = function(o) { return typeof o==='number'? Vec2(v.x-o,v.y-o): Vec2(v.x-o.x, v.y-o.y); };
  v.multiply = function(s) { return typeof s==='object'? Vec2(v.x*s.x,v.y*s.y): Vec2(v.x*s,v.y*s); };
  v.divide = function(s) { return typeof s==='object'? Vec2(v.x/s.x,v.y/s.y): Vec2(v.x/s,v.y/s); };
  v.dot = function(o) { return v.x*o.x+v.y*o.y; };
  v.perpendicular = function() { return Vec2(-v.y, v.x); };
  v.reflect = function(n) { var d=2*v.dot(n); return Vec2(v.x-d*n.x, v.y-d*n.y); };
  v.mix = function(o,t) { return Vec2(v.x+(o.x-v.x)*t, v.y+(o.y-v.y)*t); };
  v.equals = function(o) { return v.x===o.x && v.y===o.y; };
  v.toString = function() { return v.x+' '+v.y; };
  v.min = function(o) { return Vec2(Math.min(v.x,o.x),Math.min(v.y,o.y)); };
  v.max = function(o) { return Vec2(Math.max(v.x,o.x),Math.max(v.y,o.y)); };
  v.abs = function() { return Vec2(Math.abs(v.x),Math.abs(v.y)); };
  v.sign = function() { return Vec2(Math.sign(v.x),Math.sign(v.y)); };
  v.round = function() { return Vec2(Math.round(v.x),Math.round(v.y)); };
  v.floor = function() { return Vec2(Math.floor(v.x),Math.floor(v.y)); };
  v.ceil = function() { return Vec2(Math.ceil(v.x),Math.ceil(v.y)); };
  // Make `v instanceof Vec2` true even though Vec2 is closure-based.  WE
  // scripts type-dispatch on `instanceof` (Starscape 3047596375's mixValue
  // does `if (initValue instanceof Vec2)`); without the prototype link the
  // check fails, the function falls into the scalar branch, and tries to
  // set `.x` on a number → TypeError.
  Object.setPrototypeOf(v, Vec2.prototype);
  return v;
}
Vec2.fromString = function(s) { var p=String(s).trim().split(/\s+/); return Vec2(parseFloat(p[0])||0,parseFloat(p[1])||0); };
Vec2.lerp = function(a,b,t) { return Vec2(a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t); };

function Vec3(x, y, z) {
  if (!(this instanceof Vec3)) {
    var v = Object.create(Vec3.prototype);
    Vec3.call(v, x, y, z);
    return v;
  }
  if (typeof x === 'string') {
    var p = x.trim().split(/\s+/);
    this.x = parseFloat(p[0])||0;
    this.y = parseFloat(p[1])||0;
    this.z = parseFloat(p[2])||0;
  } else if (x && typeof x === 'object') {
    this.x = x.x||0; this.y = x.y||0; this.z = x.z||0;
  } else {
    this.x = x||0; this.y = y||0; this.z = z||0;
  }
}
Vec3.prototype.multiply = function(f) {
  if (typeof f === 'number') return Vec3(this.x*f, this.y*f, this.z*f);
  if (f instanceof Vec3)     return Vec3(this.x*f.x, this.y*f.y, this.z*f.z);
  return Vec3(this.x*(f.x||0), this.y*(f.y||0), this.z);
};
Vec3.prototype.add = function(f) {
  if (typeof f === 'number') return Vec3(this.x+f, this.y+f, this.z+f);
  if (f instanceof Vec3)     return Vec3(this.x+f.x, this.y+f.y, this.z+f.z);
  return Vec3(this.x+(f.x||0), this.y+(f.y||0), this.z);
};
Vec3.prototype.subtract = function(f) {
  if (typeof f === 'number') return Vec3(this.x-f, this.y-f, this.z-f);
  if (f instanceof Vec3)     return Vec3(this.x-f.x, this.y-f.y, this.z-f.z);
  return Vec3(this.x-(f.x||0), this.y-(f.y||0), this.z);
};
Vec3.prototype.length    = function() { return Math.sqrt(this.x*this.x+this.y*this.y+this.z*this.z); };
Vec3.prototype.normalize = function() { var l=this.length()||1; return Vec3(this.x/l,this.y/l,this.z/l); };
Vec3.prototype.copy      = function() { return Vec3(this.x, this.y, this.z); };
Vec3.prototype.dot       = function(o) { return this.x*o.x+this.y*o.y+this.z*o.z; };
Vec3.prototype.cross     = function(o) { return Vec3(this.y*o.z-this.z*o.y, this.z*o.x-this.x*o.z, this.x*o.y-this.y*o.x); };
Vec3.prototype.negate    = function() { return Vec3(-this.x,-this.y,-this.z); };
Vec3.prototype.divide = function(f) {
  if (typeof f === 'number') return Vec3(this.x/f, this.y/f, this.z/f);
  if (f instanceof Vec3)     return Vec3(this.x/f.x, this.y/f.y, this.z/f.z);
  return Vec3(this.x/(f.x||1), this.y/(f.y||1), this.z);
};
Vec3.prototype.lerp      = function(o, t) { return Vec3(this.x+(o.x-this.x)*t, this.y+(o.y-this.y)*t, this.z+(o.z-this.z)*t); };
Vec3.prototype.distance  = function(o) { var dx=this.x-o.x,dy=this.y-o.y,dz=this.z-o.z; return Math.sqrt(dx*dx+dy*dy+dz*dz); };
Vec3.prototype.lengthSqr = function() { return this.x*this.x+this.y*this.y+this.z*this.z; };
Vec3.prototype.reflect   = function(n) { var d=2*this.dot(n); return Vec3(this.x-d*n.x, this.y-d*n.y, this.z-d*n.z); };
Vec3.prototype.mix       = function(o,t) { return Vec3(this.x+(o.x-this.x)*t, this.y+(o.y-this.y)*t, this.z+(o.z-this.z)*t); };
Vec3.prototype.equals    = function(o) { return this.x===o.x && this.y===o.y && this.z===o.z; };
Vec3.prototype.toString  = function() { return this.x+' '+this.y+' '+this.z; };
Vec3.prototype.min       = function(o) { return Vec3(Math.min(this.x,o.x),Math.min(this.y,o.y),Math.min(this.z,o.z)); };
Vec3.prototype.max       = function(o) { return Vec3(Math.max(this.x,o.x),Math.max(this.y,o.y),Math.max(this.z,o.z)); };
Vec3.prototype.abs       = function() { return Vec3(Math.abs(this.x),Math.abs(this.y),Math.abs(this.z)); };
Vec3.prototype.sign      = function() { return Vec3(Math.sign(this.x),Math.sign(this.y),Math.sign(this.z)); };
Vec3.prototype.round     = function() { return Vec3(Math.round(this.x),Math.round(this.y),Math.round(this.z)); };
Vec3.prototype.floor     = function() { return Vec3(Math.floor(this.x),Math.floor(this.y),Math.floor(this.z)); };
Vec3.prototype.ceil      = function() { return Vec3(Math.ceil(this.x),Math.ceil(this.y),Math.ceil(this.z)); };
Vec3.prototype.set       = function(x, y, z) { this.x=x||0; this.y=y||0; this.z=z||0; return this; };
Object.defineProperty(Vec3.prototype,'r',{get:function(){return this.x;},set:function(v){this.x=v;},enumerable:true});
Object.defineProperty(Vec3.prototype,'g',{get:function(){return this.y;},set:function(v){this.y=v;},enumerable:true});
Object.defineProperty(Vec3.prototype,'b',{get:function(){return this.z;},set:function(v){this.z=v;},enumerable:true});
Vec3.fromString = function(s) {
  var p = String(s).trim().split(/\s+/);
  return Vec3(parseFloat(p[0])||0, parseFloat(p[1])||0, parseFloat(p[2])||0);
};
Vec3.lerp = function(a, b, t) { return Vec3(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t); };

function Vec4(x, y, z, w) {
  if (typeof x === 'string') { var p=x.trim().split(/\s+/);
    x=parseFloat(p[0])||0; y=parseFloat(p[1])||0; z=parseFloat(p[2])||0;
    w=parseFloat(p[3])||0; }
  else if (x && typeof x === 'object') {
    w=x.w||0; z=x.z||0; y=x.y||0; x=x.x||0; }
  var v = { x: x||0, y: y||0, z: z||0, w: w||0 };
  v.multiply = function(s) { return typeof s==='object'
     ? Vec4(v.x*s.x, v.y*s.y, v.z*s.z, v.w*s.w)
     : Vec4(v.x*s, v.y*s, v.z*s, v.w*s); };
  v.add      = function(o) { return Vec4(v.x+o.x, v.y+o.y, v.z+o.z, v.w+o.w); };
  v.subtract = function(o) { return Vec4(v.x-o.x, v.y-o.y, v.z-o.z, v.w-o.w); };
  v.divide   = function(s) { return typeof s==='object'
     ? Vec4(v.x/s.x, v.y/s.y, v.z/s.z, v.w/s.w)
     : Vec4(v.x/s, v.y/s, v.z/s, v.w/s); };
  v.length = function() { return Math.sqrt(v.x*v.x+v.y*v.y+v.z*v.z+v.w*v.w); };
  v.lengthSqr = function() { return v.x*v.x+v.y*v.y+v.z*v.z+v.w*v.w; };
  v.normalize = function() { var l=v.length()||1; return Vec4(v.x/l,v.y/l,v.z/l,v.w/l); };
  v.copy     = function() { return Vec4(v.x, v.y, v.z, v.w); };
  v.dot      = function(o) { return v.x*o.x+v.y*o.y+v.z*o.z+v.w*o.w; };
  v.negate   = function() { return Vec4(-v.x,-v.y,-v.z,-v.w); };
  v.lerp     = function(o, t) {
    return Vec4(v.x+(o.x-v.x)*t, v.y+(o.y-v.y)*t,
                v.z+(o.z-v.z)*t, v.w+(o.w-v.w)*t); };
  v.distance = function(o) {
    var dx=v.x-o.x,dy=v.y-o.y,dz=v.z-o.z,dw=v.w-o.w;
    return Math.sqrt(dx*dx+dy*dy+dz*dz+dw*dw); };
  v.mix      = function(o, t) { return v.lerp(o, t); };
  v.equals   = function(o) {
    return v.x===o.x && v.y===o.y && v.z===o.z && v.w===o.w; };
  v.toString = function() { return v.x+' '+v.y+' '+v.z+' '+v.w; };
  v.min = function(o) { return Vec4(Math.min(v.x,o.x),Math.min(v.y,o.y),
                                    Math.min(v.z,o.z),Math.min(v.w,o.w)); };
  v.max = function(o) { return Vec4(Math.max(v.x,o.x),Math.max(v.y,o.y),
                                    Math.max(v.z,o.z),Math.max(v.w,o.w)); };
  v.abs = function() { return Vec4(Math.abs(v.x),Math.abs(v.y),
                                   Math.abs(v.z),Math.abs(v.w)); };
  v.sign = function() { return Vec4(Math.sign(v.x),Math.sign(v.y),
                                    Math.sign(v.z),Math.sign(v.w)); };
  v.round = function() { return Vec4(Math.round(v.x),Math.round(v.y),
                                     Math.round(v.z),Math.round(v.w)); };
  v.floor = function() { return Vec4(Math.floor(v.x),Math.floor(v.y),
                                     Math.floor(v.z),Math.floor(v.w)); };
  v.ceil = function() { return Vec4(Math.ceil(v.x),Math.ceil(v.y),
                                    Math.ceil(v.z),Math.ceil(v.w)); };
  Object.defineProperty(v,'r',{get:function(){return v.x;},
                               set:function(val){v.x=val;},enumerable:true});
  Object.defineProperty(v,'g',{get:function(){return v.y;},
                               set:function(val){v.y=val;},enumerable:true});
  Object.defineProperty(v,'b',{get:function(){return v.z;},
                               set:function(val){v.z=val;},enumerable:true});
  Object.defineProperty(v,'a',{get:function(){return v.w;},
                               set:function(val){v.w=val;},enumerable:true});
  // Same instanceof fix as Vec2 above.
  Object.setPrototypeOf(v, Vec4.prototype);
  return v;
}
Vec4.fromString = function(s) {
  var p = String(s).trim().split(/\s+/);
  return Vec4(parseFloat(p[0])||0, parseFloat(p[1])||0,
              parseFloat(p[2])||0, parseFloat(p[3])||0);
};
Vec4.lerp = function(a, b, t) {
  return Vec4(a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t,
              a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t);
};
)JS";

// thisScene.createLayer's helper that applies an object-literal-form asset's
// per-call overrides (origin/scale/angles/alpha/color/visible) onto the rented
// layer proxy.  String / asset-descriptor forms skip the literal branch via
// the `typeof asset === 'object'` guard — without it `'visible' in 'string'`
// throws TypeError (Naruto Shippuden 2800255344's bar script bug, where
// init() crashed at the first createLayer('models/bar.json') call so only
// thisLayer ended up in bars[] and update() then TypeError'd on bars[i] for
// i>0).
inline constexpr const char* kApplyLayerLiteralJs = R"JS(
function _applyLayerLiteral(layer, asset) {
  if (asset && typeof asset === 'object' && !asset.__asset) {
    if (asset.origin) layer.origin = asset.origin;
    if (asset.scale)  layer.scale  = asset.scale;
    if (asset.angles) layer.angles = asset.angles;
    if ('alpha' in asset) layer.alpha = asset.alpha;
    if (asset.color) layer.color = asset.color;
    layer.visible = ('visible' in asset) ? !!asset.visible : true;
  } else {
    layer.visible = true;
  }
}
)JS";

// IMaterial proxy — defines _materialValueCache (per-script) and
// _makeMaterialProxy(layerName).  layerName is captured by closure; the
// proxy delegates writes to __sceneBridge.materialSetValue and reads from
// a JS-side cache so getValue never crosses threads.  Identical code is
// injected into production (SceneBackend.cpp) and the MaterialBridgeEnv
// test fixture; bugs found in tests apply to prod.
//
// Value packing rules:
// - Number             -> [n]
// - Array of numbers   -> filtered to finite, capped at 16 entries
// - Vec2/3/4 instance  -> [x], [x,y], [x,y,z], [x,y,z,w] depending on shape
// - Anything else      -> null (setValue becomes a no-op)
inline constexpr const char* kMaterialProxyJs = R"JS(
var _materialValueCache = {};
function _packMaterialValue(v) {
  if (typeof v === 'number') return isFinite(v) ? [v] : null;
  if (v && typeof v === 'object') {
    if (Array.isArray(v)) {
      var out = [];
      for (var i = 0; i < v.length && out.length < 16; i++) {
        var n = +v[i];
        if (isFinite(n)) out.push(n);
      }
      return out.length ? out : null;
    }
    var pieces = [];
    if (typeof v.x === 'number') pieces.push(v.x);
    if (typeof v.y === 'number') pieces.push(v.y);
    if (typeof v.z === 'number') pieces.push(v.z);
    if (typeof v.w === 'number') pieces.push(v.w);
    return pieces.length ? pieces : null;
  }
  return null;
}
// Property-name → author-facing shader-uniform alias map for direct-
// property writes (material.color = ..., material.channelMask = ..., etc.).
// WE authors use these short names instead of `setValue("g_Color", ...)`.
// Values are author-facing names (no g_ prefix); the C++ drain resolves
// to the actual GLSL uniform per material via SceneMaterialCustomShader::alias
// — that's per-shader so tint.frag's `material.color` lands in g_TintColor
// while other shaders see g_Color.  Pre-this-fix the hardcoded g_Color
// mapping silently dropped on tint/glow/colorize-family shaders.
var _materialPropertyAliases = {
  color:        'color',
  channelMask:  'channelMask',
  channelmask:  'channelMask',
  alpha:        'alpha',
  tint:         'tint'
};
function _makeMaterialProxy(layerName, effectIdx) {
  // effectIdx === undefined or -1 means "main layer material".  Non-negative
  // values route through __sceneBridge.effectMaterialSetValue so the render
  // thread targets the effect chain's per-effect material instead.
  var efx = (typeof effectIdx === 'number' && effectIdx >= 0) ? effectIdx : -1;
  var cacheKey = function(name) {
    return layerName + '|' + (efx < 0 ? '' : ('eff' + efx + '|')) + name;
  };
  var dispatch = function(name, arr) {
    if (efx < 0) {
      if (typeof __sceneBridge !== 'undefined' && __sceneBridge.materialSetValue)
        __sceneBridge.materialSetValue(layerName, name, arr);
    } else {
      if (typeof __sceneBridge !== 'undefined' && __sceneBridge.effectMaterialSetValue)
        __sceneBridge.effectMaterialSetValue(layerName, efx, name, arr);
    }
  };
  var proxy = {
    setValue: function(uName, v) {
      var name = String(uName || '');
      if (!name) return;
      var arr = _packMaterialValue(v);
      if (!arr) return;
      dispatch(name, arr);
      _materialValueCache[cacheKey(name)] = arr;
    },
    getValue: function(uName) {
      var name = String(uName || '');
      var key = cacheKey(name);
      return Object.prototype.hasOwnProperty.call(_materialValueCache, key)
        ? _materialValueCache[key] : null;
    }
  };
  // Direct property accessors for known uniform aliases.  WE wallpapers
  // (Game Of Life 3453251764 et al.) assign `mat.color = Vec3(...)` rather
  // than going through setValue().  Each alias defines a getter/setter that
  // reads/writes through the same dispatch path as setValue, so cache and
  // render thread stay consistent regardless of which spelling the author
  // chose.
  Object.keys(_materialPropertyAliases).forEach(function(prop) {
    var uniformName = _materialPropertyAliases[prop];
    Object.defineProperty(proxy, prop, {
      get: function() {
        var key = cacheKey(uniformName);
        return Object.prototype.hasOwnProperty.call(_materialValueCache, key)
          ? _materialValueCache[key] : null;
      },
      set: function(v) {
        var arr = _packMaterialValue(v);
        if (!arr) return;
        dispatch(uniformName, arr);
        _materialValueCache[cacheKey(uniformName)] = arr;
      },
      enumerable: true,
      configurable: true
    });
  });
  return proxy;
}
function _makeNullMaterialProxy() {
  return {
    setValue: function() {},
    getValue: function() { return null; }
  };
}
)JS";

// =====================================================================
// Layer hierarchy: thisLayer.setParent / getParent / getChildren /
// removeFromParent / getRoot / isAncestorOf / getDepth.
//
// JS-authoritative: each proxy carries a cached `_parent` reference and
// `_children` array.  Reads are synchronous against the cache; writes
// validate cycles JS-side and dispatch via __sceneBridge.setParent into
// Scene::QueueParentChange (drained at the start of RenderHandler::DRAW).
//
// Sound-layer variant stubs every method as a graceful no-op since
// sound layers carry no transform.
// =====================================================================
inline constexpr const char* kHierarchyProxyJs = R"JS(
function _installHierarchyMethods(p) {
  if (typeof p._parent === 'undefined') p._parent = null;
  if (!Array.isArray(p._children)) p._children = [];

  p.getParent = function() { return this._parent; };
  p.getChildren = function() { return this._children.slice(); };
  p.getRoot = function() {
    var cur = this;
    while (cur._parent) cur = cur._parent;
    return cur;
  };
  p.isAncestorOf = function(other) {
    if (!other || typeof other !== 'object') return false;
    var cur = other._parent;
    while (cur) {
      if (cur === this) return true;
      cur = cur._parent;
    }
    return false;
  };
  p.getDepth = function() {
    var d = 0, cur = this;
    while (cur._parent) { d++; cur = cur._parent; }
    return d;
  };

  p.setParent = function(other) {
    if (other === undefined) other = null;
    // Validity: must be null or a layer-shaped object.  Sound proxies
    // stub setParent separately, so non-layer args land here only via
    // user error or test code.
    if (other !== null && (typeof other !== 'object' || !Array.isArray(other._children))) {
      console.log('setParent: ignoring non-layer argument');
      return this;
    }
    // Cycle: refuse self-parent.
    if (other === this) {
      console.log('setParent: cannot parent to self');
      return this;
    }
    // Cycle: refuse parenting under own descendant (would create a loop).
    if (other && this.isAncestorOf(other)) {
      console.log('setParent: cannot parent under own descendant');
      return this;
    }
    // Idempotent: already parented to `other`.
    if (other === this._parent) return this;

    if (this._parent) {
      var idx = this._parent._children.indexOf(this);
      if (idx >= 0) this._parent._children.splice(idx, 1);
    }
    this._parent = other;
    if (other) other._children.push(this);

    var myId = _hierarchyResolveId(this);
    var otherId = other ? _hierarchyResolveId(other) : -1;
    if (myId !== -1 && typeof __sceneBridge !== 'undefined' &&
        typeof __sceneBridge.setLayerParent === 'function') {
      __sceneBridge.setLayerParent(myId, otherId);
    }
    return this;
  };

  p.removeFromParent = function() { return this.setParent(null); };
}

// Sound-layer hierarchy stubs — no transform, so reparenting is a no-op.
// Stubbed methods log + return safe values; matches WE behavior.
function _installSoundHierarchyStubs(p) {
  p.setParent = function() {
    console.log('setParent: sound layer ' + this.name + ' cannot be reparented');
    return this;
  };
  p.removeFromParent = function() { return this; };
  p.getParent = function() { return null; };
  p.getChildren = function() { return []; };
  p.getRoot = function() { return this; };
  p.isAncestorOf = function() { return false; };
  p.getDepth = function() { return 0; };
}

// Resolve a layer proxy to its node id.  Regular proxies stash the id on
// `_state.id`; pool/sound proxies expose it as a plain `id` field.
// Returns -1 when no id is available.
function _hierarchyResolveId(p) {
  if (!p) return -1;
  if (p._state && typeof p._state.id === 'number') return p._state.id;
  if (typeof p.id === 'number') return p.id;
  return -1;
}

// Detach a layer from its parent and orphan all its children, then
// dispatch the corresponding render-thread reset.  Called by
// thisScene.destroyLayer (production + tests) so a recycled pool slot
// returns to the scene root with no stale linkage.
function _detachLayerHierarchy(layer) {
  if (!layer) return;
  if (layer._parent) {
    var pidx = layer._parent._children.indexOf(layer);
    if (pidx >= 0) layer._parent._children.splice(pidx, 1);
    layer._parent = null;
    var myId = _hierarchyResolveId(layer);
    if (myId !== -1 && typeof __sceneBridge !== 'undefined' &&
        typeof __sceneBridge.setLayerParent === 'function') {
      __sceneBridge.setLayerParent(myId, -1);
    }
  }
  if (layer._children && layer._children.length > 0) {
    for (var ci = 0; ci < layer._children.length; ci++) {
      var ch = layer._children[ci];
      ch._parent = null;
      var chId = _hierarchyResolveId(ch);
      if (chId !== -1 && typeof __sceneBridge !== 'undefined' &&
          typeof __sceneBridge.setLayerParent === 'function') {
        __sceneBridge.setLayerParent(chId, -1);
      }
    }
    layer._children = [];
  }
}

// Deferred linkup: walk _layerInitStates and resolve each `pn` reference
// to a proxy in _layerCache.  Skips entries whose names start with `_`
// (reserved metadata like _ortho / _assetPools) and missing proxies
// (sound layers, typos, etc.).  Idempotent — safe to call multiple times.
function _linkupHierarchy() {
  if (typeof _layerInitStates !== 'object' || !_layerInitStates) return;
  for (var name in _layerInitStates) {
    if (!Object.prototype.hasOwnProperty.call(_layerInitStates, name)) continue;
    if (name.charAt(0) === '_') continue;
    var init = _layerInitStates[name];
    if (!init || !init.pn) continue;
    var child  = _layerCache[name];
    var parent = _layerCache[init.pn];
    if (!child || !parent) continue;
    if (child._parent === parent) continue;
    if (child._parent) {
      var idx = child._parent._children.indexOf(child);
      if (idx >= 0) child._parent._children.splice(idx, 1);
    }
    child._parent = parent;
    if (parent._children.indexOf(child) < 0) parent._children.push(child);
  }
}
)JS";


// =====================================================================
// Layer-proxy + thisScene block.  Builds the per-script layer-proxy
// factories and the `thisScene` object that every SceneScript sees.
// Externalized verbatim from SceneBackend.cpp's inline JS so that
// scenescript_tests evaluate the EXACT production proxy code instead of
// a hand-mirrored copy (the JS_LAYER_INFRA mirror had already drifted —
// it lacked the pool fast-path, the _woBaked/_wsBaked world-cache fields,
// and the hierarchy methods; see SceneScriptShimsJs.hpp:3-8 / Purple Void).
//
// Contents (in order):
//   _makePoolLayerProxy(name)  — pool-slot fast path (plain object, no
//                                defineProperty setters) for __pool_ slots.
//   _makeLayerProxy(name)      — full proxy: defineProperty dirty tracking,
//                                _woBaked/_wsBaked world-transform cache,
//                                hierarchy methods (_installHierarchyMethods).
//   _makeNullMaterialProxy     — no-op material proxy for layers w/o material.
//   var thisScene = { ... }    — getLayer, ortho, scene-property surface.
//   thisScene.on / off         — event-bus registration.
//   _overlayScriptProps        — wraps thisLayer with per-script scriptProperties.
//
// Pure JS, no C++ interpolation.  Depends on Vec3, _layerInitStates,
// _sceneOrtho, the material/hierarchy shims, and __sceneBridge being live.
// Evaluation order is kLayerProxyJs -> kApplyLayerLiteralJs -> kLayerRuntimeJs;
// kLayerRuntimeJs's createLayer depends on _applyLayerLiteral.
// =====================================================================
inline constexpr const char* kLayerProxyJs = R"JS(var _layerCache = {};
var _layerList = [];
function _makePoolLayerProxy(name) {
  var init = _layerInitStates[name];
  var _origin = init ? Vec3(init.o[0], init.o[1], init.o[2]) : Vec3(0,0,0);
  var _scale  = init ? Vec3(init.s[0], init.s[1], init.s[2]) : Vec3(1,1,1);
  var _angles = init ? Vec3(init.a[0], init.a[1], init.a[2]) : Vec3(0,0,0);
  var p = {
    _pool: true,
    name: name,
    alpha: 1.0, visible: init ? init.v : true,
    color: Vec3(1,1,1), text: '', pointsize: 0,
    __asset: null,
    _destroyed: false,
    isObjectValid: function() { return !this._destroyed; },
    getInitialLayerConfig: function() {
      return {
        origin:  init ? Vec3(init.o[0], init.o[1], init.o[2]) : Vec3(0,0,0),
        scale:   init ? Vec3(init.s[0], init.s[1], init.s[2]) : Vec3(1,1,1),
        angles:  init ? Vec3(init.a[0], init.a[1], init.a[2]) : Vec3(0,0,0),
        alpha:   1.0,
        visible: init ? init.v : true,
        color:   Vec3(1, 1, 1),
        size:    init && init.sz ? {x: init.sz[0], y: init.sz[1]}
                                  : {x: 0, y: 0}
      };
    }
  };
  Object.defineProperty(p, 'origin', {
    get: function(){ return Vec3(_origin.x, _origin.y, _origin.z); },
    set: function(v){
      _origin = v ? Vec3(v.x||0, v.y||0, v.z||0) : Vec3(0,0,0);
    }, enumerable: true, configurable: true });
  Object.defineProperty(p, 'scale', {
    get: function(){ return Vec3(_scale.x, _scale.y, _scale.z); },
    set: function(v){
      _scale = v ? Vec3(v.x||0, v.y||0, v.z||0) : Vec3(1,1,1);
    }, enumerable: true, configurable: true });
  Object.defineProperty(p, 'angles', {
    get: function(){ return Vec3(_angles.x, _angles.y, _angles.z); },
    set: function(v){
      _angles = v ? Vec3(v.x||0, v.y||0, v.z||0) : Vec3(0,0,0);
    }, enumerable: true, configurable: true });
  p._prev = { ox: p.origin.x, oy: p.origin.y, oz: p.origin.z,
              sx: p.scale.x, sy: p.scale.y, sz: p.scale.z,
              ax: p.angles.x, ay: p.angles.y, az: p.angles.z,
              alpha: p.alpha, visible: p.visible };
  if (typeof _layerNameToId !== 'undefined' && _layerNameToId[name] !== undefined) {
    p.id = _layerNameToId[name];
  }
  _installHierarchyMethods(p);
  return p;
}
function _makeLayerProxy(name) {
  if (name.charCodeAt(0) === 95 && name.charCodeAt(1) === 95 &&
      name.charCodeAt(2) === 112 && name.charCodeAt(3) === 111 &&
      name.charCodeAt(4) === 111 && name.charCodeAt(5) === 108) {
    return _makePoolLayerProxy(name);
  }
  var init = _layerInitStates[name];
  var _s = init ? {
    origin: Vec3(init.o[0], init.o[1], init.o[2]),
    scale:  Vec3(init.s[0], init.s[1], init.s[2]),
    angles: Vec3(init.a[0], init.a[1], init.a[2]),
    size: init.sz ? {x:init.sz[0], y:init.sz[1]} : {x:0, y:0},
    parallaxDepth: init.pd ? {x:init.pd[0], y:init.pd[1]} : {x:0, y:0},
    _woBaked: init.wo ? Vec3(init.wo[0], init.wo[1], init.wo[2]) : null,
    _wsBaked: init.ws ? Vec3(init.ws[0], init.ws[1], init.ws[2]) : null,
    visible: init.v, alpha: 1.0,
    text: '', pointsize: init.ps || 0,
    name: name, _dirty: {}, _cmds: []
  } : { origin: Vec3(0,0,0), scale: Vec3(1,1,1),
        angles: Vec3(0,0,0), size: {x:0, y:0},
        parallaxDepth: {x:0, y:0},
        _woBaked: null, _wsBaked: null,
        visible: true, alpha: 1.0,
        text: '', pointsize: 0,
        name: name, _dirty: {}, _cmds: [] };
  Object.defineProperty(_s, 'worldOrigin', {
    get: function() {
      if (typeof __sceneBridge !== 'undefined' && __sceneBridge.getLayerWorldTransform) {
        var m = __sceneBridge.getLayerWorldTransform(_s.name);
        if (m && m.length === 16 && Number.isFinite(+m[12]) && Number.isFinite(+m[13])) {
          var tx = +m[12], ty = +m[13];
          if (tx !== 0 || ty !== 0) return Vec3(tx, ty, +m[14]||0);
        }
      }
      return _s._woBaked;
    }, enumerable: true, configurable: true });
  Object.defineProperty(_s, 'worldScale', {
    get: function() {
      if (typeof __sceneBridge !== 'undefined' && __sceneBridge.getLayerWorldTransform) {
        var m = __sceneBridge.getLayerWorldTransform(_s.name);
        if (m && m.length === 16 && Number.isFinite(+m[0]) && Number.isFinite(+m[5])) {
          var sx = Math.sqrt((+m[0])*(+m[0]) + (+m[1])*(+m[1]) + (+m[2])*(+m[2]));
          var sy = Math.sqrt((+m[4])*(+m[4]) + (+m[5])*(+m[5]) + (+m[6])*(+m[6]));
          var sz = Math.sqrt((+m[8])*(+m[8]) + (+m[9])*(+m[9]) + (+m[10])*(+m[10]));
          if (sx > 0 && sy > 0) return Vec3(sx, sy, sz);
        }
      }
      return _s._wsBaked;
    }, enumerable: true, configurable: true });
  var p = {};
  Object.defineProperty(p, 'name', { get: function(){return _s.name;}, enumerable:true });
  Object.defineProperty(p, 'debug', { get: function(){return undefined;}, enumerable:true });
  var vec3Props = ['origin','scale','angles'];
  for (var i=0; i<vec3Props.length; i++) {
    (function(prop){
      Object.defineProperty(p, prop, {
        get: function(){ var s = _s[prop]; return Vec3(s.x, s.y, s.z); },
        set: function(v){
          if (!v) { _s[prop] = Vec3(0,0,0); }
          else    { _s[prop] = Vec3(v.x||0, v.y||0, v.z||0); }
          _s._dirty[prop] = true; },
        enumerable: true
      });
    })(vec3Props[i]);
  }
  var _origO = init ? {x:init.o[0], y:init.o[1], z:init.o[2]} : {x:0,y:0,z:0};
  var _origS = init ? {x:init.s[0], y:init.s[1], z:init.s[2]} : {x:1,y:1,z:1};
  var _origA = init ? {x:init.a[0], y:init.a[1], z:init.a[2]} : {x:0,y:0,z:0};
  Object.defineProperty(p, 'originalOrigin', {
    get: function(){ return Vec3(_origO.x, _origO.y, _origO.z); },
    set: function(){},
    enumerable: true });
  Object.defineProperty(p, 'originalScale', {
    get: function(){ return Vec3(_origS.x, _origS.y, _origS.z); },
    set: function(){},
    enumerable: true });
  Object.defineProperty(p, 'originalAngles', {
    get: function(){ return Vec3(_origA.x, _origA.y, _origA.z); },
    set: function(){},
    enumerable: true });
  p.getInitialLayerConfig = function() {
    return {
      origin:  Vec3(_origO.x, _origO.y, _origO.z),
      scale:   Vec3(_origS.x, _origS.y, _origS.z),
      angles:  Vec3(_origA.x, _origA.y, _origA.z),
      alpha:   1.0,
      visible: init ? init.v : true,
      color:   Vec3(1, 1, 1),
      size:    init && init.sz ? {x: init.sz[0], y: init.sz[1]}
                                : {x: 0, y: 0}
    };
  };
  var scalarProps = ['visible','alpha'];
  for (var j=0; j<scalarProps.length; j++) {
    (function(prop){
      Object.defineProperty(p, prop, {
        get: function(){ return _s[prop]; },
        set: function(v){ _s[prop] = v; _s._dirty[prop] = true; },
        enumerable: true
      });
    })(scalarProps[j]);
  }
  Object.defineProperty(p, 'text', {
    get: function(){ return _s.text; },
    set: function(v){ _s.text = v; _s._dirty.text = true; },
    enumerable: true
  });
  Object.defineProperty(p, 'pointsize', {
    get: function(){ return _s.pointsize; },
    set: function(v){ _s.pointsize = v; _s._dirty.pointsize = true;
                      _s._dirty.text = true; },
    enumerable: true
  });
  Object.defineProperty(p, 'size', {
    get: function(){ return _s.size; },
    enumerable: true
  });
  if (init && init.halign && _s.halign === undefined) _s.halign = init.halign;
  if (init && init.valign && _s.valign === undefined) _s.valign = init.valign;
  if (init && init.font   && _s.font   === undefined) _s.font   = init.font;
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
  Object.defineProperty(p, 'opacity', {
    get: function(){ return _s.alpha; },
    set: function(v){ _s.alpha = v; _s._dirty.alpha = true; },
    enumerable: true
  });
  Object.defineProperty(p, 'solid', {
    get: function(){ return _s.solid === false ? false : true; },
    set: function(v){ _s.solid = !!v; },
    enumerable: true
  });
  p.play = function(){};
  p.stop = function(){};
  p.pause = function(){};
  p.isPlaying = function(){ return false; };
  p.getTextureAnimation = function(){
    var _name = _s.name;
    var _readInfo = function() {
      var info = (typeof __sceneBridge !== 'undefined'
                  && __sceneBridge.getLayerSpriteInfo)
                 ? __sceneBridge.getLayerSpriteInfo(_name) : null;
      var fc = info && info.frameCount ? info.frameCount : 1;
      var cf = info && typeof info.currentFrame === 'number' ? info.currentFrame : 0;
      var dur = info && typeof info.duration === 'number' ? info.duration : 0;
      var mp = info ? !!info.isManualPin : false;
      return { frameCount: fc, currentFrame: cf, duration: dur, isManualPin: mp };
    };
    return {
      get frameCount(){ return _readInfo().frameCount; },
      get duration(){ return _readInfo().duration; },
      get rate(){ return _readInfo().isManualPin ? 0 : 1; },
      // Writable: WE scripts set rate=0 to pause the sprite animation and
      // rate>0 to play it (e.g. a play/pause button icon morph in 2992803622's
      // music player).  Without a setter, `layer.rate = N` throws a TypeError
      // under 'use strict' (assignment to getter-only), aborting the whole
      // update().  Map to the manual-pin bridge: >0 = play (unpinned),
      // <=0 = pause at the current frame.
      set rate(v){
        if (typeof __sceneBridge === 'undefined' || !__sceneBridge.setLayerSpriteFrame) return;
        if (v > 0) __sceneBridge.setLayerSpriteFrame(_name, false, 0);
        else __sceneBridge.setLayerSpriteFrame(_name, true, _readInfo().currentFrame);
      },
      get _frame(){ return _readInfo().currentFrame; },
      getFrame: function(){ return _readInfo().currentFrame; },
      setFrame: function(f){
        if (typeof __sceneBridge !== 'undefined' && __sceneBridge.setLayerSpriteFrame)
          __sceneBridge.setLayerSpriteFrame(_name, true, f|0); },
      play: function(){
        if (typeof __sceneBridge !== 'undefined' && __sceneBridge.setLayerSpriteFrame)
          __sceneBridge.setLayerSpriteFrame(_name, false, 0); },
      pause: function(){
        var cur = _readInfo().currentFrame;
        if (typeof __sceneBridge !== 'undefined' && __sceneBridge.setLayerSpriteFrame)
          __sceneBridge.setLayerSpriteFrame(_name, true, cur); },
      stop: function(){
        if (typeof __sceneBridge !== 'undefined' && __sceneBridge.setLayerSpriteFrame)
          __sceneBridge.setLayerSpriteFrame(_name, true, 0); },
      isPlaying: function(){ return !_readInfo().isManualPin; }
    };
  };
  p.getVideoTexture = function() {
    var name = _s.name;
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
  };
  p.getMaterial = function() { return _makeMaterialProxy(_s.name); };
  p.getTransformMatrix = function() {
    var arr = __sceneBridge.getLayerWorldTransform(_s.name);
    var mm = new Mat4();
    if (arr && arr.length === 16) {
      for (var i = 0; i < 16; i++) mm.m[i] = +arr[i] || 0;
    }
    return mm;
  };
  p.getBoneIndex = function(bname) {
    return __sceneBridge.getBoneIndex(_s.name, String(bname == null ? '' : bname)) | 0;
  };
  if (!_s._aniLayers) _s._aniLayers = {};
  p.getAnimationLayerCount = function(){ return Object.keys(_s._aniLayers).length || 1; };
  p.getAnimationLayer = function(idx) {
    var key = String(idx);
    if (_s._aniLayers[key]) return _s._aniLayers[key];
    var al = { rate: 1, blend: 1, visible: true, frameCount: 60, _frame: 0, _playing: false,
      play: function(){ al._playing = true; },
      pause: function(){ al._playing = false; },
      stop: function(){ al._playing = false; al._frame = 0; },
      isPlaying: function(){ return al._playing; },
      getFrame: function(){ return al._frame; },
      setFrame: function(f){ al._frame = f; }
    };
    _s._aniLayers[key] = al;
    return al;
  };
  if (!_s._namedAnims) _s._namedAnims = {};
  p.getAnimation = function(animName) {
    if (_s._namedAnims[animName]) return _s._namedAnims[animName];
    var ctrl = { _playing: false, _name: animName,
      play:  function(){ this._playing = true;  _s._cmds.push('panim_play:'  + animName); _s._dirty._cmds = true; },
      pause: function(){ this._playing = false; _s._cmds.push('panim_pause:' + animName); _s._dirty._cmds = true; },
      stop:  function(){ this._playing = false; _s._cmds.push('panim_stop:'  + animName); _s._dirty._cmds = true; },
      isPlaying: function(){ return this._playing; }
    };
    _s._namedAnims[animName] = ctrl;
    return ctrl;
  };
  if (!_s._effCache) _s._effCache = {};
  p.getEffect = function(ename) {
    var efxList = init ? init.efx : null;
    if (!efxList) return null;
    var idx = -1;
    if (typeof ename === 'number') {
      if (ename >= 0 && ename < efxList.length) { idx = ename|0; ename = efxList[idx]; }
    } else {
      for (var i = 0; i < efxList.length; i++) {
        if (efxList[i] === ename) { idx = i; break; }
      }
    }
    if (idx < 0) return null;
    if (_s._effCache[ename]) return _s._effCache[ename];
    var es = { visible: true, name: ename, _idx: idx };
    var ep = {};
    Object.defineProperty(ep, 'name', { get: function(){ return es.name; }, enumerable: true });
    Object.defineProperty(ep, 'visible', {
      get: function(){ return es.visible; },
      set: function(v){ es.visible = v;
        if (!_s._efxDirty) _s._efxDirty = [];
        _s._efxDirty.push(idx, v ? 1 : 0); },
      enumerable: true
    });
    ep.getMaterial = function() {
      return _makeMaterialProxy(_s.name, idx);
    };
    _s._effCache[ename] = ep;
    return ep;
  };
  p.getEffectCount = function() {
    return (init && init.efx) ? init.efx.length : 0;
  };
  p._destroyed = false;
  p.isObjectValid = function() { return !this._destroyed; };
  p._state = _s;
  if (typeof _layerNameToId !== 'undefined' && _layerNameToId[name] !== undefined) {
    _s.id = _layerNameToId[name];
  }
  _installHierarchyMethods(p);
  return p;
}
var _nullProxy = (function() {
  var _s = { origin:Vec3(0,0,0), scale:Vec3(1,1,1),
    angles:Vec3(0,0,0), size:{x:0,y:0},
    visible:false, alpha:0, text:'', name:'', _dirty:{} };
  var p = {};
  Object.defineProperty(p, 'name', {get:function(){return '';}, enumerable:true});
  Object.defineProperty(p, 'debug', {get:function(){return undefined;}, enumerable:true});
  var vec3Props = ['origin','scale','angles'];
  for (var i=0; i<vec3Props.length; i++) {
    (function(prop){
      Object.defineProperty(p, prop, {
        get: function(){ var s = _s[prop]; return Vec3(s.x, s.y, s.z); },
        set: function(v){},
        enumerable: true
      });
    })(vec3Props[i]);
  }
  var scalarProps = ['visible','alpha'];
  for (var j=0; j<scalarProps.length; j++) {
    (function(prop){
      Object.defineProperty(p, prop, {
        get: function(){return _s[prop];}, set: function(v){},
        enumerable: true
      });
    })(scalarProps[j]);
  }
  Object.defineProperty(p, 'text', {get:function(){return '';}, set:function(v){}, enumerable:true});
  Object.defineProperty(p, 'size', {get:function(){return _s.size;}, enumerable:true});
  p.play = function(){}; p.stop = function(){};
  p.pause = function(){}; p.isPlaying = function(){return false;};
  p.getTextureAnimation = function(){
    return { rate:0, frameCount:1, _frame:0,
      getFrame:function(){return this._frame;}, setFrame:function(f){},
      play:function(){}, pause:function(){}, stop:function(){},
      isPlaying:function(){return false;}
    };
  };
  p.getVideoTexture = function() {
    var name = _s.name;
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
  };
  p.getAnimationLayerCount = function(){ return 0; };
  p.getAnimationLayer = function(idx){
    return { rate:0, blend:0, visible:false, frameCount:0, _frame:0, _playing:false,
      play:function(){}, pause:function(){}, stop:function(){},
      isPlaying:function(){return false;}, getFrame:function(){return 0;}, setFrame:function(f){}
    };
  };
  p.getAnimation = function(animName){
    return { _playing: false, _name: animName,
      play:function(){}, pause:function(){}, stop:function(){},
      isPlaying:function(){return false;}
    };
  };
  p.getEffect = function(name) { return { name: name||'', visible: false }; };
  p.getEffectCount = function() { return 0; };
  p.getMaterial = function() { return _makeNullMaterialProxy(); };
  p.isObjectValid = function() { return true; };
  p._state = _s;
  return p;
})();
var thisScene = {
  getLayer: function(name) {
    if (_layerCache[name]) return _layerCache[name];
    if (!_layerInitStates[name]) return null;
    var layer = _makeLayerProxy(name);
    _layerCache[name] = layer;
    _layerList.push(layer);
    return layer;
  },
  getInitialLayerConfig: function(layer) {
    if (!layer || typeof layer.getInitialLayerConfig !== 'function') return null;
    var cfg = layer.getInitialLayerConfig();
    var efx = layer._state && layer._state._initEfx;
    if (!efx) {
      var st = layer._state && layer._state.name && _layerInitStates[layer._state.name];
      if (st && st.efx) efx = st.efx;
    }
    if (efx && efx.length) {
      cfg.effects = [];
      for (var i = 0; i < efx.length; i++) cfg.effects.push({ name: efx[i] });
    } else { cfg.effects = []; }
    return cfg;
  }
};
var _sceneListeners = {};
thisScene.on = function(eventName, callback) {
  if (typeof eventName !== 'string' || typeof callback !== 'function') return;
  if (!_sceneListeners[eventName]) _sceneListeners[eventName] = [];
  _sceneListeners[eventName].push(callback);
};
thisScene.off = function(eventName, callback) {
  if (!_sceneListeners[eventName]) return;
  if (typeof callback === 'function') {
    _sceneListeners[eventName] = _sceneListeners[eventName].filter(
      function(cb) { return cb !== callback; });
  } else { delete _sceneListeners[eventName]; }
};
function _fireSceneEvent(eventName) {
  var listeners = _sceneListeners[eventName];
  if (!listeners || listeners.length === 0) return 0;
  var args = Array.prototype.slice.call(arguments, 1);
  var count = 0;
  for (var i = 0; i < listeners.length; i++) {
    try { listeners[i].apply(null, args); count++; }
    catch(e) { console.log('scene.on(' + eventName + ') error: ' + e.message); }
  }
  return count;
}
function _hasSceneListeners(eventName) {
  var l = _sceneListeners[eventName];
  return l && l.length > 0;
}
var scene = thisScene;
var thisLayer = null;
function _overlayScriptProps(layer, sp) {
  if (!sp || typeof sp !== 'object') return layer;
  var w = layer ? Object.create(layer) : {};
  for (var k in sp) if (Object.prototype.hasOwnProperty.call(sp, k)) {
    (function(key) {
      Object.defineProperty(w, key, {
        get: function() { return sp[key]; },
        set: function(v) { sp[key] = v; },
        enumerable: true, configurable: true
      });
    })(k);
  }
  return w;
}
)JS";

// =====================================================================
// Layer-runtime block.  thisScene.createLayer / destroyLayer, the pool
// rent/return machinery, and _collectDirtyLayers (the stride-17 flat-array
// dirty collector C++ calls each tick via m_collectDirtyLayersFn).
// Externalized verbatim from SceneBackend.cpp's inline JS; shared with
// scenescript_tests.  MUST be evaluated AFTER kLayerProxyJs and
// kApplyLayerLiteralJs (createLayer calls _applyLayerLiteral and
// _makeLayerProxy).  Pure JS, depends on engine._assetPools/_assetLive,
// _layerCache/_layerList, the hierarchy shims, and __sceneBridge.
// =====================================================================
inline constexpr const char* kLayerRuntimeJs = R"JS(thisScene.createLayer = function(asset) {
  var path = (typeof asset === 'string') ? asset
             : (asset && (asset.__asset || asset.image));
  var pool = path && engine._assetPools[path];
  if (!engine._assetLive[path]) engine._assetLive[path] = [];
  var live = engine._assetLive[path];
  var name = null;
  if (pool && pool.length > 0) {
    name = pool.shift();
  } else if (live.length > 0) {
    name = live.shift();
    console.log('createLayer LRU-recycle: path=' + path + ' name=' + name);
  }
  if (name) {
    var layer = thisScene.getLayer(name);
    if (layer && layer !== _nullProxy) {
      layer._destroyed = false;
      layer.__asset = path;
      _applyLayerLiteral(layer, asset);
      live.push(name);
      console.log('createLayer OK: path=' + path + ' name=' + name +
                  ' pool.free=' + (pool ? pool.length : 0) +
                  ' live=' + live.length);
      return layer;
    }
    console.log('createLayer: pool layer ' + name + ' not a real proxy');
  }
  if (path === undefined || path === null || path === '') {
    return { __stub: true, __asset: '',
      origin: Vec3(0,0,0), scale: Vec3(1,1,1), angles: Vec3(0,0,0),
      alpha: 1.0, visible: true, text: '',
      isObjectValid: function() { return false; },
      getAnimation: function(n) {
        return { play:function(){}, stop:function(){}, pause:function(){},
                 isPlaying:function(){ return false; } }; } };
  }
  console.log('createLayer NO-POOL: path=' + path +
              ' (pre-scan did not register this asset)');
  return { __stub: true, __asset: path,
    origin: Vec3(0,0,0), scale: Vec3(1,1,1), angles: Vec3(0,0,0),
    alpha: 1.0, visible: true, text: '',
    isObjectValid: function() { return false; },
    getAnimation: function(n) {
      return { play:function(){}, stop:function(){}, pause:function(){},
               isPlaying:function(){ return false; } }; } };
};
thisScene.destroyLayer = function(layer) {
  if (!layer || layer.__stub) return;
  layer.visible = false;
  layer._destroyed = true;
  if (typeof _detachLayerHierarchy === 'function') _detachLayerHierarchy(layer);
  var path = layer.__asset;
  if (!path) return;
  var live = engine._assetLive[path];
  if (live && layer.name) {
    var idx = live.indexOf(layer.name);
    if (idx >= 0) live.splice(idx, 1);
  }
  if (engine._assetPools[path] && layer.name)
    engine._assetPools[path].push(layer.name);
};
thisScene.sortLayer    = function(layer, index) {
  if (!layer || layer._destroyed) return;
  var id = (typeof layer === 'number') ? layer : layer.id;
  if (typeof id !== 'number') return;
  if (typeof index !== 'number') return;
  if (typeof __sceneBridge !== 'undefined' && __sceneBridge.sortLayer)
    __sceneBridge.sortLayer(id|0, index|0);
};
thisScene.getLayerIndex = function(name) {
  var l = thisScene.getLayer(name);
  return l && typeof l._index === 'number' ? l._index : 0;
};
var DIRTY_STRIDE = 17;
var F_ORIGIN=1, F_SCALE=2, F_ANGLES=4, F_VISIBLE=8, F_ALPHA=16,
    F_TEXT=32, F_PSIZE=64, F_CMDS=128, F_EFX=256;
function _collectDirtyLayers() {
  var out = [];
  var list = _layerList;
  for (var li = 0, ln = list.length; li < ln; li++) {
    var layer = list[li];
    var name = layer.name;
    if (layer._pool) {
      var prev = layer._prev;
      var o = layer.origin, sc = layer.scale, a = layer.angles;
      var flags = 0;
      if (o.x !== prev.ox || o.y !== prev.oy || o.z !== prev.oz)
        { flags |= F_ORIGIN; prev.ox = o.x; prev.oy = o.y; prev.oz = o.z; }
      if (sc.x !== prev.sx || sc.y !== prev.sy || sc.z !== prev.sz)
        { flags |= F_SCALE; prev.sx = sc.x; prev.sy = sc.y; prev.sz = sc.z; }
      if (a.x !== prev.ax || a.y !== prev.ay || a.z !== prev.az)
        { flags |= F_ANGLES; prev.ax = a.x; prev.ay = a.y; prev.az = a.z; }
      if (layer.alpha !== prev.alpha)
        { flags |= F_ALPHA; prev.alpha = layer.alpha; }
      if (layer.visible !== prev.visible)
        { flags |= F_VISIBLE; prev.visible = layer.visible; }
      if (flags === 0) continue;
      // Spec 10 — emit the resolved integer id (set at proxy creation from
      // _layerNameToId) in slot 0 so the C++ hot loop reads an int directly,
      // with no per-tick QString->std::string + nodeNameToId lookup.  -1 =
      // unresolved (the C++ side counts it a miss; the name is reported once at
      // first flush by scanning _layerList).
      var pid = (typeof layer.id === 'number') ? layer.id : -1;
      out.push(
        pid, flags,
        o.x, o.y, o.z,
        sc.x, sc.y, sc.z,
        a.x, a.y, a.z,
        layer.alpha, layer.visible ? 1 : 0, 0,
        null, null, null);
      continue;
    }
    var s = layer._state;
    var d = s._dirty;
    var cmds = s._cmds;
    var efxList = s._efxDirty;
    var flags = 0;
    if (d.origin)    flags |= F_ORIGIN;
    if (d.scale)     flags |= F_SCALE;
    if (d.angles)    flags |= F_ANGLES;
    if (d.visible)   flags |= F_VISIBLE;
    if (d.alpha)     flags |= F_ALPHA;
    if (d.text)      flags |= F_TEXT;
    if (d.pointsize) flags |= F_PSIZE;
    if (cmds && cmds.length) flags |= F_CMDS;
    if (efxList && efxList.length) flags |= F_EFX;
    if (flags === 0) continue;
    var o = s.origin, sc = s.scale, a = s.angles;
    // Spec 10 — resolved id in slot 0 (echoed onto _state.id at proxy creation).
    var rid = (typeof s.id === 'number') ? s.id : -1;
    out.push(
      rid, flags,
      o.x, o.y, o.z,
      sc.x, sc.y, sc.z,
      a.x, a.y, a.z,
      s.alpha, s.visible ? 1 : 0, s.pointsize,
      (flags & F_TEXT) ? s.text : null,
      (flags & F_CMDS) ? cmds.slice() : null,
      (flags & F_EFX) ? efxList.slice() : null);
    s._dirty = {};
    if (flags & F_CMDS) s._cmds = [];
    if (flags & F_EFX)  s._efxDirty = [];
  }
  return out;
}
)JS";
} // namespace wek::qml_helper
