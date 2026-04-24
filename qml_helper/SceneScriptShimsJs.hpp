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

} // namespace wek::qml_helper
