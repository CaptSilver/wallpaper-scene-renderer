#pragma once

// JavaScript source for the batched property-script dispatch loop.  Kept here
// as a header-only constant so the scenescript_tests can evaluate the exact
// same code path as production (no drift).
//
// The loop processes three partitions of `_allPropertyScripts`:
//   [0, visEnd)   — Kind::Visible (boolean return)
//   [visEnd, vec3End) — Kind::Vec3 (Vec3-or-scalar return; scalar broadcasts)
//   [vec3End, N)  — Kind::Alpha   (number return)
//
// The scalar-broadcast branch in the Vec3 loop implements WE's documented
// semantic: `return value.x + k` from a `scale` script is treated as uniform
// scale applied to all three components.  Dropping the scalar (as the
// original implementation did) meant Lucy's hover-zoom Clock scale script,
// and every other "return modified single-axis value" script, silently
// did nothing.

namespace wek::qml_helper {

inline constexpr const char* kPropertyScriptDispatchJs =
    "var _allPropertyScripts = [];\n"
    "var _scriptErrors = [];\n"
    "var _scriptOut = [];\n"
    "var _scriptPartVisEnd = 0;\n"
    "var _scriptPartVec3End = 0;\n"
    "function _runAllPropertyScripts() {\n"
    "  var out = _scriptOut;\n"
    "  out.length = 0;\n"
    "  _scriptErrors.length = 0;\n"
    "  var scripts = _allPropertyScripts;\n"
    "  var N = scripts.length;\n"
    "  var ve = _scriptPartVisEnd;\n"
    "  var xe = _scriptPartVec3End;\n"
    "  var i, s, r;\n"
    // The `var fn = s.fn; fn(arg)` dance is deliberate: calling `s.fn(arg)`
    // directly would bind `this = s` inside the fn, but the original C++
    // `updateFn.call({arg})` path bound `this = undefined`.  Extracting
    // the function into a local first matches that semantics, and avoids
    // polluting our entry objects if scripts do `this.foo = bar` on the
    // side.
    "  var fn;\n"
    "  for (i = 0; i < ve; i++) {\n"
    "    s = scripts[i];\n"
    "    if (!s.valid) continue;\n"
    "    if (s.hasLayer) thisLayer = s.proxy;\n"
    // thisObject rebinds per attachment site.  For Object-attached scripts
    // it equals thisLayer; for AnimationLayer-attached scripts it resolves
    // to the specific animation-layer proxy (Lucy puppet init scripts rely
    // on thisObject.setFrame / frameCount reaching the rig layer, not the
    // parent image).  `s.obj` is always set when `hasLayer` is true.
    "    if (s.obj) thisObject = s.obj;\n"
    "    fn = s.fn;\n"
    "    try { r = fn(s.cb); }\n"
    "    catch (e) {\n"
    "      _scriptErrors.push(i, String((e && e.message) || e),\n"
    "        (e && e.lineNumber) || 0, String((e && e.stack) || ''));\n"
    "      continue;\n"
    "    }\n"
    "    if (r === undefined || r === null || typeof r !== 'boolean') continue;\n"
    "    if (r === s.cb) continue;\n"
    "    s.cb = r;\n"
    "    out.push(i, r ? 1 : 0, 0, 0);\n"
    "  }\n"
    "  for (i = ve; i < xe; i++) {\n"
    "    s = scripts[i];\n"
    "    if (!s.valid) continue;\n"
    "    if (s.hasLayer) thisLayer = s.proxy;\n"
    // thisObject rebinds per attachment site.  For Object-attached scripts
    // it equals thisLayer; for AnimationLayer-attached scripts it resolves
    // to the specific animation-layer proxy (Lucy puppet init scripts rely
    // on thisObject.setFrame / frameCount reaching the rig layer, not the
    // parent image).  `s.obj` is always set when `hasLayer` is true.
    "    if (s.obj) thisObject = s.obj;\n"
    "    fn = s.fn;\n"
    "    try { r = fn(Vec3(s.cx, s.cy, s.cz)); }\n"
    "    catch (e) {\n"
    "      _scriptErrors.push(i, String((e && e.message) || e),\n"
    "        (e && e.lineNumber) || 0, String((e && e.stack) || ''));\n"
    "      continue;\n"
    "    }\n"
    "    if (r === undefined || r === null) continue;\n"
    // WE semantic: returning a scalar broadcasts to all three components
    // (used by hover-zoom style scripts that do `return value.x + k`).
    "    if (typeof r === 'number') {\n"
    "      if (Math.abs(r - s.cx) < 0.0001 &&\n"
    "          Math.abs(r - s.cy) < 0.0001 &&\n"
    "          Math.abs(r - s.cz) < 0.0001) continue;\n"
    "      s.cx = r; s.cy = r; s.cz = r;\n"
    "      out.push(i, r, r, r);\n"
    "      continue;\n"
    "    }\n"
    "    if (typeof r.x !== 'number') continue;\n"
    "    if (Math.abs(r.x - s.cx) < 0.0001 &&\n"
    "        Math.abs(r.y - s.cy) < 0.0001 &&\n"
    "        Math.abs(r.z - s.cz) < 0.0001) continue;\n"
    "    s.cx = r.x; s.cy = r.y; s.cz = r.z;\n"
    "    out.push(i, r.x, r.y, r.z);\n"
    "  }\n"
    "  for (i = xe; i < N; i++) {\n"
    "    s = scripts[i];\n"
    "    if (!s.valid) continue;\n"
    "    if (s.hasLayer) thisLayer = s.proxy;\n"
    // thisObject rebinds per attachment site.  For Object-attached scripts
    // it equals thisLayer; for AnimationLayer-attached scripts it resolves
    // to the specific animation-layer proxy (Lucy puppet init scripts rely
    // on thisObject.setFrame / frameCount reaching the rig layer, not the
    // parent image).  `s.obj` is always set when `hasLayer` is true.
    "    if (s.obj) thisObject = s.obj;\n"
    "    fn = s.fn;\n"
    "    try { r = fn(s.cf); }\n"
    "    catch (e) {\n"
    "      _scriptErrors.push(i, String((e && e.message) || e),\n"
    "        (e && e.lineNumber) || 0, String((e && e.stack) || ''));\n"
    "      continue;\n"
    "    }\n"
    "    if (r === undefined || r === null || typeof r !== 'number') continue;\n"
    "    if (Math.abs(r - s.cf) < 0.001) continue;\n"
    "    s.cf = r;\n"
    "    out.push(i, r, 0, 0);\n"
    "  }\n"
    "  return out;\n"
    "}\n";

// Per-IIFE shadow of `createScriptProperties` — the builder used inside a
// single script's wrapper to seed property values from scene.json's stored
// `scriptproperties` block (and its `user:` user-property bindings).
//
// Exposes each defined property via a getter/setter pair rather than a
// plain value.  Writes fire the optional `onChange` callback.  Same-value
// writes are suppressed so mutual-exclusion patterns (Lucy Clock date
// formats: setting one checkbox's onChange writes `false` to its siblings)
// don't infinite-recurse.
//
// The `%1` placeholder is the JSON-encoded stored-props map.  Use with
// `QString(kCreateScriptPropertiesShadowJs).arg(jsonStr)`.
inline constexpr const char* kCreateScriptPropertiesShadowJs =
    "var _storedProps = JSON.parse('%1');\n"
    "function createScriptProperties() {\n"
    "  var _values = {};\n"
    "  var _onChange = {};\n"
    "  var b = {};\n"
    "  function ap(def) {\n"
    "    if (!def) return b;\n"
    "    var n = def.name || def.n;\n"
    "    if (!n) return b;\n"
    "    var initial;\n"
    "    if (n in _storedProps) {\n"
    "      var sp = _storedProps[n];\n"
    // WE scene.json lets a scriptProperty bind to a user property via
    // `user: '<name>'`.  Resolve from engine.userProperties first, fall
    // back to the stored `value`, then the script's own default.
    // Wallpaper 2866203962's Player Options binds `enableplayer` to
    // user prop `ui`; without the user-resolve step the UI always faded.
    "      if (typeof sp === 'object' && sp !== null) {\n"
    "        if ('user' in sp && typeof engine !== 'undefined' &&\n"
    "            engine.userProperties && sp.user in engine.userProperties) {\n"
    "          initial = engine.userProperties[sp.user];\n"
    "        } else if ('value' in sp) {\n"
    "          initial = sp.value;\n"
    "        } else {\n"
    "          initial = def.value;\n"
    "        }\n"
    "      } else {\n"
    "        initial = sp;\n"
    "      }\n"
    "    } else { initial = def.value; }\n"
    "    _values[n] = initial;\n"
    "    if (def.onChange && typeof def.onChange === 'function') {\n"
    "      _onChange[n] = def.onChange;\n"
    "    }\n"
    "    if (!Object.getOwnPropertyDescriptor(b, n)) {\n"
    "      Object.defineProperty(b, n, {\n"
    "        get: function() { return _values[n]; },\n"
    "        set: function(v) {\n"
    "          if (_values[n] === v) return;\n"
    "          _values[n] = v;\n"
    "          var h = _onChange[n];\n"
    "          if (h) {\n"
    "            try { h.call(b, v); }\n"
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
    "    return b;\n"
    "  }\n"
    "  b.addCheckbox=ap; b.addSlider=ap; b.addCombo=ap;\n"
    "  b.addText=ap; b.addTextInput=ap;\n"
    "  b.addColor=ap; b.addFile=ap; b.addDirectory=ap;\n"
    "  b.finish=function(){return b;};\n"
    "  return b;\n"
    "}\n";

} // namespace wek::qml_helper
